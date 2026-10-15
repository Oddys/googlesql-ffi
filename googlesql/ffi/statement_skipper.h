#ifndef GOOGLESQL_FFI_STATEMENT_SKIPPER_H_
#define GOOGLESQL_FFI_STATEMENT_SKIPPER_H_

#include <utility>

#include "absl/strings/string_view.h"

namespace googlesql::ffi {

// [start, end) byte range of the first non-comment token at or after
// `offset`. At end of input returns {sql.size(), sql.size()}. If the tokenizer
// fails (e.g. unterminated string), returns {offset, end of that line}.
std::pair<int, int> TokenRangeAt(absl::string_view sql, int offset);

// True if only whitespace and comments remain from `offset`.
// False if the tokenizer fails, so the parser gets to report that error.
bool OnlyTriviaFrom(absl::string_view sql, int offset);

}  // namespace googlesql::ffi

#endif  // GOOGLESQL_FFI_STATEMENT_SKIPPER_H_
