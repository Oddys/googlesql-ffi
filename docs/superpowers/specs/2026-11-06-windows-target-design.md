# Windows x86_64 target for googlesql/ffi

## Intent

The FFI library (`//googlesql/ffi:googlesql_ffi`) is linked into a Zig language
server used by the Zed editor. Today it ships prebuilt static archives for
macOS (x86_64, arm64) and Linux (x86_64, aarch64). Windows users of Zed
can't get the language server. This work adds a Windows release built and
packaged the same way as the other four platforms.

Success: a `v*` tag produces `googlesql_ffi_windows_x86_64.tar.gz` next to
the existing four archives, and the Zig language server links it when
targeting `x86_64-windows-msvc`.

The consumer: the Zig language server is built in GitHub Actions on a
dedicated Windows runner (owner's answer), which has MSVC and the Windows
SDK. So linking the MSVC-ABI `.lib` (which pulls in MSVC's static C++
library and CRT through `/DEFAULTLIB`) works there. Cross-compiling the
language server from macOS/Linux isn't supported by this design.

## Goals

- New platform `windows_x86_64`, built in CI on `windows-latest`. CI runs
  the same steps as the other platforms: unit test, build, link smoke test,
  package, release.
- The output is a single self-contained static library with MSVC ABI that
  includes ICU, just like the `.a` on other platforms.
- The archive layout matches the other platforms, plus a Zig manifest.

## Non-goals

- Windows ARM64. Tracked in `googlesql/ffi/TODO.md` (see "Known blockers"
  below).
- 32-bit Windows.
- MinGW / `x86_64-windows-gnu` ABI.
- Building or testing the Zig language server itself in this repo.

## Anti-goals

- **macOS and Linux builds must not change.** They must get the same
  compiler flags, toolchain and outputs as today. Any refactor of shared
  config (`.bazelrc`, `BUILD`, workflow) must leave those platforms'
  effective flags identical.
- No large rewrites of upstream GoogleSQL code. Source changes are small,
  local `#ifdef _WIN32` branches, so rebasing onto upstream stays cheap.

## Constraints

- Bazel 7.6.1 (`.bazelversion`), bzlmod, `--experimental_cc_static_library`.
- Release tag format and `build.zig.zon` version check are unchanged (see
  `googlesql/ffi/README.md`, "CI").
- Repo rule: commits and pushes need the owner's approval
  (`googlesql/ffi/CLAUDE.md`). For this work, the owner gave one approval
  covering all pushes to the working branch `feat/windows`. Iterate on
  Windows through CI runs on that branch. Commits still follow the normal
  rule. Code style: `googlesql/ffi/CODE_STYLE.md`.

## Decisions

### Compiler: clang-cl (decided during the spike)

A throwaway spike on branch `spike/windows` (CI runs 42201814493,
42204324119, 42207544303) established:

- **Plain MSVC `cl.exe` doesn't work.** GoogleSQL uses `__int128` in
  `googlesql/common/multiprecision_int*.h` (MSVC doesn't support it), and
  `cl` also fails on `googlesql/parser/macros/quoting.cc` (StatusBuilder
  conversion) and `googlesql/public/pico_time.h` (constexpr).
- **clang-cl works.** It is MSVC ABI and links with MSVC's STL and CRT, so
  the output can be consumed by Zig's `-msvc` target. All of abseil,
  protobuf, re2 and googletest compile, as do the GoogleSQL files above.
  It's preinstalled on GitHub's `windows-latest` (LLVM in `C:\Program Files\LLVM`).

Selected with Bazel's built-in auto-configured toolchain. This is what the
spike used:

```starlark
platform(
    name = "windows_x86_64",
    constraint_values = [
        "@platforms//cpu:x86_64",
        "@platforms//os:windows",
        "@bazel_tools//tools/cpp:clang-cl",
    ],
)
```

```sh
bazel build -c opt \
  --extra_toolchains=@local_config_cc//:cc-toolchain-x64_windows-clang-cl \
  --extra_execution_platforms=//googlesql/ffi:windows_x86_64 \
  --platforms=//googlesql/ffi:windows_x86_64 \
  //googlesql/ffi:googlesql_ffi
```

Builder's call: put the toolchain flags in a `build:windows` section of
`.bazelrc` (preferred, so local Windows builds match CI) or in the workflow.
Also use a short output root on CI (`--output_user_root=C:/b` worked) to
avoid path-length issues.

### `.bazelrc`: per-OS compiler flags

Today `build --cxxopt=-std=c++20 --host_cxxopt=-std=c++20 --config=clang`
applies on every OS. MSVC-style drivers reject the `-Wno-…` flags (error
D8021). The spike moved it to:

```
build --java_runtime_version=remotejdk_11
build:linux --cxxopt=-std=c++20 --host_cxxopt=-std=c++20 --config=clang
build:macos --cxxopt=-std=c++20 --host_cxxopt=-std=c++20 --config=clang
build:windows --cxxopt=/std:c++20 --host_cxxopt=/std:c++20
```

(`common --enable_platform_specific_config` is already on.) This keeps
Linux and macOS identical (anti-goal). Mark it with the existing
"Modified by Oddys" comment style already used in `.bazelrc`. Builder's
call whether clang-cl needs any of the `--config=clang` warning
suppressions (clang-cl accepts `-Wno-…`). Add only what the build needs.

### GoogleSQL source fixes

The last spike round (clang-cl) failed only on these. Fix each with a
small `#ifdef _WIN32` branch and a `// Modified by Oddys: …` comment:

| File | Issue |
|---|---|
| `googlesql/common/multiprecision_int.h`, `multiprecision_int_impl.h` | `uint`, `ssize_t` undeclared (POSIX typedefs) |
| `googlesql/base/arena.cc` | `posix_memalign` (use `_aligned_malloc`/`_aligned_free`, matching the free path) |
| `googlesql/base/logging.cc` | `libgen.h` (`basename`) |
| `googlesql/common/thread_stack.cc`, `thread_stack.h` | `pthread.h`, `pthread_t` |

The list may grow once ICU stops failing first, since targets behind it
never compiled in the spike. New failures get the same treatment. If one
needs more than a local fix, stop and raise it with the owner.

### ICU on Windows: vcpkg static build (my call)

The current ICU build (`bazel/icu.BUILD`, `rules_foreign_cc`
`configure_make`) can't work on Windows. The spike showed it failing to
bootstrap GNU make and pkg-config with MSVC. macOS and Linux keep it unchanged.

What the FFI needs from ICU (from `bazel cquery`): only
`//googlesql/public:parse_location` (header-only `U8_NEXT` from
`unicode/utf8.h`) and `//googlesql/common:unicode_utils`
(`icu::Normalizer2::getNFKCCasefoldInstance`, `StringByteSink`, `ErrorCode`).
That means `icuuc` plus ICU data. `icui18n` isn't needed by the FFI, but
`@icu//:icu` exposes it, so include it if anything else in the dependency
graph references it.

On Windows, ICU comes from vcpkg (preinstalled on `windows-latest`):

- Triplet `x64-windows-static`: static `.lib`s, static CRT (`/MT`).
- Pin the version. Match upstream's 76.1 if the vcpkg registry has it,
  otherwise the nearest version, recorded in the README.
- Expose it to Bazel under the same labels the rest of the graph uses
  (`@icu//:icu`, `@icu//:common`, `@icu//:headers`), selected on
  `@platforms//os:windows`. The current setup is `http_archive` in
  `bazel/http_archive_deps.bzl` with `build_file = "//bazel:icu.BUILD"`.
  A `select()` inside `icu.BUILD` that switches to `cc_import`s of the
  vcpkg libs on Windows is the simplest option. Builder's call on the exact
  mechanism (repository rule pointing at the vcpkg install dir, or the CI
  step copying the libs into a known path).

Fallback if vcpkg proves unworkable: build ICU `common` + data with native
`cc_library` rules over the existing `@icu` source archive. Raise this with
the owner before switching, since it's a bigger change.

### C runtime: static `/MT` everywhere (owner's decision)

The language server must start on Windows machines that don't have the
Visual C++ Redistributable. So the CRT is linked statically: no dependency
on `msvcp140.dll` or `vcruntime140.dll`.

- Our objects: enable Bazel's `static_link_msvcrt` feature for the Windows
  config (`--features=static_link_msvcrt`). Check the compile lines show
  `/MT`, not `/MD`, and never the debug `/MTd`.
- ICU: the `x64-windows-static` vcpkg triplet (`/MT`).
- Every object in the merged `.lib` must agree, or linking fails with
  `LNK2038` (RuntimeLibrary mismatch). `link_smoke_test` catches this in CI.
- The Zig language server's own link must also use the static CRT. The
  `.lib`'s `/DEFAULTLIB:libcmt`/`libcpmt` directives request it.
  Verification step 4 confirms this works under Zig.

### Packaging: one `.lib`

- `cc_static_library(googlesql_ffi_core)` should produce a `.lib` on
  Windows. Verify the output name.
- `genrule(googlesql_ffi)` gets a Windows branch that merges core + ICU
  libs: `lib.exe /OUT:$@ <libs>` (or `llvm-lib`). The `ARCHIVES` grep
  matches `\.a$` names today and must also match the Windows names. The
  output name differs per OS (`libgooglesql_ffi.a` vs `googlesql_ffi.lib`),
  so the builder decides how to express that (separate genrule behind a
  `select`, or a fixed name). Whatever is chosen, the release archive must
  contain `lib/googlesql_ffi.lib`.
- `cc_import(googlesql_ffi_import)` must accept the Windows lib.
- `link_smoke_test` gets an `@platforms//os:windows` branch in `linkopts`.
  Expect none beyond the defaults, or Windows system libs if ICU or abseil
  need them (e.g. `advapi32`, `dbghelp`). Keep "no default branch"
  (the existing comment there must be updated, since it names Windows as
  the unsupported OS).

### CI (`.github/workflows/ffi.yml`)

- Matrix: add `{ platform: windows_x86_64, runner: windows-latest }`.
- Steps run under `shell: bash` (Git Bash on the runner). Set
  `defaults.run.shell: bash` for the job, or per step.
- Windows needs the vcpkg ICU install step (cache it) before Bazel.
- `bazel test -c opt //googlesql/ffi:googlesql_ffi_test` currently runs
  without `--platforms`. On Windows it must use the clang-cl config too.
- Package step: copy `googlesql_ffi.lib` instead of `libgooglesql_ffi.a` on
  Windows. Everything else (header, LICENSE, LICENSES, manifest, version
  check, tar.gz) is unchanged.
- Release job unchanged (it collects `*.tar.gz`).

### Zig manifest

New `googlesql/ffi/zig/windows_x86_64/build.zig.zon`, copied from
`macos_arm64` with `.name = .googlesql_ffi_windows_x86_64`, the same
version, and a **new** fingerprint. Zig computes it: leave it out or use a
placeholder, run `zig build`, and Zig reports the right value. The
`.paths` stay the same.

### Docs and licenses

- `googlesql/ffi/README.md`: add `windows_x86_64` to the platforms list and
  the package snippet (`.lib` name). Note the vcpkg ICU version if it
  differs from 76.1.
- `googlesql/ffi/LICENSES`: ICU's license is already there. If the vcpkg
  version differs, update `licenses-metadata.json` to record where the
  Windows ICU comes from.
- `check_linked.sh` uses `.a` tooling. Running it on Windows is out of scope
  unless trivial. Note that in the README.

## Verification

1. CI green on all five platforms; the macOS/Linux jobs show no change
   (compare the effective flags, e.g. `bazel aquery` or the compile
   command line, before and after the `.bazelrc` change).
2. `googlesql_ffi_test` passes on Windows.
3. `link_smoke_test` passes on Windows, linking only the `.lib` + header.
4. Optional: in CI, check the merged `.lib` has no `/MD` objects:
   `dumpbin /directives` shows `libcmt`/`libcpmt`, and doesn't show
   `msvcrt`/`msvcprt`.
5. **Manual, by the owner:** the Zig language server builds for
   `x86_64-windows-msvc` on its Windows runner against the release
   archive. `dumpbin /dependents` on the resulting `.exe` lists no
   `msvcp140.dll`/`vcruntime140.dll`, and it works in Zed.

## Known blockers outside scope

- **Windows ARM64:** module resolution fails before compiling, because
  gRPC's `grpc_python_dependencies` pip hub needs Python 3.10, and
  rules_python has no 3.10 for Windows ARM (only 3.11+). gRPC isn't linked
  into the FFI. A `MODULE.bazel` override might fix it. Tracked in
  `googlesql/ffi/TODO.md`.

## Left to the builder

- Exact Bazel mechanism for wiring vcpkg ICU into `@icu//…` labels.
- Where the clang-cl toolchain flags live (`.bazelrc` preferred).
- How the per-OS output filename is expressed in `BUILD`.
- Any extra Windows link libraries the smoke test turns out to need.
- Additional `#ifdef _WIN32` source fixes surfaced after ICU is unblocked.
- Work happens on `feat/windows`. The `spike/windows` branch is
  throwaway. Delete it once the owner agrees.
