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
} gsql_diagnostic;

typedef struct {
  gsql_diagnostic* items;  // owned by the library
  size_t count;
} gsql_diagnostics;

// Parses sql[0..len) as a BigQuery script; sql need not be NUL-terminated.
// Returns 0 and fills *out (count == 0 means the input is clean).
// Returns non-zero only on internal failure, len > INT32_MAX, out == NULL,
// or sql == NULL with len != 0; *out is then {NULL, 0} (if out != NULL).
// Diagnostics are sorted by start and never overlap. Thread-safe.
int gsql_parse_script(const char* sql, size_t len, gsql_diagnostics* out);

// Frees everything gsql_parse_script allocated in *out and resets it to
// {NULL, 0}. Safe on an empty result and on NULL.
void gsql_diagnostics_free(gsql_diagnostics* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // GOOGLESQL_FFI_PARSER_FFI_H_
