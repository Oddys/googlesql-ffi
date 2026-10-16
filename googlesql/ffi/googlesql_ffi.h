#ifndef GOOGLESQL_FFI_GOOGLESQL_FFI_H_
#define GOOGLESQL_FFI_GOOGLESQL_FFI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t start;       // byte offset, inclusive
  uint32_t end;         // byte offset, exclusive (== start at EOF)
  const char* message;  // NUL-terminated UTF-8, no location suffix
} gsql_error;

typedef struct {
  gsql_error* errors;  // owned by the library
  size_t count;
} check_result;

// Checks GoogleSQL syntax in sql[0..len); sql need not be NUL-terminated.
// Returns 0 and fills `res` (count == 0 means the input is clean).
// Returns non-zero on internal failure, len > INT32_MAX, res == NULL, or sql == NULL with len != 0;
// `res` is then {NULL, 0} (if res != NULL).
// Syntax errors are sorted by start and never overlap.
// Thread-safe.
int check_gsql(const char* sql, size_t len, check_result* res);

// Frees everything check_gsql allocated in *res and resets it to {NULL, 0}.
// Safe on an empty result and on NULL.
void free_check_result(check_result* res);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // GOOGLESQL_FFI_GOOGLESQL_FFI_H_
