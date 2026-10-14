//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "googlesql/ffi/googlesql_parser.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using ::testing::HasSubstr;

// Owns the result for the duration of a test.
class Diagnostics {
 public:
  explicit Diagnostics(absl::string_view sql) {
    count_ = gsql_check_syntax(sql.data(), sql.size(), &errors_);
  }
  ~Diagnostics() { gsql_syntax_errors_free(errors_, count_); }

  Diagnostics(const Diagnostics&) = delete;
  Diagnostics& operator=(const Diagnostics&) = delete;

  int count() const { return count_; }
  const gsql_syntax_error& operator[](int i) const { return errors_[i]; }

 private:
  gsql_syntax_error* errors_ = nullptr;
  int count_ = 0;
};

TEST(GoogleSqlParserFfi, ValidStatementHasNoErrors) {
  const Diagnostics d("SELECT 1");
  EXPECT_EQ(d.count(), 0);
}

TEST(GoogleSqlParserFfi, ReportsLocationOfSyntaxError) {
  constexpr absl::string_view kSql = "SELECT 1 FROM";
  const Diagnostics d(kSql);
  ASSERT_EQ(d.count(), 1);
  EXPECT_THAT(d[0].message, HasSubstr("Syntax error"));
  // The error is at end of input, one past the last character.
  EXPECT_EQ(d[0].start_byte, static_cast<int>(kSql.size()));
  // Nothing to underline there, so the range is empty.
  EXPECT_EQ(d[0].end_byte, d[0].start_byte);
  EXPECT_EQ(d[0].line, 1);
  // No location text in the message itself; that is what the fields are for.
  EXPECT_THAT(d[0].message, ::testing::Not(HasSubstr("[at ")));
}

// Every invalid statement below starts with the misspelled keyword SELEKT and
// nothing else in the input contains it, so the expected errors come from the
// input itself: one per SELEKT, located at its first byte. This also checks the
// recovery loop: a bad statement must not hide the ones after
// it, nor be blamed on a neighbour. In scripts each closed top-level block
// holds at most one SELEKT, since a failed block is skipped whole; an unclosed
// one is skipped from the error to the next semicolon instead.
TEST(GoogleSqlParserFfi, StatementMatrix) {
  for (const absl::string_view sql : {
           // No statements.
           "",
           "-- only a comment",
           "# only a comment\n",
           "/* only a comment */",
           "/*\n multi-line\n comment\n*/\n",
           "-- a\n# b\n/* c */\n",

           // One statement, with and without a terminator.
           "SELECT 1",
           "SELECT 1;",
           "SELEKT 1",
           "SELEKT 1;",

           // Two statements, every order.
           "SELECT 1; SELECT a FROM t;",
           "SELECT 1; SELEKT 2;",
           "SELEKT 1; SELECT 2;",
           "SELEKT 1; SELEKT 2;",
           "SELECT 1; SELEKT 2",  // Unterminated last statement.

           // Three statements, every order.
           "SELECT 1; SELECT 2; SELECT 3;",
           "SELECT 1; SELECT 2; SELEKT 3;",
           "SELECT 1; SELEKT 2; SELECT 3;",
           "SELECT 1; SELEKT 2; SELEKT 3;",
           "SELEKT 1; SELECT 2; SELECT 3;",
           "SELEKT 1; SELECT 2; SELEKT 3;",
           "SELEKT 1; SELEKT 2; SELECT 3;",
           "SELEKT 1; SELEKT 2; SELEKT 3;",

           // Scripts: a block is one statement.
           "DECLARE x INT64; SET x = 1; SELEKT x;",
           "BEGIN SELECT 1; END; SELEKT 2;",
           "IF TRUE THEN SELECT 1; END IF; SELEKT 2;",
           "WHILE FALSE DO SELECT 1; END WHILE;",
           "SELEKT 1; BEGIN SELECT 2; END;",

           // An error inside a block skips the whole block: one error, and
           // nothing reported for the rest of it or its closing END.
           "BEGIN\n  SELEKT 1;\nEND;",
           "BEGIN SELEKT 1; SELECT 2; END; SELEKT 3;",
           "BEGIN BEGIN SELEKT 1; END; SELECT 2; END; SELEKT 3;",
           "IF a THEN SELEKT 1; ELSE SELECT 2; END IF; SELEKT 3;",
           "IF a THEN SELECT 1; ELSEIF b THEN SELEKT 2; ELSE SELECT 3; END IF;",
           "IF a THEN IF b THEN SELEKT 1; END IF; SELECT 2; END IF; SELEKT 3;",
           "BEGIN SELEKT 1; EXCEPTION WHEN ERROR THEN SELECT 2; END;",
           "CASE x WHEN 1 THEN SELEKT 1; ELSE SELECT 2; END CASE; SELEKT 3;",
           "WHILE a DO SELEKT 1; SELECT 2; END WHILE; SELEKT 3;",
           "LOOP SELEKT 1; BREAK; END LOOP; SELEKT 2;",
           "REPEAT SELEKT 1; UNTIL a END REPEAT; SELEKT 2;",
           "FOR r IN (SELECT 1) DO SELEKT 1; END FOR; SELEKT 2;",
           "lbl: BEGIN SELEKT 1; END lbl; SELEKT 2;",
           "CREATE PROCEDURE p() BEGIN SELEKT 1; SELECT 2; END; SELEKT 3;",
           "CREATE PROCEDURE p() OPTIONS (strict_mode = FALSE) BEGIN SELEKT 1; END; SELEKT 2;",
           "DROP PROCEDURE p; BEGIN; SELEKT 1; COMMIT;",
           // Keywords are case-insensitive.
           "begin SELEKT 1; end; SELEKT 2;",
           "if a then SELEKT 1; end if; SELEKT 2;",
           // Keywords that open no block, inside a block that does.
           "BEGIN SELECT IF(a, 1, 2); SELEKT 1; END; SELEKT 2;",
           "BEGIN DROP TABLE IF EXISTS t; SELEKT 1; END; SELEKT 2;",
           "BEGIN CREATE TABLE IF NOT EXISTS t (a INT64); SELEKT 1; END;",
           "BEGIN SELECT * FROM t FOR SYSTEM_TIME AS OF CURRENT_TIMESTAMP(); "
           "SELEKT 1; END; SELEKT 2;",
           "BEGIN SELECT * FROM t PIVOT(SUM(a) FOR b IN (1)); SELEKT 1; END;",
           "BEGIN SELECT CASE WHEN a THEN 1 ELSE 2 END; SELEKT 1; END; "
           "SELEKT 2;",
           "BEGIN SELECT CASE WHEN a THEN IF(b, 1, 2) END; SELEKT 1; END;",
           "BEGIN SELECT REPEAT('a', 2); SELEKT 1; END; SELEKT 2;",
           "BEGIN lbl: LOOP SELEKT 1; END LOOP lbl; END; SELEKT 2;",
           // DO as a name, not the start of a loop body.
           "BEGIN SELEKT 1; SELECT * FROM t AS do FOR SYSTEM_TIME AS OF x; END; SELEKT 2;",
           "BEGIN SELEKT 1; SELECT * FROM t do FOR SYSTEM_TIME AS OF x; END; SELEKT 2;",
           // Transactions are not blocks.
           "BEGIN; SELEKT 1; COMMIT; SELEKT 2;",
           "BEGIN SELEKT 1; BEGIN READ ONLY; SELECT 2; COMMIT; END; SELEKT 3;",
           "BEGIN SELEKT 1; BEGIN ISOLATION LEVEL SERIALIZABLE; COMMIT; END; SELEKT 2;",
           "BEGIN TRANSACTION; SELEKT 1; COMMIT; SELEKT 2;",
           // Comments between the tokens that make up the block structure.
           "IF a THEN SELEKT 1; END /* x */ IF; SELEKT 2;",
           "BEGIN lbl /* x */ : LOOP SELEKT 1; END LOOP lbl; END; SELEKT 2;",
           "BEGIN SELEKT 1; -- c\nSELECT 2; END; SELEKT 3;",
           "BEGIN /* x */ TRANSACTION; SELEKT 1; COMMIT; SELEKT 2;",
           // Keywords used as names.
           "BEGIN SELECT t.end FROM t; SELEKT 1; END; SELEKT 2;",
           "BEGIN SELECT t.if FROM t; SELEKT 1; END; SELEKT 2;",
           "BEGIN loop: LOOP SELEKT 1; END LOOP loop; END; SELEKT 2;",
           // Labels the tokenizer reads as exponents.
           "e: BEGIN SELEKT 1; END; SELEKT 2;",
           "e1: BEGIN SELEKT 1; END; SELEKT 2;",
           // A block with no END yet, as while typing: skipped to the next
           // semicolon instead, so the statements after it are still checked.
           "BEGIN\n  SELEKT 1;\nSELECT 2;\nSELEKT 3;",
           "IF a THEN SELEKT 1; SELECT 2; SELEKT 3;",
           // An error after the first semicolon is still reported once.
           "BEGIN SELECT 1; SELEKT 2;",
           "IF a THEN SELECT 1; SELEKT 2; SELECT 3; SELEKT 4;",

           // Comments before, inside, before ';', after ';', between and after
           // statements, in each style.
           "-- lead\nSELECT 1;",
           "-- lead\nSELEKT 1;",
           "# lead\nSELEKT 1;",
           "/* lead */SELEKT 1;",
           "SELECT /* in */ 1 -- in\n FROM t;",
           "SELEKT /* in */ 1 -- in\n FROM t;",
           "SELECT 1 /* before ; */;",
           "SELEKT 1 /* before ; */;",
           "SELECT 1; -- after ;\nSELEKT 2;",
           "SELEKT 1; # after ;\nSELECT 2;",
           "SELEKT 1;/* between */SELEKT 2;",
           "SELEKT 1; -- trailing, no newline",
           "SELEKT 1; # trailing\n",
           "SELECT 1 /* trailing, no terminator */",
           "SELEKT 1 -- trailing, no terminator",
           // A ';' inside a comment is not a statement boundary.
           "SELEKT 1 /* ; */ 2; SELECT 3;",
           "SELEKT 1 -- ;\n 2; SELEKT 3;",
           "SELECT 1 /* ; */; SELEKT 2;",

           // Multi-line statements and comments.
           "SELECT\n  a\nFROM\n  t;",
           "SELEKT\n  a\nFROM\n  t;",
           "SELECT 1;\n\nSELEKT\n  2;\nSELECT\n  3;",
           "/*\n * header\n */\nSELEKT 1;\n-- done\n",
           "SELECT a\n-- ;\nFROM t;\nSELEKT b\n/* ;\n */\nFROM t;\nSELEKT c;",
       }) {
    SCOPED_TRACE(sql);
    std::vector<int> expected;
    for (size_t at = sql.find("SELEKT"); at != sql.npos; at = sql.find("SELEKT", at + 1)) {
      expected.push_back(static_cast<int>(at));
    }

    const Diagnostics d(sql);
    ASSERT_EQ(d.count(), static_cast<int>(expected.size()));
    for (int i = 0; i < d.count(); ++i) {
      const int at = expected[i];
      EXPECT_EQ(d[i].start_byte, at) << "error " << i;
      // The range covers the offending token, SELEKT.
      EXPECT_EQ(d[i].end_byte, at + 6) << "error " << i;
      EXPECT_EQ(d[i].line, 1 + std::count(sql.begin(), sql.begin() + at, '\n')) << "error " << i;
      EXPECT_THAT(d[i].message, HasSubstr("SELEKT")) << "error " << i;
    }
  }
}

// An editor opens empty and comment-only buffers constantly; neither is an
// error, though the parser on its own calls both "Unexpected end of statement".
TEST(GoogleSqlParserFfi, BlankInputHasNoErrors) {
  for (const absl::string_view sql :
       {"", "   ", "\n\t ", "-- todo\n", "# todo", "/* a */\n\n", "SELECT 1;\n-- trailing\n",
        // An ideographic space: whitespace to the tokenizer, which is why the
        // blank check defers to it rather than matching ASCII spaces itself.
        "\xe3\x80\x80"}) {
    const Diagnostics d(sql);
    EXPECT_EQ(d.count(), 0) << "input: " << sql;
  }
}

TEST(GoogleSqlParserFfi, ScriptsHaveNoErrors) {
  for (const absl::string_view sql : {
           "BEGIN SELECT 1; END;",
           "DECLARE x INT64 DEFAULT 1; SET x = x + 1;",
           "IF x = 1 THEN SELECT 1; ELSEIF x = 2 THEN SELECT 2; "
           "ELSE SELECT 3; END IF;",
           "IF a THEN IF b THEN SELECT 1; END IF; END IF;",
           "LOOP SELECT 1; BREAK; END LOOP;",
           "WHILE x < 3 DO SET x = x + 1; END WHILE;",
           "FOR r IN (SELECT 1 AS a) DO SELECT r.a; END FOR;",
           "BEGIN SELECT 1; EXCEPTION WHEN ERROR THEN SELECT 2; END;",
           "CASE x WHEN 1 THEN SELECT 1; ELSE SELECT 2; END CASE;",
       }) {
    const Diagnostics d(sql);
    EXPECT_EQ(d.count(), 0) << "input: " << sql;
  }
}

// A block that looks unclosed (a missing END IF, or a malformed opener inside
// it) falls back to skipping from the error to the next semicolon, so the
// block's own END is then reported as a follow-up error. Pinned so a change to
// the block heuristic cannot silently add more of them.
TEST(GoogleSqlParserFfi, UnclosedInnerBlockReportsFollowUpEnd) {
  for (const absl::string_view sql : {
           "BEGIN IF a THEN SELEKT 1; END; SELEKT 2;",
           "BEGIN IF; SELEKT 1; END; SELEKT 2;",
       }) {
    SCOPED_TRACE(sql);
    const Diagnostics d(sql);
    const size_t end = sql.find("END;");
    const size_t last = sql.rfind("SELEKT");
    std::vector<int> starts;
    for (int i = 0; i < d.count(); ++i) starts.push_back(d[i].start_byte);
    EXPECT_THAT(starts, ::testing::Contains(static_cast<int>(end)));
    EXPECT_EQ(starts.back(), static_cast<int>(last));
    EXPECT_EQ(d.count(), sql.find("IF;") == sql.npos ? 3 : 4);
  }
}

// `;` cannot appear inside an expression, so it ends an unfinished CASE, and the
// block's END still closes the block.
TEST(GoogleSqlParserFfi, SemicolonEndsUnfinishedCase) {
  const absl::string_view sql = "BEGIN SELECT CASE WHEN a THEN 1; SELEKT 2; END; SELEKT 3;";
  const Diagnostics d(sql);
  ASSERT_EQ(d.count(), 2);
  EXPECT_EQ(d[1].start_byte, static_cast<int>(sql.rfind("SELEKT")));
}

// A statement that closes is skipped even if the text after it cannot be
// tokenized, as while typing a comment; that text is then reported on its own.
TEST(GoogleSqlParserFfi, UntokenizableTextAfterSkippedStatement) {
  for (const absl::string_view sql : {
           "SELEKT 1;\n/* typing",
           "BEGIN SELEKT 1; END;\n/* typing",
           // Unclosed, so skipped to the next semicolon instead.
           "BEGIN SELEKT 1;\n/* typing",
       }) {
    SCOPED_TRACE(sql);
    const Diagnostics d(sql);
    ASSERT_EQ(d.count(), 2);
    EXPECT_EQ(d[0].start_byte, static_cast<int>(sql.find("SELEKT")));
    EXPECT_EQ(d[1].start_byte, static_cast<int>(sql.find("/*")));
  }
}

// A block closed by end of input, with no `;` after its END yet, must not be
// taken for one that never closes because an earlier skip scanned past it. The
// first block's END is a follow-up error (see above), the second is skipped whole.
TEST(GoogleSqlParserFfi, BlockClosedAtEndOfInput) {
  const absl::string_view sql = "BEGIN BEGIN SELEKT 1; END; BEGIN SELEKT 2; END";
  const Diagnostics d(sql);
  ASSERT_EQ(d.count(), 3);
  EXPECT_EQ(d[2].start_byte, static_cast<int>(sql.rfind("SELEKT")));
}

// Many unclosed blocks, as while typing: each is skipped from its error, and
// every error is still reported once. Used to take seconds (a scan to end of
// input per error).
TEST(GoogleSqlParserFfi, ManyUnclosedBlocks) {
  std::string sql;
  for (int i = 0; i < 2000; ++i) sql += "IF a THEN SELEKT 1;\n";
  const Diagnostics d(sql);
  EXPECT_EQ(d.count(), 2000);
}

TEST(GoogleSqlParserFfi, NullBufferIsNotDereferenced) {
  gsql_syntax_error* errors = nullptr;
  EXPECT_EQ(gsql_check_syntax(nullptr, 10, &errors), 0);
  EXPECT_EQ(errors, nullptr);
  gsql_syntax_errors_free(errors, 0);
}

TEST(GoogleSqlParserFfi, ZeroLengthIsEmptyInput) {
  gsql_syntax_error* errors = nullptr;
  // The buffer is not blank, but none of it is within the length.
  EXPECT_EQ(gsql_check_syntax("SELECT", 0, &errors), 0);
  EXPECT_EQ(errors, nullptr);
}

TEST(GoogleSqlParserFfi, RejectsNullOutParameter) {
  EXPECT_EQ(gsql_check_syntax("SELEKT", 6, nullptr), -1);
}

// A non-NULL *errors may be a live array from an earlier call; overwriting it
// would leak it.
TEST(GoogleSqlParserFfi, RejectsOutParameterNotPointingAtNull) {
  gsql_syntax_error sentinel = {};
  gsql_syntax_error* errors = &sentinel;
  EXPECT_EQ(gsql_check_syntax("SELEKT", 6, &errors), -1);
  EXPECT_EQ(errors, &sentinel);
}

TEST(GoogleSqlParserFfi, RejectsTooLongInput) {
  gsql_syntax_error* errors = nullptr;
  // Too long for the parser's int offsets; must be rejected before reading.
  EXPECT_EQ(gsql_check_syntax("SELECT", size_t{1} << 31, &errors), -1);
  EXPECT_EQ(errors, nullptr);
  gsql_syntax_errors_free(nullptr, 3);
}

// sql_len is the length precisely so the caller can hand over a slice of an
// editor buffer, which is not NUL-terminated.
TEST(GoogleSqlParserFfi, HonorsLengthOverNulTermination) {
  // The trailing 'X' is past the length, so it must not be parsed.
  const Diagnostics d(absl::string_view("SELECT 1 FROMX", 13));
  ASSERT_EQ(d.count(), 1);
  // Reported at the end of the 13 bytes we passed, not of the 14 in the buffer.
  EXPECT_EQ(d[0].start_byte, 13);
}

// An unterminated string swallows the rest of the input: the scan stops there,
// but still reports the error it found on the way.
TEST(GoogleSqlParserFfi, ReportsErrorOnLaterLine) {
  const Diagnostics d("SELECT 1;\nSELECT * FROM t WHERE x = 'oops");
  ASSERT_EQ(d.count(), 1);
  EXPECT_EQ(d[0].line, 2);
}

}  // namespace
