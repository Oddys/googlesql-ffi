# GoogleSQL FFI

C ABI for syntax errors over GoogleSQL scripts

## Semantics
- `start`/`end` are byte offsets `[start, end)` into the buffer you passed. Convert to LSP positions on the caller's side.
- One syntax error per broken top-level statement. An error inside a `BEGIN…END`/`IF…END IF`/loop block skips the whole block. A missing `END` skips to end of file.

### CI
The release tag format is vX.Y.Z-yyyy-m-n (e.g. v0.1.0-2026.9.1), where X.Y.Z is FFI's own SemVer version, and yyyy-m-n is the upstream's version (year.month.number).

The version in build.zig.zon manifests (in the [zig/](./zig/) directory) must match the release tag version (without the leading v).

## Code style
See [CODE_STYLE.md](googlesql/ffi/CODE_STYLE.md)

### Usage:
Build (on a host matching the platform):
```sh
# platforms: macos_x86_64, macos_arm64, linux_x86_64, linux_aarch64
bazel build -c opt --platforms=//googlesql/ffi:macos_x86_64 //googlesql/ffi:googlesql_ffi
```
Build artifacts can be found in `bazel-bin/googlesql/ffi`.

Prebuilt archives (`lib/libgooglesql_ffi.a` + `include/googlesql_ffi.h`) for all four platforms are produced by the [ffi workflow](.github/workflows/ffi.yml), run manually or on a `v*` tag.

To create a tar locally for testing on the caller's side:
```sh
# Specify your platform
platform=macos_x86_64
d="googlesql_ffi_$platform"
mkdir -p $d/lib $d/include
cp bazel-bin/googlesql/ffi/libgooglesql_ffi.a $d/lib/
cp googlesql/ffi/googlesql_ffi.h $d/include/
tar -czf $d.tar.gz $d
```

Test:
```sh
bazel test //googlesql/ffi/... --test_output=all
# fresh run: ignore cached results, rerun even if nothing changed
bazel test //googlesql/ffi/... --nocache_test_results
```

Generate `compile_commands.json` for `clangd`:
```sh
./refresh_compile_commands.sh
```

## Third-party licenses
LICENSES/ holds the license files of the dependency versions actually built, and
[licenses-metadata.json](googlesql/ffi/LICENSES/licenses-metadata.json) records where each one came from.

To list dependencies:
```sh
bazel cquery 'deps(//googlesql/ffi:googlesql_ffi_lib)' --notool_deps --noimplicit_deps 2>/dev/null \
  | grep -oE '^@[^/]+//(.*/)?third_party/[^/:]+|^@[^/ ]+' \
  | sort -u \
  | grep -vE '^@(platforms|rules_cc|bazel_tools)(//|$)'
```
Lists `repo_name`s as specified in [MODULE.bazel](MODULE.bazel) (actual versions from MODULE.bazel.lock) 
plus ICU from [bazel/http_archive_deps.bzl](bazel/http_archive_deps.bzl) 
plus transitive dependencies.

If the produced list differ from the one used in [check_linked.sh](googlesql/ffi/check_linked.sh) the latter must be updated.

To check which dependencies are linked: 
```sh
./googlesql/ffi/check_linked.sh <path to .a file>
```
If count > 0, the library is linked. The output lists the actual names of dependencies (e.g. the name "nlohmann_json" instead of the repo_name "json").

If the produced list differ from the one used in [fetch_licenses.py](googlesql/ffi/fetch_licenses.py) the latter must be updated.

To fetch license documents:
```sh
uv run googlesql/ffi/fetch_licenses.py
```
