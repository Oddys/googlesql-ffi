#ifndef GOOGLESQL_FFI_HELPERS_H_
#define GOOGLESQL_FFI_HELPERS_H_

#include <string>
#include <utility>

#include "absl/strings/string_view.h"

namespace googlesql::ffi {

// C++ counterpart of the C `syntax_error`: [start, end) byte range + message.
struct SyntaxError {
  int start;
  int end;
  std::string message;
};

// [start, end) byte range of the first non-comment token at or after
// `offset`. At end of input returns {sql.size(), sql.size()}. If the tokenizer
// fails (e.g. unterminated string), returns {offset, end of that line}.
std::pair<int, int> TokenRangeAt(absl::string_view sql, int offset);

// True if only whitespace and comments remain from `offset`.
// False if the tokenizer fails, so the parser gets to report that error.
bool NothingSignificantFrom(absl::string_view sql, int offset);

// Scans from `stmt_start` (the start of a top-level script statement) and
// returns the byte position just after the first ';' that is not inside a
// BEGIN/IF/LOOP/WHILE/REPEAT/FOR/CASE block and starts at or after
// `error_offset`. Returns sql.size() if there is none or the tokenizer fails.
int SkipStatement(absl::string_view sql, int stmt_start, int error_offset);

} // namespace googlesql::ffi

#endif // GOOGLESQL_FFI_HELPERS_H_
