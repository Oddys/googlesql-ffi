# Code style

## Autoformatting:
```sh
clang-format -i googlesql/ffi/*.{h,cc,c}
``` 
It picks up `googlesql/ffi/.clang-format`, which covers layout (Google base, 100 columns).

## Rules
- Add a trailing comma to braced init list that wraps (does not fit a single line), so that
  clang-format puts one element per line. 
```cpp
res->errors[i] = {
    static_cast<uint32_t>(errors[i].start),
    static_cast<uint32_t>(errors[i].end),
    message,
};
```
- Put one idea per line in function's comments (summary, each return case, each guarantee), 
  with a `Returns:` list for several outcomes:
```cpp
// Scans from `stmt_start` (the start of a top-level script statement).
// Returns the byte position just after the first ';' that:
// - is not inside a BEGIN/IF/LOOP/WHILE/REPEAT/FOR/CASE block, and
// - starts at or after `error_offset`.
// Returns sql.size() if there is none or the tokenizer fails.
int SkipStatement(absl::string_view sql, int stmt_start, int error_offset);
```
- Follow the Google convention in naming for C++: 
  `PascalCase` functions and types, 
  `snake_case` variables, 
  `kName` constants. 
- Use `snake_case` for C names.
