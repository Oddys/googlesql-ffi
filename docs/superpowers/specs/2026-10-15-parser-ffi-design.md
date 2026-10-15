# Parser FFI for a Zig language server — design

Date: 2026-10-15
Status: approved in brainstorming, pending spec review

## Goal

A static library with a C ABI that a Zig LSP server links to publish syntax
errors for BigQuery `.sql` files. It must:

- accept both standard SQL and GoogleSQL procedural (scripting) syntax;
- report **all** syntax errors in a file, not just the first;
- never put a syntax error on a statement that parses successfully.

Non-goals: semantic analysis (no catalog, no unknown-table errors), warnings,
quick-fixes, cancellation, incremental parsing. The LSP re-parses the whole
document on each change.

## Decisions

| Topic | Decision |
|---|---|
| Recovery granularity | **Top-level script statement**, block-aware skip (option A). Inner-block recovery (option B) is a possible later extension and must not require C API changes. |
| Language features | `LanguageOptions::EnableMaximumLanguageFeatures()` + `SetSupportsAllStatementKinds()`, hard-coded. Not configurable. |
| Positions | Byte offsets only, half-open `[start, end)` into the caller's UTF-8 buffer. The Zig side converts to LSP positions. |
| Range | The offending token. Zero-width `[len, len)` at end of input. |
| Packaging | `cc_static_library` (Bazel 7.4+, `--experimental_cc_static_library`) producing one self-contained `.a`. |
| Targets | macOS arm64 + x86_64, Linux x86_64 + aarch64; one native build per target. Windows out of scope (GoogleSQL does not build there). |
| Line endings | `\n`, `\r\n` and lone `\r` all end a line, regardless of host OS (matches `ParseLocationTranslator`). |

## C API — `googlesql/ffi/parser_ffi.h`

```c
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
// - non-zero only on internal failure or len > INT32_MAX; `out` is then empty ({NULL, 0}).
int check_syntax(const char* sql, size_t len, findings* out);

// Frees everything check_syntax allocated in *out and resets it to {NULL, 0}.
// Safe on an empty result.
void free_findings(findings* out);

#ifdef __cplusplus
}
#endif
```

Contract:

- Stateless and reentrant: no init call, no globals. Each call builds its own
  `ParserOptions`. Safe to call from any thread.
- Syntax errors are sorted by `start` and never overlap.
- Messages come from the parser in `ERROR_MESSAGE_WITH_PAYLOAD` mode, so they
  carry no `[at line:col]` suffix, e.g. `Syntax error: Unexpected keyword END`.
- `start`/`end` are always `<= len`.

## Recovery loop — `parser_ffi.cc`

```
loc = ParseResumeLocation::FromStringView(sql)
at_end = input is empty or only whitespace/comments
while not at_end:
  S = loc.byte_position()
  status = ParseNextScriptStatement(&loc, opts, &out, &at_end)
  if ok: continue                        # parser advanced loc
  E = byte offset of the error           # see "Error offset" below
  push { TokenRangeAt(sql, E), message }
  next = SkipStatement(sql, S, E)
  loc.set_byte_position(next)
  at_end = only whitespace/comments remain from next
```

**Error offset.** The parser converts its internal byte-offset payload into an
external `ErrorLocation` (1-based line/column, tabs expand to 8 columns;
`parser.cc` → `ConvertInternalErrorLocationAndAdjustErrorString`). Map it back
with `ParseLocationTranslator::GetByteOffsetFromLineAndColumn`. If the error
carries no location, use `E = S`. Line and column are relative to the whole
input (`parser.cc` converts against `resume_location->input()`).

**Progress guarantee.** `SkipStatement` returns a position past at least one
`;` that comes after `S`, or `len`. The loop always advances.

## Token helpers — `helpers.{h,cc}` (internal C++)

```cpp
// [start, end) of the first non-comment token at or after `offset`. EOI → {len, len}. Tokenizer
// error → {offset, end of line}.
std::pair<int, int> TokenRangeAt(absl::string_view sql, int offset);

// Byte position just after the first top-level ';' (depth 0) that lies at or after `error_offset`,
// scanning from `stmt_start`. Returns sql.size() if none exists or the tokenizer fails.
int SkipStatement(absl::string_view sql, int stmt_start, int error_offset);
```

Both use `parser::GoogleSqlTokenizer` (as `SkipNextStatement` does in
`public/parse_helpers.cc`). Comments are skipped. Semicolons inside strings and
comments are therefore ignored.

The skipper keeps a stack of open frames (`block` or `case-expr`) and a
parenthesis depth. A token is at **statement start** when all of these hold:

- the previous significant token is absent, or is `;`, `THEN`, `ELSE`, `DO`,
  the `:` after a label, or a `BEGIN`/`LOOP`/`REPEAT` that just opened a block;
- the parenthesis depth is 0;
- the innermost frame is not `case-expr`.

Frame rules. A keyword that immediately follows `END` never opens a frame,
which handles `END IF`, `END LOOP`, `END CASE` and so on.

| Token | Effect |
|---|---|
| `BEGIN`, if at statement start, right after `)`, or the first paren-depth-0 `BEGIN` after `PROCEDURE` in a top-level `CREATE` statement, and the next token is not `TRANSACTION`, `ISOLATION`, `READ`, `;` or EOI | push `block` |
| `CASE` | push `block` at statement start, otherwise push `case-expr` |
| `IF`, `LOOP`, `WHILE`, `REPEAT`, `FOR`, at statement start only | push `block` |
| `END` | pop (no-op when empty) |
| `;` | pop any trailing `case-expr` frames (a CASE expression cannot contain `;`) |
| `(` / `)` | parenthesis depth +1 / −1, floored at 0 |

Why these rules:

- `BEGIN` is a non-reserved keyword, so `SELECT begin FROM t` must not open a
  block.
- The "right after `)`" case covers `CREATE PROCEDURE p() [OPTIONS(...)] BEGIN`.
- The "after `PROCEDURE`" case covers `CREATE PROCEDURE p() EXTERNAL SECURITY
  {INVOKER|DEFINER} [WITH CONNECTION c] BEGIN`, where `BEGIN` follows a keyword
  or identifier rather than `)`. The flag resets at each top-level `;`.
- `case-expr` frames stop `CASE WHEN c THEN IF(a, b, c) ELSE REPEAT('a', 2) END`
  from opening blocks.
- `;` closes open `case-expr` frames, so an unclosed `CASE` expression cannot
  swallow the rest of the script.
- The statement-start rule excludes the `IF(...)` function, `IF [NOT] EXISTS`,
  `FOR SYSTEM_TIME AS OF` and `FOR UPDATE`.
- `ELSEIF`, `EXCEPTION`, `WHEN` and `UNTIL` do not change frames.

The skipper stops after the first `;` that has an empty stack (after `case-expr` frames are popped) and starts at or
after `error_offset`.

A third helper reports whether only whitespace and comments remain:

```cpp
// True if only whitespace/comments remain from `offset`.
// False on tokenizer error.
bool NothingSignificantFrom(absl::string_view sql, int offset);
```

Known limits (accepted for option A):

- A missing `END` makes the skipper run to EOF. The rest of the file goes
  unchecked until the `END` is added.
- A second error inside the same top-level block is not reported until the
  first one is fixed.

## Build — `googlesql/ffi/BUILD`

```python
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_static_library.bzl", "cc_static_library")

cc_library(
    name = "parser_ffi_lib",
    srcs = ["helpers.cc", "parser_ffi.cc"],
    hdrs = ["helpers.h", "parser_ffi.h"],
    deps = [...],  # //googlesql/parser, tokenizer, language_options,
                   # parse_location, error_helpers, absl status/strings
)

cc_static_library(
    name = "googlesql_parser_ffi",  # bazel-bin/googlesql/ffi/libgooglesql_parser_ffi.a
    deps = [":parser_ffi_lib"],
)
```

Exact dependency labels and the `cc_static_library` load path are confirmed
during implementation. If `rules_cc` 0.2.14 does not export
`cc_static_library`, use the native rule.

Add `common --experimental_cc_static_library` to `.bazelrc`, so that this
single command produces the artifact:

```
bazel build //googlesql/ffi:googlesql_parser_ffi
```

## Zig integration (documentation only)

The deliverables are `libgooglesql_parser_ffi.a` and `parser_ffi.h`. The
`build.zig` sketch:

```zig
exe.addIncludePath(.{ .cwd_relative = "<googlesql>/googlesql/ffi" });
exe.addObjectFile(.{ .cwd_relative = "<googlesql>/bazel-bin/googlesql/ffi/libgooglesql_parser_ffi.a" });
// NOT exe.linkLibCpp(): must match the archive's C++ runtime.
// macOS: linkSystemLibrary("c++") + linkFramework("CoreFoundation").
// Linux: linkSystemLibrary("stdc++"), "m", "pthread".
// Final lists confirmed during implementation.
```

Ship this snippet with the final framework list in a short
`googlesql/ffi/README.md`.

## Testing

Write tests first (TDD). One gtest target, `//googlesql/ffi:parser_ffi_test`,
calls only the C API. Express expected ranges as substrings: a helper finds the
n-th occurrence of a substring in the input and returns its `[start, end)`.

1. **Clean input → 0 syntax errors.** Empty, comments only, standard SQL. Also a
   full script with `DECLARE`, `SET`, `IF/ELSEIF/ELSE/END IF`, `LOOP`,
   `WHILE ... DO`, `REPEAT ... UNTIL`, `FOR ... IN ... DO`,
   `BEGIN ... EXCEPTION WHEN ERROR THEN ... END`, labels, and
   `CREATE PROCEDURE ... BEGIN ... END`.
2. **Multiple top-level errors.** In 3 statements with the 1st and 3rd broken,
   expect exactly 2 syntax errors and none in the 2nd.
3. **Error inside a block.** `BEGIN SELECT FROM; SELECT 1; END; SELECT 2;`
   gives exactly 1 syntax error: none at `END;` and none on `SELECT 2`.
4. **Skipper traps.** Each is followed by a statement that must stay clean:
   `IF(a,b,c)`, `CREATE TABLE IF NOT EXISTS`, `FOR SYSTEM_TIME AS OF`,
   expression `CASE ... END`, `BEGIN TRANSACTION;`, `BEGIN;`, nested blocks,
   stray top-level `END;`.
5. **Ranges.** The range is exactly the offending token. `SELECT 1 +` gives a
   zero-width range at EOF. Multi-byte UTF-8 and tabs before the error check the
   line:col → byte mapping. Also: unterminated string, and `;` inside strings
   and comments.
6. **API contract.** Input that is not NUL-terminated (a slice of a larger
   buffer), output sorted and non-overlapping, `free_findings` on an empty result,
   `len == 0`.

**Link smoke test.** A `cc_import(static_library = ":googlesql_parser_ffi")`
plus a `cc_test` written in pure C (`.c` source) that depends only on that
import. It calls `check_syntax` on one valid and one invalid input. This
proves that the archive is self-contained.

## Extension path (option B, not in scope)

Inner-block recovery replaces `SkipStatement` with a skipper that resumes at the
next inner statement. It also needs a way to parse a block body's statements on
their own. The C API stays unchanged: more syntax errors in the same list.
