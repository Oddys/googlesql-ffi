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

// A minimal C ABI over the GoogleSQL parser, exposing syntax checking only.

#ifndef GOOGLESQL_FFI_GOOGLESQL_PARSER_H_
#define GOOGLESQL_FFI_GOOGLESQL_PARSER_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  // 0-based byte offset into the sql buffer, or -1 if unknown.
  int start_byte;
  // 1-based line, or -1 if unknown.
  int line;
  // 1-based column, counted with tabs expanded to 8, or -1 if unknown.
  int column;
  // NUL-terminated, freed by gsql_syntax_errors_free.
  const char* message;
} gsql_syntax_error;

// Checks every statement in `sql` and returns the number of syntax errors
// found, storing the array of them in *`errors` (NULL when the count is 0).
// The list may be partial: input that cannot be tokenized past the last
// reported error (an unterminated string, say) ends the scan there.
//
// At most one error is reported per statement: the grammar has no in-statement
// recovery, so recovery is statement-level (skip to the next semicolon).
//
// All LanguageFeatures are enabled: an editor wants fewer false positive error
// checks.
//
// `errors` is an out parameter; the array is allocated here, so pass it and the
// returned count to gsql_syntax_errors_free. Point it at a pointer you have set
// to NULL, since the caller cannot know the count in advance and so cannot
// allocate the array itself:
//
//   gsql_syntax_error* errors = NULL;
//   int count = gsql_check_syntax(sql, sql_len, &errors);
//
// Reusing that variable for a later call means setting it back to NULL after
// freeing. It is read on entry, so an uninitialized one is undefined behaviour.
//
// Returns -1 if `errors` is NULL or does not point at a NULL pointer, if
// `sql_len` exceeds INT_MAX, or if checking fails internally (out of memory).
// A NULL `sql` is treated as empty input.
int gsql_check_syntax(const char* sql, size_t sql_len,
                      gsql_syntax_error** errors);

// Frees an array populated by `gsql_check_syntax`. NULL is a no-op.
// Passing a pointer to some other array and/or
// an integer that is not the item count returned from `gsql_check_syntax` is
// UB.
void gsql_syntax_errors_free(gsql_syntax_error* errors, int count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // GOOGLESQL_FFI_GOOGLESQL_PARSER_H_
