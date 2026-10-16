// Links only libgooglesql_parser_ffi.a: proves the archive is self-contained.
#include <stdio.h>
#include <string.h>

#include "googlesql/ffi/parser_ffi.h"

static int check(const char* sql, size_t expected) {
  check_result res;
  if (check_gsql(sql, strlen(sql), &res) != 0) {
    fprintf(stderr, "internal failure on: %s\n", sql);
    return 1;
  }
  size_t count = res.count;
  free_check_result(&res);
  if (count != expected) {
    fprintf(stderr, "%s: got %zu syntax errors, want %zu\n", sql, count, expected);
    return 1;
  }
  return 0;
}

int main(void) {
  return check("SELECT 1;", 0) | check("SELECT 1 2;\nSELECT 3 4;", 2);
}
