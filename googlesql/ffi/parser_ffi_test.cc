#include "googlesql/ffi/parser_ffi.h"

#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"

namespace {

struct Diag {
  int start;
  int end;
  std::string message;
};

std::vector<Diag> Parse(absl::string_view sql) {
  gsql_diagnostics out;
  EXPECT_EQ(gsql_parse_script(sql.data(), sql.size(), &out), 0);
  std::vector<Diag> result;
  for (size_t i = 0; i < out.count; ++i) {
    result.push_back({static_cast<int>(out.items[i].start),
                      static_cast<int>(out.items[i].end),
                      out.items[i].message});
  }
  gsql_diagnostics_free(&out);
  return result;
}

// [start, end) of the n-th (0-based) occurrence of `needle` in `sql`.
std::pair<int, int> At(absl::string_view sql, absl::string_view needle,
                       int n = 0) {
  size_t pos = sql.find(needle);
  for (int i = 0; i < n && pos != absl::string_view::npos; ++i) {
    pos = sql.find(needle, pos + 1);
  }
  EXPECT_NE(pos, absl::string_view::npos) << "missing: " << needle;
  return {static_cast<int>(pos), static_cast<int>(pos + needle.size())};
}

std::pair<int, int> Range(const Diag& d) { return {d.start, d.end}; }

constexpr absl::string_view kCleanScript = R"sql(
-- leading comment
DECLARE x INT64 DEFAULT 0;
SET x = x + 1;
IF x > 0 THEN SELECT 1; ELSEIF x < 0 THEN SELECT 2; ELSE SELECT 3; END IF;
LOOP SET x = x + 1; IF x > 10 THEN LEAVE; END IF; END LOOP;
WHILE x > 0 DO SET x = x - 1; END WHILE;
REPEAT SET x = x + 1; UNTIL x > 3 END REPEAT;
FOR r IN (SELECT 1 AS a) DO SELECT r.a; END FOR;
CASE x WHEN 1 THEN SELECT 1; ELSE SELECT 2; END CASE;
BEGIN SELECT 1; EXCEPTION WHEN ERROR THEN SELECT @@error.message; END;
lbl: BEGIN SELECT 1; END lbl;
CREATE TEMP FUNCTION f(a INT64) AS (a + 1);
CREATE PROCEDURE ds.p(a INT64) BEGIN SELECT a; END;
SELECT IF(x > 0, 'a', 'b'), CASE WHEN x = 1 THEN 'one' END AS c FROM t
QUALIFY ROW_NUMBER() OVER (ORDER BY c) = 1;
BEGIN TRANSACTION; COMMIT TRANSACTION;
FROM t |> WHERE true |> SELECT *;
)sql";

TEST(ParserFfiTest, CleanInputsHaveNoDiagnostics) {
  for (absl::string_view sql :
       {absl::string_view(""), absl::string_view("  \n\t"),
        absl::string_view("-- only a comment\n/* and another */"),
        absl::string_view("SELECT 1"),
        absl::string_view("SELECT 1;\nSELECT 2;"), kCleanScript}) {
    EXPECT_TRUE(Parse(sql).empty()) << sql;
  }
}

TEST(ParserFfiTest, ErrorRangeIsOffendingToken) {
  constexpr absl::string_view sql = "SELECT 1 2;";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 1);
  EXPECT_EQ(Range(diags[0]), At(sql, "2"));
  EXPECT_TRUE(absl::StartsWith(diags[0].message, "Syntax error")) << diags[0].message;
  EXPECT_FALSE(absl::StrContains(diags[0].message, "[at ")) << diags[0].message;
}

TEST(ParserFfiTest, ErrorAfterMultibyteAndTabs) {
  constexpr absl::string_view sql = "SELECT 'äö',\t'ü';\n\tSELECT 1 2;";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 1);
  EXPECT_EQ(Range(diags[0]), At(sql, "2"));
}

TEST(ParserFfiTest, ErrorAfterCrlf) {
  constexpr absl::string_view sql = "SELECT 1;\r\nSELECT 3 4;";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 1);
  EXPECT_EQ(Range(diags[0]), At(sql, "4"));
}

TEST(ParserFfiTest, ErrorAfterLoneCr) {
  constexpr absl::string_view sql = "SELECT 1;\rSELECT 3 4;";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 1);
  EXPECT_EQ(Range(diags[0]), At(sql, "4"));
}

TEST(ParserFfiTest, ErrorAtEofIsZeroWidth) {
  constexpr absl::string_view sql = "SELECT 1 +";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 1);
  EXPECT_EQ(Range(diags[0]),
            std::make_pair(static_cast<int>(sql.size()),
                           static_cast<int>(sql.size())));
}

TEST(ParserFfiTest, UnterminatedStringRangeRunsToEndOfLine) {
  constexpr absl::string_view sql = "SELECT 'abc\nSELECT 2;";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 1);
  EXPECT_EQ(Range(diags[0]), At(sql, "'abc"));
}

TEST(ParserFfiTest, UnterminatedStringRangeStopsBeforeCrlf) {
  constexpr absl::string_view sql = "SELECT 'abc\r\nSELECT 2;";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 1);
  EXPECT_EQ(Range(diags[0]), At(sql, "'abc"));
}

TEST(ParserFfiTest, InputNeedNotBeNulTerminated) {
  const std::string buffer = "SELECT 1;garbage(";
  gsql_diagnostics out;
  ASSERT_EQ(gsql_parse_script(buffer.data(), 9, &out), 0);  // "SELECT 1;"
  EXPECT_EQ(out.count, 0);
  gsql_diagnostics_free(&out);
}

TEST(ParserFfiTest, NullAndEmptyInputs) {
  gsql_diagnostics out;
  ASSERT_EQ(gsql_parse_script(nullptr, 0, &out), 0);
  EXPECT_EQ(out.count, 0);
  EXPECT_EQ(out.items, nullptr);
  EXPECT_NE(gsql_parse_script(nullptr, 5, &out), 0);
  EXPECT_EQ(out.count, 0);
  EXPECT_NE(gsql_parse_script("SELECT 1", 8, nullptr), 0);
}

TEST(ParserFfiTest, FreeIsIdempotent) {
  gsql_diagnostics out = {nullptr, 0};
  gsql_diagnostics_free(&out);
  gsql_diagnostics_free(&out);
  gsql_diagnostics_free(nullptr);
  ASSERT_EQ(gsql_parse_script("SELECT 1 2", 10, &out), 0);
  gsql_diagnostics_free(&out);
  EXPECT_EQ(out.items, nullptr);
  EXPECT_EQ(out.count, 0);
  gsql_diagnostics_free(&out);
}

TEST(ParserFfiTest, ReportsEveryTopLevelError) {
  constexpr absl::string_view sql = "SELECT 1 2;\nSELECT 3;\nSELECT 4 5;";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 2);
  EXPECT_EQ(Range(diags[0]), At(sql, "2"));
  EXPECT_EQ(Range(diags[1]), At(sql, "5"));
}

TEST(ParserFfiTest, ErrorInsideBlockSkipsWholeBlock) {
  constexpr absl::string_view sql =
      "BEGIN\n  SELECT 1 2;\n  SELECT 3;\nEND;\nSELECT 4;";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 1);
  EXPECT_EQ(Range(diags[0]), At(sql, "2"));
}

TEST(ParserFfiTest, ErrorAtEofAfterValidStatements) {
  constexpr absl::string_view sql = "SELECT 1;\nSELECT 2 +";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 1);
  EXPECT_EQ(Range(diags[0]),
            std::make_pair(static_cast<int>(sql.size()),
                           static_cast<int>(sql.size())));
}

// Accepted limit: a missing END makes recovery run to EOF.
TEST(ParserFfiTest, MissingEndReportsOnceAndTerminates) {
  constexpr absl::string_view sql = "BEGIN\n  SELECT 1 2;\nSELECT 9 8;";
  EXPECT_EQ(Parse(sql).size(), 1);
}

TEST(ParserFfiTest, DiagnosticsAreSortedAndDisjoint) {
  constexpr absl::string_view sql =
      "SELECT 1 2;\nIF true THEN SELECT 3 4; END IF;\nSELECT 5;\nSELECT 6 +";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 3);
  for (size_t i = 1; i < diags.size(); ++i) {
    EXPECT_LE(diags[i - 1].end, diags[i].start);
  }
}

TEST(ParserFfiTest, UnclosedCaseExprDoesNotFlagValidCode) {
  constexpr absl::string_view sql =
      "SELECT CASE WHEN a THEN 1 FROM t;\nWHILE x DO\n  IF y THEN SELECT 1; END IF;\nEND WHILE;\nSELECT 9 8;";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 2);
  EXPECT_EQ(Range(diags[1]), At(sql, "8"));
}

// Each trap is a broken statement (or block) followed by a second broken
// statement. If the skipper over-skips, the second error ("8") is lost.
class SkipperTrapTest : public ::testing::TestWithParam<absl::string_view> {};

TEST_P(SkipperTrapTest, SecondErrorIsStillReported) {
  const std::string sql = std::string(GetParam()) + "\nSELECT 9 8;";
  std::vector<Diag> diags = Parse(sql);
  ASSERT_EQ(diags.size(), 2) << sql;
  EXPECT_LT(diags[0].start, diags[1].start);
  EXPECT_EQ(Range(diags[1]), At(sql, "8")) << sql;
}

INSTANTIATE_TEST_SUITE_P(
    Traps, SkipperTrapTest,
    ::testing::Values(
        "SELECT IF(true, 1, 2) );",
        "CREATE TABLE IF NOT EXISTS t (x INT64) );",
        "SELECT * FROM t FOR SYSTEM_TIME AS OF CURRENT_TIMESTAMP() );",
        "SELECT CASE WHEN true THEN IF(true, 1, 2) ELSE REPEAT('a', 2) END );",
        "SELECT CASE WHEN true THEN 1 FROM t;",
        "SELECT begin FROM t );",
        "BEGIN TRANSACTION );",
        "BEGIN\n  BEGIN TRANSACTION;\n  BEGIN;\n  SELECT 1 2;\n  COMMIT;\nEND;",
        "BEGIN BEGIN SELECT 1 2; END; END;",
        "END;",
        "lbl: LOOP\n  SELECT 1 2;\nEND LOOP lbl;",
        "IF (true) THEN\n  SELECT 1 2;\nEND IF;",
        "CREATE PROCEDURE p() BEGIN SELECT 1 2; END;",
        "CREATE PROCEDURE p() OPTIONS (strict_mode = false) BEGIN SELECT 1 2; END;",
        "CREATE PROCEDURE p() EXTERNAL SECURITY INVOKER BEGIN SELECT 1 2; SELECT 3; END;",
        "CREATE PROCEDURE p() WITH CONNECTION c BEGIN SELECT 1 2; SELECT 3; END;",
        "WHILE true DO SELECT 1 2; END WHILE;",
        "REPEAT SELECT 1 2; UNTIL true END REPEAT;",
        "FOR r IN (SELECT 1) DO SELECT 1 2; END FOR;",
        "CASE 1 WHEN 1 THEN SELECT 1 2; ELSE SELECT 3; END CASE;",
        "BEGIN SELECT 1 2; EXCEPTION WHEN ERROR THEN SELECT 3; END;",
        "IF true THEN SELECT CASE WHEN true THEN 1 END; SELECT 1 2; END IF;",
        "SELECT 1 2, ';', /* ; */ 3 -- ;\n;"));

}  // namespace
