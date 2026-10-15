# Parser FFI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a self-contained static library with a C ABI that returns every syntax error in a BigQuery script as a byte range and an error message, for a Zig LSP server.

**Architecture:** The C entry point `check_syntax` loops over `ParseNextScriptStatement`. On a syntax error it records the offending token's byte range, then skips to the end of the enclosing top-level statement with a block-aware token scanner (`helpers`), and resumes. `cc_static_library` merges every transitive dependency into a single `.a`.

**Tech Stack:** C++20, Bazel 7.6.1 (bzlmod), rules_cc 0.2.14, GoogleSQL parser and tokenizer, gtest.

**Spec:** `docs/superpowers/specs/2026-10-15-parser-ffi-design.md`

## Global Constraints

- The public header `googlesql/ffi/parser_ffi.h` is pure C (it must compile as C99), with `extern "C"` guards.
- Positions are byte offsets in a half-open range `[start, end)` into the caller's buffer. Never line/col.
- Language options: `EnableMaximumLanguageFeatures()` + `SetSupportsAllStatementKinds()`, hard-coded. Not configurable.
- Error messages are produced with `ERROR_MESSAGE_WITH_PAYLOAD` and carry no `[at l:c]` suffix.
- `check_syntax` returns non-zero only on internal failure, `len > INT32_MAX`, `out == NULL`, or `sql == NULL && len != 0`. On non-zero, `*out` is `{NULL, 0}` whenever `out != NULL`.
- No global state. Every call is independent.
- Syntax errors are sorted by `start` and never overlap. A syntax error never touches a statement that parsed OK.
- Build artifact: `bazel build //googlesql/ffi:googlesql_parser_ffi` produces `bazel-bin/googlesql/ffi/libgooglesql_parser_ffi.a`.
- Recovery granularity is the top-level script statement (option A). Inner-block recovery is out of scope.
- Targets: macOS arm64 + x86_64, Linux x86_64 + aarch64. Build natively on each (or `--cpu=darwin_x86_64` from Apple Silicon). Windows is out of scope: GoogleSQL does not build there. No `#ifdef` per OS in the library code.
- Line endings are a property of the input, not the host: `\n`, `\r\n` and lone `\r` all end a line, as in `ParseLocationTranslator::CalculateLineOffsets`.

## Review Focus

1. `BEGIN` used as an identifier (it's non-reserved), e.g. `SELECT begin FROM t )`. The skipper must not open a block and swallow the rest of the file. Pinned in Task 2 traps.
2. Inside an expression `CASE`, a `THEN IF(...)` or `ELSE REPEAT(...)` must not look like the start of a statement. Pinned in Task 2 traps.
3. CRLF line endings before an error. The line:col → byte mapping must still land on the exact token. Pinned in Task 1.
4. An error in the last statement with no trailing `;`, after valid statements, gives a zero-width range at EOF and the loop terminates. Pinned in Task 2.
5. A missing `END` (an accepted limit) must give exactly one syntax error and terminate, with no hang and no spurious syntax errors. Pinned in Task 2.

## File Structure

| File | Responsibility |
|---|---|
| `googlesql/ffi/parser_ffi.h` | Public C API (create) |
| `googlesql/ffi/parser_ffi.cc` | Parser options, recovery loop, C marshalling (create) |
| `googlesql/ffi/helpers.h/.cc` | Helpers: `TokenRangeAt`, `NothingSignificantFrom`, `SkipStatement` (create) |
| `googlesql/ffi/parser_ffi_test.cc` | gtest through the C API only (create) |
| `googlesql/ffi/link_smoke_test.c` | Pure-C program linked against the `.a` only (create, Task 3) |
| `googlesql/ffi/BUILD` | Targets (create) |
| `googlesql/ffi/README.md` | Build command + Zig link snippet (create, Task 3) |
| `.bazelrc` | Add `--experimental_cc_static_library` (modify, Task 3) |

---

### Task 1: C API, first-error reporting, token ranges

Delivers a working `check_syntax` that reports **at most the first** error, with exact token ranges. Recovery comes in Task 2.

**Files:**
- Create: `googlesql/ffi/parser_ffi.h`, `googlesql/ffi/parser_ffi.cc`, `googlesql/ffi/helpers.h`, `googlesql/ffi/helpers.cc`, `googlesql/ffi/BUILD`
- Test: `googlesql/ffi/parser_ffi_test.cc`

**Interfaces:**
- Produces (C): `syntax_error`, `findings`, `int check_syntax(const char*, size_t, findings*)`, `void free_findings(findings*)`.
- Produces (C++, namespace `googlesql::ffi`, header `googlesql/ffi/helpers.h`):
  - `std::pair<int, int> TokenRangeAt(absl::string_view sql, int offset);`
  - `bool NothingSignificantFrom(absl::string_view sql, int offset);`
- Produces (in `parser_ffi.cc`, anonymous namespace): `bool CollectSyntaxErrors(absl::string_view sql, std::vector<SyntaxError>& errors)`, where `struct SyntaxError { int start; int end; std::string message; };`. Task 2 edits its loop body.

- [ ] **Step 1: Write the header**

`googlesql/ffi/parser_ffi.h`:

```c
#ifndef GOOGLESQL_FFI_PARSER_FFI_H_
#define GOOGLESQL_FFI_PARSER_FFI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t start;       // byte offset, inclusive
  uint32_t end;         // byte offset, exclusive (== start at EOF)
  const char* message;  // NUL-terminated UTF-8, no location suffix
} syntax_error;

typedef struct {
  syntax_error* items;  // owned by the library
  size_t count;
} findings;

// Checks GoogleSQL syntax in sql[0..len); sql need not be NUL-terminated.
// Returns:
// - 0 and fills `out` (count == 0 means the input is clean).
// - non-zero on internal failure, len > INT32_MAX, out == NULL, or sql == NULL with len != 0;
//   `out` is then {NULL, 0} (if out != NULL).
// Syntax errors are sorted by start and never overlap.
// Thread-safe.
int check_syntax(const char* sql, size_t len, findings* out);

// Frees everything check_syntax allocated in *out and resets it to {NULL, 0}.
// Safe on an empty result and on NULL.
void free_findings(findings* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // GOOGLESQL_FFI_PARSER_FFI_H_
```

- [ ] **Step 2: Write the failing tests**

`googlesql/ffi/parser_ffi_test.cc`:

```cpp
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "googlesql/ffi/parser_ffi.h"
#include "gtest/gtest.h"

namespace {

struct SyntaxError {
  int start;
  int end;
  std::string message;
};

std::vector<SyntaxError> CheckSyntax(absl::string_view sql) {
  findings out;
  EXPECT_EQ(check_syntax(sql.data(), sql.size(), &out), 0);
  std::vector<SyntaxError> result;
  for (size_t i = 0; i < out.count; ++i) {
    result.push_back({
        static_cast<int>(out.items[i].start),
        static_cast<int>(out.items[i].end),
        out.items[i].message,
    });
  }
  free_findings(&out);
  return result;
}

// [start, end) of the n-th (0-based) occurrence of `needle` in `sql`.
std::pair<int, int> At(absl::string_view sql, absl::string_view needle, int n = 0) {
  size_t pos = sql.find(needle);
  for (int i = 0; i < n && pos != absl::string_view::npos; ++i) {
    pos = sql.find(needle, pos + 1);
  }
  EXPECT_NE(pos, absl::string_view::npos) << "missing: " << needle;
  return {static_cast<int>(pos), static_cast<int>(pos + needle.size())};
}

std::pair<int, int> Range(const SyntaxError& e) {
  return {e.start, e.end};
}

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

TEST(ParserFfiTest, CleanInputsHaveNoSyntaxErrors) {
  for (absl::string_view sql : {
           absl::string_view(""),
           absl::string_view("  \n\t"),
           absl::string_view("-- only a comment\n/* and another */"),
           absl::string_view("SELECT 1"),
           absl::string_view("SELECT 1;\nSELECT 2;"),
           kCleanScript,
       }) {
    EXPECT_TRUE(CheckSyntax(sql).empty()) << sql;
  }
}

TEST(ParserFfiTest, ErrorRangeIsOffendingToken) {
  constexpr absl::string_view sql = "SELECT 1 2;";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(Range(errors[0]), At(sql, "2"));
  EXPECT_TRUE(absl::StartsWith(errors[0].message, "Syntax error")) << errors[0].message;
  EXPECT_FALSE(absl::StrContains(errors[0].message, "[at ")) << errors[0].message;
}

TEST(ParserFfiTest, ErrorAfterMultibyteAndTabs) {
  constexpr absl::string_view sql = "SELECT 'äö',\t'ü';\n\tSELECT 1 2;";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(Range(errors[0]), At(sql, "2"));
}

TEST(ParserFfiTest, ErrorAfterCrlf) {
  constexpr absl::string_view sql = "SELECT 1;\r\nSELECT 3 4;";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(Range(errors[0]), At(sql, "4"));
}

TEST(ParserFfiTest, ErrorAfterLoneCr) {
  constexpr absl::string_view sql = "SELECT 1;\rSELECT 3 4;";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(Range(errors[0]), At(sql, "4"));
}

TEST(ParserFfiTest, ErrorAtEofIsZeroWidth) {
  constexpr absl::string_view sql = "SELECT 1 +";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(Range(errors[0]),
            std::make_pair(static_cast<int>(sql.size()), static_cast<int>(sql.size())));
}

TEST(ParserFfiTest, UnterminatedStringRangeRunsToEndOfLine) {
  constexpr absl::string_view sql = "SELECT 'abc\nSELECT 2;";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(Range(errors[0]), At(sql, "'abc"));
}

TEST(ParserFfiTest, UnterminatedStringRangeStopsBeforeCrlf) {
  constexpr absl::string_view sql = "SELECT 'abc\r\nSELECT 2;";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(Range(errors[0]), At(sql, "'abc"));
}

TEST(ParserFfiTest, InputNeedNotBeNulTerminated) {
  const std::string buffer = "SELECT 1;garbage(";
  findings out;
  ASSERT_EQ(check_syntax(buffer.data(), 9, &out), 0);  // "SELECT 1;"
  EXPECT_EQ(out.count, 0);
  free_findings(&out);
}

TEST(ParserFfiTest, NullAndEmptyInputs) {
  findings out;
  ASSERT_EQ(check_syntax(nullptr, 0, &out), 0);
  EXPECT_EQ(out.count, 0);
  EXPECT_EQ(out.items, nullptr);
  EXPECT_NE(check_syntax(nullptr, 5, &out), 0);
  EXPECT_EQ(out.count, 0);
  EXPECT_NE(check_syntax("SELECT 1", 8, nullptr), 0);
}

TEST(ParserFfiTest, FreeIsIdempotent) {
  findings out = {nullptr, 0};
  free_findings(&out);
  free_findings(&out);
  free_findings(nullptr);
  ASSERT_EQ(check_syntax("SELECT 1 2", 10, &out), 0);
  free_findings(&out);
  EXPECT_EQ(out.items, nullptr);
  EXPECT_EQ(out.count, 0);
  free_findings(&out);
}

}  // namespace
```

- [ ] **Step 3: Write the BUILD file**

`googlesql/ffi/BUILD`:

```python
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "parser_ffi_lib",
    srcs = [
        "helpers.cc",
        "helpers.h",
        "parser_ffi.cc",
    ],
    hdrs = ["parser_ffi.h"],
    deps = [
        "//googlesql/parser",
        "//googlesql/parser:token",
        "//googlesql/parser:token_with_location",
        "//googlesql/parser:tokenizer",
        "//googlesql/public:error_helpers",
        "//googlesql/public:error_location_cc_proto",
        "//googlesql/public:language_options",
        "//googlesql/public:options_cc_proto",
        "//googlesql/public:parse_location",
        "//googlesql/public:parse_resume_location",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "parser_ffi_test",
    srcs = ["parser_ffi_test.cc"],
    deps = [
        ":parser_ffi_lib",
        "//googlesql/base/testing:googlesql_gtest_main",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest",
    ],
)
```

- [ ] **Step 4: Write empty-bodied implementations so the test compiles and fails**

`googlesql/ffi/parser_ffi.cc` with only:

```cpp
#include "googlesql/ffi/parser_ffi.h"

extern "C" int check_syntax(const char*, size_t, findings* out) {
  if (out != nullptr)
    *out = {nullptr, 0};
  return 0;
}

extern "C" void free_findings(findings*) {
}
```

Leave `helpers.cc` as just `#include "googlesql/ffi/helpers.h"`, with the header from Step 5.

Run: `bazel test //googlesql/ffi:parser_ffi_test --test_output=errors`
Expected: FAIL. The error-range tests report `errors.size()` 0 instead of 1, and `NullAndEmptyInputs` fails on the `EXPECT_NE` checks. `CleanInputsHaveNoSyntaxErrors` passes.

- [ ] **Step 5: Write `helpers.h`**

```cpp
#ifndef GOOGLESQL_FFI_HELPERS_H_
#define GOOGLESQL_FFI_HELPERS_H_

#include <utility>

#include "absl/strings/string_view.h"

namespace googlesql::ffi {

// [start, end) byte range of the first non-comment token at or after `offset`.
// Returns {sql.size(), sql.size()} at end of input.
// Returns {offset, end of that line} if the tokenizer fails (e.g. unterminated string).
std::pair<int, int> TokenRangeAt(absl::string_view sql, int offset);

// True if only whitespace and comments remain from `offset`.
// False if the tokenizer fails, so the parser gets to report that error.
bool NothingSignificantFrom(absl::string_view sql, int offset);

}  // namespace googlesql::ffi

#endif  // GOOGLESQL_FFI_HELPERS_H_
```

- [ ] **Step 6: Implement `helpers.cc`**

```cpp
#include <optional>
#include <utility>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "googlesql/ffi/helpers.h"
#include "googlesql/parser/tm_token.h"
#include "googlesql/parser/token_with_location.h"
#include "googlesql/parser/tokenizer.h"

namespace googlesql::ffi {
namespace {

using ::googlesql::parser::GoogleSqlTokenizer;
using ::googlesql::parser::Token;
using ::googlesql::parser::TokenWithLocation;

// Next non-comment token, or nullopt if the tokenizer fails.
std::optional<TokenWithLocation> NextSignificant(GoogleSqlTokenizer& tokenizer) {
  while (true) {
    absl::StatusOr<TokenWithLocation> token = tokenizer.GetNextToken();
    if (!token.ok())
      return std::nullopt;
    if (token->kind != Token::COMMENT)
      return *token;
  }
}

}  // namespace

std::pair<int, int> TokenRangeAt(absl::string_view sql, int offset) {
  GoogleSqlTokenizer tokenizer(/*filename=*/"", sql, offset);
  std::optional<TokenWithLocation> token = NextSignificant(tokenizer);
  if (!token.has_value()) {
    // Any of \n, \r\n, \r ends the line (same as ParseLocationTranslator).
    size_t eol = sql.find_first_of("\r\n", offset);
    if (eol == absl::string_view::npos)
      eol = sql.size();
    return {offset, static_cast<int>(eol)};
  }
  if (token->kind == Token::EOI) {
    return {static_cast<int>(sql.size()), static_cast<int>(sql.size())};
  }
  return {token->location.start().GetByteOffset(), token->location.end().GetByteOffset()};
}

bool NothingSignificantFrom(absl::string_view sql, int offset) {
  GoogleSqlTokenizer tokenizer(/*filename=*/"", sql, offset);
  std::optional<TokenWithLocation> token = NextSignificant(tokenizer);
  return token.has_value() && token->kind == Token::EOI;
}

}  // namespace googlesql::ffi
```

- [ ] **Step 7: Implement `parser_ffi.cc` (stops at the first error)**

```cpp
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "googlesql/ffi/helpers.h"
#include "googlesql/ffi/parser_ffi.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/error_location.pb.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/parse_location.h"
#include "googlesql/public/parse_resume_location.h"

namespace {

using ::googlesql::ffi::NothingSignificantFrom;
using ::googlesql::ffi::TokenRangeAt;

struct SyntaxError {
  int start;
  int end;
  std::string message;
};

googlesql::ParserOptions MakeParserOptions() {
  googlesql::LanguageOptions language_options;
  language_options.EnableMaximumLanguageFeatures();
  language_options.SetSupportsAllStatementKinds();
  return googlesql::ParserOptions(
      /*id_string_pool=*/nullptr,
      /*arena=*/nullptr,
      language_options,
      {.mode = googlesql::ERROR_MESSAGE_WITH_PAYLOAD, .attach_error_location_payload = true},
      googlesql::parser::MacroExpansionMode::kNone,
      /*macro_catalog=*/nullptr);
}

// Byte offset of the error in `status`, or `fallback` if it has none.
int ErrorOffset(const absl::Status& status,
                const googlesql::ParseLocationTranslator& translator,
                int fallback) {
  googlesql::ErrorLocation location;
  if (!googlesql::GetErrorLocation(status, &location))
    return fallback;
  absl::StatusOr<int> offset =
      translator.GetByteOffsetFromLineAndColumn(location.line(), location.column());
  return offset.ok() ? *offset : fallback;
}

// Appends each syntax error to `errors`.
// Returns false on internal failure.
bool CollectSyntaxErrors(absl::string_view sql, std::vector<SyntaxError>& errors) {
  if (NothingSignificantFrom(sql, 0))
    return true;
  const googlesql::ParserOptions options = MakeParserOptions();
  const googlesql::ParseLocationTranslator translator(sql);
  googlesql::ParseResumeLocation location = googlesql::ParseResumeLocation::FromStringView(sql);
  bool at_end = false;
  while (!at_end) {
    const int stmt_start = location.byte_position();
    std::unique_ptr<googlesql::ParserOutput> output;
    absl::Status status = googlesql::ParseNextScriptStatement(&location, options, &output, &at_end);
    if (status.ok())
      continue;
    if (!absl::IsInvalidArgument(status))
      return false;
    const int error_offset = ErrorOffset(status, translator, stmt_start);
    auto [start, end] = TokenRangeAt(sql, error_offset);
    errors.push_back({start, end, std::string(status.message())});
    break;  // Recovery is added in Task 2.
  }
  return true;
}

}  // namespace

extern "C" int check_syntax(const char* sql, size_t len, findings* out) {
  if (out == nullptr)
    return 1;
  *out = {nullptr, 0};
  if (len > static_cast<size_t>(INT32_MAX) || (sql == nullptr && len != 0)) {
    return 1;
  }
  std::vector<SyntaxError> errors;
  if (!CollectSyntaxErrors(absl::string_view(sql, len), errors))
    return 1;
  if (errors.empty())
    return 0;
  out->items = new syntax_error[errors.size()];
  for (size_t i = 0; i < errors.size(); ++i) {
    const std::string& text = errors[i].message;
    char* message = new char[text.size() + 1];
    std::memcpy(message, text.c_str(), text.size() + 1);
    out->items[i] = {
        static_cast<uint32_t>(errors[i].start),
        static_cast<uint32_t>(errors[i].end),
        message,
    };
  }
  out->count = errors.size();
  return 0;
}

extern "C" void free_findings(findings* out) {
  if (out == nullptr)
    return;
  for (size_t i = 0; i < out->count; ++i)
    delete[] out->items[i].message;
  delete[] out->items;
  *out = {nullptr, 0};
}
```

- [ ] **Step 8: Run the tests**

Run: `bazel test //googlesql/ffi:parser_ffi_test --test_output=errors`
Expected: PASS, all 11 tests.

If a test fails, handle it this way:
- **`CleanInputsHaveNoSyntaxErrors`:** find the rejected line by running it through `bazel run //googlesql/tools/execute_query:execute_query -- --mode=parse '<sql>'` with all language features enabled (see `--help`). If GoogleSQL itself rejects it, remove that line from `kCleanScript` and say so in the commit message. Do not loosen the implementation.
- **`UnterminatedStringRangeRunsToEndOfLine`:** if the parser reports the error somewhere other than the opening quote, change only the expected substring to the reported position and note it in the commit.

- [ ] **Step 9: Commit**

```bash
git add googlesql/ffi
git commit -m "Add parser FFI C API reporting the first syntax error" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Block-aware recovery (all top-level errors)

**Files:**
- Modify: `googlesql/ffi/helpers.h`, `googlesql/ffi/helpers.cc` (add `SkipStatement`)
- Modify: `googlesql/ffi/parser_ffi.cc` (replace the `break;` in `CollectSyntaxErrors`)
- Test: `googlesql/ffi/parser_ffi_test.cc` (append)

**Interfaces:**
- Consumes: `TokenRangeAt`, `NothingSignificantFrom`, `CollectSyntaxErrors` from Task 1.
- Produces: `int googlesql::ffi::SkipStatement(absl::string_view sql, int stmt_start, int error_offset);`

- [ ] **Step 1: Append the failing tests**

Add inside the anonymous namespace of `parser_ffi_test.cc`:

```cpp
TEST(ParserFfiTest, ReportsEveryTopLevelError) {
  constexpr absl::string_view sql = "SELECT 1 2;\nSELECT 3;\nSELECT 4 5;";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 2);
  EXPECT_EQ(Range(errors[0]), At(sql, "2"));
  EXPECT_EQ(Range(errors[1]), At(sql, "5"));
}

TEST(ParserFfiTest, ErrorInsideBlockSkipsWholeBlock) {
  constexpr absl::string_view sql = "BEGIN\n  SELECT 1 2;\n  SELECT 3;\nEND;\nSELECT 4;";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(Range(errors[0]), At(sql, "2"));
}

TEST(ParserFfiTest, ErrorAtEofAfterValidStatements) {
  constexpr absl::string_view sql = "SELECT 1;\nSELECT 2 +";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(Range(errors[0]),
            std::make_pair(static_cast<int>(sql.size()), static_cast<int>(sql.size())));
}

// Accepted limit: a missing END makes recovery run to EOF.
TEST(ParserFfiTest, MissingEndReportsOnceAndTerminates) {
  constexpr absl::string_view sql = "BEGIN\n  SELECT 1 2;\nSELECT 9 8;";
  EXPECT_EQ(CheckSyntax(sql).size(), 1);
}

TEST(ParserFfiTest, SyntaxErrorsAreSortedAndDisjoint) {
  constexpr absl::string_view sql =
      "SELECT 1 2;\nIF true THEN SELECT 3 4; END IF;\nSELECT 5;\nSELECT 6 +";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 3);
  for (size_t i = 1; i < errors.size(); ++i) {
    EXPECT_LE(errors[i - 1].end, errors[i].start);
  }
}

// Each trap is a broken statement (or block) followed by a second broken statement.
// If the skipper over-skips, the second error ("8") is lost.
class SkippingTrapTest : public ::testing::TestWithParam<absl::string_view> {};

TEST_P(SkippingTrapTest, SecondErrorIsStillReported) {
  const std::string sql = std::string(GetParam()) + "\nSELECT 9 8;";
  std::vector<SyntaxError> errors = CheckSyntax(sql);
  ASSERT_EQ(errors.size(), 2) << sql;
  EXPECT_LT(errors[0].start, errors[1].start);
  EXPECT_EQ(Range(errors[1]), At(sql, "8")) << sql;
}

INSTANTIATE_TEST_SUITE_P(
    Traps,
    SkippingTrapTest,
    ::testing::Values("SELECT IF(true, 1, 2) );",
                      "CREATE TABLE IF NOT EXISTS t (x INT64) );",
                      "SELECT * FROM t FOR SYSTEM_TIME AS OF CURRENT_TIMESTAMP() );",
                      "SELECT CASE WHEN true THEN IF(true, 1, 2) ELSE REPEAT('a', 2) END );",
                      "SELECT begin FROM t );",
                      "BEGIN TRANSACTION );",
                      "BEGIN\n  BEGIN TRANSACTION;\n  BEGIN;\n  SELECT 1 2;\n  COMMIT;\nEND;",
                      "BEGIN BEGIN SELECT 1 2; END; END;",
                      "END;",
                      "lbl: LOOP\n  SELECT 1 2;\nEND LOOP lbl;",
                      "IF (true) THEN\n  SELECT 1 2;\nEND IF;",
                      "CREATE PROCEDURE p() BEGIN SELECT 1 2; END;",
                      "CREATE PROCEDURE p() OPTIONS (strict_mode = false) BEGIN SELECT 1 2; END;",
                      "WHILE true DO SELECT 1 2; END WHILE;",
                      "REPEAT SELECT 1 2; UNTIL true END REPEAT;",
                      "FOR r IN (SELECT 1) DO SELECT 1 2; END FOR;",
                      "CASE 1 WHEN 1 THEN SELECT 1 2; ELSE SELECT 3; END CASE;",
                      "BEGIN SELECT 1 2; EXCEPTION WHEN ERROR THEN SELECT 3; END;",
                      "IF true THEN SELECT CASE WHEN true THEN 1 END; SELECT 1 2; END IF;",
                      "SELECT 1 2, ';', /* ; */ 3 -- ;\n;"));
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `bazel test //googlesql/ffi:parser_ffi_test --test_output=errors`
Expected: FAIL. `ReportsEveryTopLevelError`, `SyntaxErrorsAreSortedAndDisjoint` and every `SkippingTrapTest` case get 1 syntax error instead of 2 or 3. The Task 1 tests still pass.

- [ ] **Step 3: Declare `SkipStatement` in `helpers.h`**

Add before the closing namespace:

```cpp
// Scans from `stmt_start` (the start of a top-level script statement).
// Returns the byte position just after the first ';' that:
// - is not inside a BEGIN/IF/LOOP/WHILE/REPEAT/FOR/CASE block, and
// - starts at or after `error_offset`.
// Returns sql.size() if there is none or the tokenizer fails.
int SkipStatement(absl::string_view sql, int stmt_start, int error_offset);
```

- [ ] **Step 4: Implement `SkipStatement` in `helpers.cc`**

Add `#include <algorithm>` and `#include <vector>`. Inside the anonymous namespace add:

```cpp
// BEGIN followed by these is BEGIN [TRANSACTION] [modes], not a block.
bool BeginsTransaction(const std::optional<TokenWithLocation>& next) {
  if (!next.has_value())
    return true;
  switch (next->kind) {
    case Token::EOI:
    case Token::SEMICOLON:
    case Token::KW_TRANSACTION:
    case Token::KW_ISOLATION:
    case Token::KW_READ:
      return true;
    default:
      return false;
  }
}
```

After `NothingSignificantFrom` add:

```cpp
int SkipStatement(absl::string_view sql, int stmt_start, int error_offset) {
  enum class Frame { kBlock, kCaseExpr };
  std::vector<Frame> frames;
  int paren_depth = 0;
  // True when the previous token lets a new statement begin here.
  bool after_boundary = true;
  Token prev = Token::EOI;  // EOI stands for "no previous token".

  GoogleSqlTokenizer tokenizer(/*filename=*/"", sql, stmt_start);
  std::optional<TokenWithLocation> token = NextSignificant(tokenizer);
  while (token.has_value() && token->kind != Token::EOI) {
    std::optional<TokenWithLocation> next = NextSignificant(tokenizer);
    const Token kind = token->kind;
    const bool at_statement_start =
        after_boundary && paren_depth == 0 && (frames.empty() || frames.back() != Frame::kCaseExpr);
    bool opened_block = false;

    // A keyword right after END closes (END IF, END CASE, ...); never opens.
    if (prev != Token::KW_END) {
      switch (kind) {
        case Token::KW_BEGIN:
          // BEGIN is non-reserved, so it opens a block only at statement start
          // or as a procedure body after ')'.
          if ((at_statement_start || prev == Token::RPAREN) && !BeginsTransaction(next)) {
            frames.push_back(Frame::kBlock);
            opened_block = true;
          }
          break;
        case Token::KW_CASE:
          frames.push_back(at_statement_start ? Frame::kBlock : Frame::kCaseExpr);
          break;
        case Token::KW_IF:
        case Token::KW_LOOP:
        case Token::KW_WHILE:
        case Token::KW_REPEAT:
        case Token::KW_FOR:
          if (at_statement_start) {
            frames.push_back(Frame::kBlock);
            opened_block = true;
          }
          break;
        default:
          break;
      }
    }

    switch (kind) {
      case Token::LPAREN:
        ++paren_depth;
        break;
      case Token::RPAREN:
        paren_depth = std::max(0, paren_depth - 1);
        break;
      case Token::KW_END:
        if (!frames.empty())
          frames.pop_back();
        break;
      case Token::SEMICOLON:
        paren_depth = 0;  // ';' never sits inside parentheses in valid SQL.
        if (frames.empty() && token->location.start().GetByteOffset() >= error_offset) {
          return token->location.end().GetByteOffset();
        }
        break;
      default:
        break;
    }

    after_boundary = kind == Token::SEMICOLON || kind == Token::KW_THEN || kind == Token::KW_ELSE ||
                     kind == Token::KW_DO || kind == Token::COLON ||
                     (opened_block && (kind == Token::KW_BEGIN || kind == Token::KW_LOOP ||
                                       kind == Token::KW_REPEAT));
    prev = kind;
    token = std::move(next);
  }
  return static_cast<int>(sql.size());
}
```

- [ ] **Step 5: Replace the `break;` in `CollectSyntaxErrors` with recovery**

In `parser_ffi.cc`, add `using ::googlesql::ffi::SkipStatement;`, then replace

```cpp
    break;  // Recovery is added in Task 2.
```

with

```cpp
    int next = SkipStatement(sql, stmt_start, error_offset);
    if (next <= stmt_start)
      next = static_cast<int>(sql.size());  // progress
    location.set_byte_position(next);
    at_end = NothingSignificantFrom(sql, next);
```

- [ ] **Step 6: Run the tests**

Run: `bazel test //googlesql/ffi:parser_ffi_test --test_output=errors`
Expected: PASS, every test including all 20 trap instances.

If a trap fails because the **parser** reports its first error somewhere unexpected (for example it rejects `BEGIN;` inside a block), that's fine as long as there are still 2 syntax errors and the second is at `8`. If the count is wrong, debug the skipper. Do not edit the trap.

- [ ] **Step 7: Commit**

```bash
git add googlesql/ffi
git commit -m "Recover from syntax errors at top-level statement boundaries" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Self-contained static archive, C link smoke test, README

**Files:**
- Modify: `.bazelrc` (append)
- Modify: `googlesql/ffi/BUILD` (append)
- Create: `googlesql/ffi/link_smoke_test.c`, `googlesql/ffi/README.md`

**Interfaces:**
- Consumes: `:parser_ffi_lib` and the C API from Task 1.
- Produces: `//googlesql/ffi:googlesql_parser_ffi` → `bazel-bin/googlesql/ffi/libgooglesql_parser_ffi.a`.

- [ ] **Step 1: Write the failing smoke test**

`googlesql/ffi/link_smoke_test.c`:

```c
// Links only libgooglesql_parser_ffi.a: proves the archive is self-contained.
#include <stdio.h>
#include <string.h>

#include "googlesql/ffi/parser_ffi.h"

static int check(const char* sql, size_t expected) {
  findings out;
  if (check_syntax(sql, strlen(sql), &out) != 0) {
    fprintf(stderr, "internal failure on: %s\n", sql);
    return 1;
  }
  size_t count = out.count;
  free_findings(&out);
  if (count != expected) {
    fprintf(stderr, "%s: got %zu syntax errors, want %zu\n", sql, count, expected);
    return 1;
  }
  return 0;
}

int main(void) {
  return check("SELECT 1;", 0) | check("SELECT 1 2;\nSELECT 3 4;", 2);
}
```

Append to `googlesql/ffi/BUILD` (add `load("@rules_cc//cc:cc_import.bzl", "cc_import")` and `load("@rules_cc//cc:cc_static_library.bzl", "cc_static_library")` at the top):

```python
cc_static_library(
    name = "googlesql_parser_ffi",
    deps = [":parser_ffi_lib"],
)

# Wraps the archive the way an external consumer (Zig) sees it: just the .a
# plus the header, with no Bazel dependency graph.
cc_import(
    name = "googlesql_parser_ffi_import",
    hdrs = ["parser_ffi.h"],
    static_library = ":googlesql_parser_ffi",
)

cc_test(
    name = "link_smoke_test",
    srcs = ["link_smoke_test.c"],
    # No default branch: an unsupported OS (Windows) fails here, loudly.
    linkopts = select({
        "@platforms//os:macos": [
            "-lc++",
            "-framework",
            "CoreFoundation",
        ],
        "@platforms//os:linux": [
            "-lstdc++",
            "-lm",
            "-lpthread",
        ],
    }),
    deps = [":googlesql_parser_ffi_import"],
)
```

Run: `bazel test //googlesql/ffi:link_smoke_test --test_output=errors`
Expected: FAIL at analysis with an error that `cc_static_library` requires `--experimental_cc_static_library`.

- [ ] **Step 2: Enable the flag**

Append to `.bazelrc`:

```
# Needed for cc_static_library (//googlesql/ffi:googlesql_parser_ffi).
common --experimental_cc_static_library
```

- [ ] **Step 3: Build the archive and run the smoke test**

Run: `bazel build //googlesql/ffi:googlesql_parser_ffi && ls -lh bazel-bin/googlesql/ffi/libgooglesql_parser_ffi.a`
Expected: the build succeeds and the archive exists (tens of MB).

If the build fails with **duplicate symbol** validation errors (`cc_static_library` checks this), identify the two libraries involved from the error. If both are header-only or test-only deps pulled in by mistake, remove them from `parser_ffi_lib`'s deps. Otherwise stop and report: the fallback is the hand-merged archive from the spec's rejected approach 2, and that needs a decision.

Run: `bazel test //googlesql/ffi:link_smoke_test --test_output=errors`
Expected: PASS.

If linking fails with undefined symbols, they show which system libraries or frameworks are missing (on macOS, typically `-framework Foundation` or `-framework Security`, or `-liconv`). Add exactly those to the `linkopts` `select`. The final list is what the README tells Zig to link.

- [ ] **Step 4: Write `googlesql/ffi/README.md`**

Write the `linkFramework` / `linkSystemLibrary` lines to match the final `linkopts` from Step 3 exactly:

````markdown
# GoogleSQL parser FFI

C ABI for syntax errors over BigQuery scripts. See
`docs/superpowers/specs/2026-10-15-parser-ffi-design.md`.

## Build

```
bazel build //googlesql/ffi:googlesql_parser_ffi
```

Artifacts: `bazel-bin/googlesql/ffi/libgooglesql_parser_ffi.a` and
`googlesql/ffi/parser_ffi.h`.

The archive is for the host OS and CPU. Build it once per target:

| Target | How |
|---|---|
| macOS arm64 | native build on Apple Silicon |
| macOS x86_64 | native on Intel, or `--cpu=darwin_x86_64` on Apple Silicon |
| Linux x86_64 / aarch64 | native build on that machine (or a container of that arch) |

Windows is not supported (GoogleSQL does not build there).

## Linking from Zig

```zig
exe.addIncludePath(.{ .cwd_relative = "<googlesql>/googlesql/ffi" });
exe.addObjectFile(.{ .cwd_relative = "<googlesql>/bazel-bin/googlesql/ffi/libgooglesql_parser_ffi.a" });
// The archive is built against the system C++ runtime. Do NOT use
// exe.linkLibCpp() (Zig's bundled libc++); the two runtimes conflict.
switch (target.result.os.tag) {
    .macos => {
        exe.linkSystemLibrary("c++");
        exe.linkFramework("CoreFoundation");
    },
    .linux => {
        exe.linkSystemLibrary("stdc++");
        exe.linkSystemLibrary("m");
        exe.linkSystemLibrary("pthread");
    },
    else => @panic("googlesql_parser_ffi: unsupported OS"),
}
```

## Semantics

- `start`/`end` are byte offsets `[start, end)` into the buffer you passed.
  Convert to LSP positions on the Zig side.
- One syntax error per broken top-level statement. An error inside a
  `BEGIN…END`/`IF…END IF`/loop block skips the whole block. A missing `END`
  skips to end of file.
````

- [ ] **Step 5: Run the whole package**

Run: `bazel test //googlesql/ffi:all --test_output=errors`
Expected: PASS for `parser_ffi_test` and `link_smoke_test`.

- [ ] **Step 6: Commit**

```bash
git add .bazelrc googlesql/ffi
git commit -m "Package parser FFI as a self-contained static library" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
