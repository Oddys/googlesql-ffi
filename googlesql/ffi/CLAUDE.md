Before committing/pushing ask the user to approve. Proceed to the next task/step after approval.

## Code style

Follow code style specified in @CODE_STYLE.md

Never align inline comments horizontally unless the formatter can maintain this automatically. E.g.:
```cpp
int x;     // short
int yyyy;  // long
```
is fine for C++ because clang-format keeps the alignment,

while
```sh
x = 1     # one
y = 1000  # thousand
```
is bad for Bash or Python because they do not have no such formatter.
