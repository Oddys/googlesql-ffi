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
  EXPECT_EQ(d[0].line, 1);
  // No location text in the message itself; that is what the fields are for.
  EXPECT_THAT(d[0].message, ::testing::Not(HasSubstr("[at ")));
}

// Every invalid statement below starts with the misspelled keyword SELEKT and
// nothing else in the input contains it, so the expected errors come from the
// input itself: one per SELEKT, located at its first byte. This also checks the
// SkipNextStatement recovery loop: a bad statement must not hide the ones after
// it, nor be blamed on a neighbour.
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
    for (size_t at = sql.find("SELEKT"); at != sql.npos;
         at = sql.find("SELEKT", at + 1)) {
      expected.push_back(static_cast<int>(at));
    }

    const Diagnostics d(sql);
    ASSERT_EQ(d.count(), static_cast<int>(expected.size()));
    for (int i = 0; i < d.count(); ++i) {
      const int at = expected[i];
      EXPECT_EQ(d[i].start_byte, at) << "error " << i;
      EXPECT_EQ(d[i].line, 1 + std::count(sql.begin(), sql.begin() + at, '\n'))
          << "error " << i;
      EXPECT_THAT(d[i].message, HasSubstr("SELEKT")) << "error " << i;
    }
  }
}

// An editor opens empty and comment-only buffers constantly; neither is an
// error, though the parser on its own calls both "Unexpected end of statement".
TEST(GoogleSqlParserFfi, BlankInputHasNoErrors) {
  for (const absl::string_view sql :
       {"",
        "   ",
        "\n\t ",
        "-- todo\n",
        "# todo",
        "/* a */\n\n",
        "SELECT 1;\n-- trailing\n",
        // An ideographic space: whitespace to the tokenizer, which is why the
        // blank check defers to it rather than matching ASCII spaces itself.
        "\xe3\x80\x80"}) {
    const Diagnostics d(sql);
    EXPECT_EQ(d.count(), 0) << "input: " << sql;
  }
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
