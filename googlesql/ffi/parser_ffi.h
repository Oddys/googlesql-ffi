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
// Returns 0 and fills `out` (count == 0 means the input is clean).
// Returns non-zero on internal failure, len > INT32_MAX, out == NULL, or sql == NULL with len != 0;
// `out` is then {NULL, 0} (if out != NULL).
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
