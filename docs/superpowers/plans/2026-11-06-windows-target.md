# Windows x86_64 Target Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `googlesql_ffi_windows_x86_64.tar.gz` (one static, MSVC-ABI, `/MT` `.lib` + header + Zig manifest) from the existing `ffi` workflow, without changing the macOS/Linux builds.

**Architecture:** On Windows, Bazel uses its built-in clang-cl toolchain, selected via a `build:windows` section in `.bazelrc`. ICU comes from a pinned vcpkg `x64-windows-static` install, exposed as a local repository behind the existing `@icu//…` labels. A few GoogleSQL files get `#ifdef _WIN32` branches. The existing merge genrule, smoke test and CI matrix each get a Windows branch.

**Tech Stack:** Bazel 7.6.1 (bzlmod), clang-cl (LLVM on `windows-latest`), vcpkg (manifest mode), GitHub Actions, Git Bash.

**Spec:** `docs/superpowers/specs/2026-11-06-windows-target-design.md`

## Global Constraints

- macOS and Linux get the same compiler flags, toolchain and outputs as before. Prove it with an `aquery` diff (Task 1) and green macOS/Linux jobs (every CI run).
- C runtime is static `/MT` everywhere (our code, ICU, smoke test). Never `/MD`, `/MDd` or `/MTd`.
- Upstream source changes: small `#ifdef _WIN32` branches, each marked `// Modified by Oddys: <why>`. If a fix needs more than a local change, stop and ask the owner.
- Work on branch `feat/windows`. Pushes to it are pre-approved. **Commits need the owner's approval** (`googlesql/ffi/CLAUDE.md`).
- Code style: `googlesql/ffi/CODE_STYLE.md`. Don't horizontally align trailing comments in Bash, YAML, Python or Starlark.
- No Windows host is available, so test on Windows by dispatching CI: `gh workflow run ffi.yml --ref feat/windows`, then `gh run watch <id>` and `gh run view <id> --log-failed`. The default branch is `ffi`, so dispatch works.
- Release archive layout: `googlesql_ffi_windows_x86_64/{lib/googlesql_ffi.lib, include/googlesql_ffi.h, LICENSE, LICENSES/, build.zig.zon}`.

## Review Focus

1. **Deeply nested SQL on Windows.** The default stack on Windows is 1 MB (vs 8 MB on Linux), and the parser's stack guard reads its bounds from `ThreadStackStats`. Deeply nested input must give an error, not crash. Test added in Task 3.
2. **Arena memory freed with the wrong function.** `_aligned_malloc` memory released by `free()` corrupts the heap, and only on Windows. Every block allocation and free in `arena.cc` must go through one matched pair of functions. This is covered by the existing tests running on Windows, and reviewers should read the diff with it in mind.
3. **ICU data actually linked.** If `icudt` is missing from the merged `.lib`, `getNFKCCasefoldInstance` fails at runtime, not at link time. Test added in Task 3, and the smoke test runs against the merged `.lib` in Task 4.
4. **CRT mismatch.** A single `/MD` object makes the language server need `msvcp140.dll`, or fails with `LNK2038`. The `dumpbin /directives` check is in Task 4.
5. **macOS/Linux regressions from shared-file edits** (`.bazelrc`, `BUILD`, `bazel/icu.BUILD`, `ffi.yml`). Checked by the `aquery` diff in Task 1 and all four existing jobs staying green.

---

### Task 1: Windows build config and CI job

**Files:**
- Modify: `.bazelrc` (the `build --cxxopt=-std=c++20 … --config=clang …` line)
- Modify: `googlesql/ffi/BUILD` (append platform)
- Modify: `.github/workflows/ffi.yml` (matrix, shell)

**Interfaces:**
- Produces: platform `//googlesql/ffi:windows_x86_64`, the `build:windows` config (applied automatically on Windows), and a CI matrix entry `windows_x86_64`. Later tasks add steps under `if: runner.os == 'Windows'`.

- [ ] **Step 1: Capture macOS compile flags before the change.** On the macOS dev machine, run:
  `bazel aquery -c opt 'mnemonic("CppCompile", //googlesql/ffi:googlesql_ffi_lib)' --output=text > /tmp/aq_before.txt`
- [ ] **Step 2: Edit `.bazelrc`.** Replace the line with:
  ```
  # Modified by Oddys: clang/gcc flags only off Windows; Windows uses clang-cl (MSVC ABI).
  build --java_runtime_version=remotejdk_11
  build:linux --cxxopt=-std=c++20 --host_cxxopt=-std=c++20 --config=clang
  build:macos --cxxopt=-std=c++20 --host_cxxopt=-std=c++20 --config=clang
  build:windows --cxxopt=/std:c++20 --host_cxxopt=/std:c++20
  build:windows --extra_toolchains=@local_config_cc//:cc-toolchain-x64_windows-clang-cl
  build:windows --extra_execution_platforms=//googlesql/ffi:windows_x86_64
  build:windows --platforms=//googlesql/ffi:windows_x86_64
  build:windows --features=static_link_msvcrt
  # TEMPORARY (removed in Task 5): report all compile errors per CI round.
  build:windows --keep_going
  ```
- [ ] **Step 3: Append to `googlesql/ffi/BUILD`** a `platform(name = "windows_x86_64")` with constraint values `@platforms//cpu:x86_64`, `@platforms//os:windows` and `@bazel_tools//tools/cpp:clang-cl`, after `linux_aarch64`.
- [ ] **Step 4: Verify macOS is unchanged.** Run the Step 1 command into `/tmp/aq_after.txt`, then `diff /tmp/aq_before.txt /tmp/aq_after.txt`.
  Expected: no differences in `Command Line` arguments. Differences limited to action keys or digests are acceptable only if the argument lists are the same.
- [ ] **Step 5: Add the CI job.** In `ffi.yml`:
  - Add the matrix entry `- { platform: windows_x86_64, runner: windows-latest }`.
  - Add `defaults: run: shell: bash` under `jobs.build`.
  - Leave the other steps alone for now: the Windows job is expected to fail until Task 4.
- [ ] **Step 6: Commit** (after owner approval), push, and dispatch: `gh workflow run ffi.yml --ref feat/windows`.
  Expected:
  - All four macOS/Linux jobs pass.
  - The Windows log shows `clang-cl.exe` compile lines containing `/MT` and no `D8021`/`-Wno-` errors.
  - The remaining failures are only ICU (`rules_foreign_cc`) and the five source files listed in Task 3.

### Task 2: ICU from vcpkg on Windows

**Files:**
- Create: `vcpkg.json`, `vcpkg-configuration.json` (repo root)
- Create: `bazel/icu_windows.BUILD`
- Modify: `bazel/icu.BUILD` (rename the `configure_make` target, add an `icu` alias)
- Modify: `MODULE.bazel` (local repository)
- Modify: `googlesql/ffi/BUILD` (`genrule(googlesql_ffi)` srcs only)
- Modify: `.github/workflows/ffi.yml` (vcpkg install + cache, Windows only)

**Interfaces:**
- Consumes: the `build:windows` config from Task 1.
- Produces:
  - Repository `@icu_windows` with `:icu` (a `cc_library` linking the ICU static libs) and `:libs` (a filegroup of those `.lib` files).
  - `@icu//:icu` resolves to `@icu_windows//:icu` on Windows and is unchanged elsewhere.
  - Task 4 merges `@icu_windows//:libs`.

- [ ] **Step 1: Pick the ICU version.** Check the vcpkg version database:
  `gh api repos/microsoft/vcpkg/contents/versions/i-/icu.json --jq .content | base64 -d | grep -A1 '"version"' | head`
  Use `76.1` if it's listed. Otherwise use the newest listed version, and note it for Task 5 (README and licenses).
- [ ] **Step 2: Create the vcpkg manifests.**
  - `vcpkg.json`: `{"name": "googlesql-ffi", "version-string": "0", "dependencies": ["icu"], "overrides": [{"name": "icu", "version": "<from Step 1>"}]}`
  - `vcpkg-configuration.json`: a `default-registry` of kind `git` with repository `https://github.com/microsoft/vcpkg` and `baseline` set to the current `microsoft/vcpkg` master SHA, from `gh api repos/microsoft/vcpkg/commits/master --jq .sha`.
- [ ] **Step 3: Create `bazel/icu_windows.BUILD`:**
  - `filegroup(name = "libs")` = `glob(["lib/*icuuc*.lib", "lib/*icuin*.lib", "lib/*icudt*.lib"], allow_empty = False)`.
  - `cc_library(name = "icu")` with:
    - `srcs = [":libs"]`
    - `hdrs = glob(["include/unicode/*.h"])`
    - `includes = ["include"]`
    - `defines = ["U_STATIC_IMPLEMENTATION"]`, without which the ICU headers declare `dllimport`
    - public visibility
  - Release libs only (`lib/`, not `debug/lib/`).
- [ ] **Step 4: Register the repository in `MODULE.bazel`** next to the existing ICU notes (around line 106):
  ```starlark
  new_local_repository = use_repo_rule("@bazel_tools//tools/build_defs/repo:local.bzl", "new_local_repository")
  # Modified by Oddys: Windows ICU from vcpkg (rules_foreign_cc configure_make can't build it with MSVC).
  new_local_repository(
      name = "icu_windows",
      build_file = "//bazel:icu_windows.BUILD",
      path = "vcpkg_installed/x64-windows-static",
  )
  ```
  The repository is only fetched when a Windows `select()` branch references it, so macOS/Linux never touch the missing path.
- [ ] **Step 5: Edit `bazel/icu.BUILD`.**
  - Rename `configure_make(name = "icu")` to `name = "icu_make"`.
  - Add `alias(name = "icu", actual = select({"@platforms//os:windows": "@icu_windows//:icu", "//conditions:default": ":icu_make"}))`.
  - `common`, `headers` and `unicode` keep `deps = ["icu"]`.

  If `@icu_windows` isn't visible from the `@icu` repo (error `No repository visible as '@icu_windows'`), put the alias in `//bazel:BUILD` instead and point the three wrapper targets at it.
- [ ] **Step 6: Check the genrule on macOS.** In `genrule(googlesql_ffi)`, change srcs to `[":googlesql_ffi_core"] + select({"@platforms//os:windows": ["@icu_windows//:libs"], "//conditions:default": ["@icu//:icu"]})`. Leave the `cmd` alone; Task 4 adds the Windows branch. Then run `bazel build -c opt //googlesql/ffi:googlesql_ffi && ./googlesql/ffi/check_linked.sh bazel-bin/googlesql/ffi/libgooglesql_ffi.a` on macOS.
  Expected: it builds, and ICU still shows as linked. Also rerun the Task 1 `aquery` diff: still no differences.
- [ ] **Step 7: Add the CI steps.** In `ffi.yml`, before the first `bazel` step, add two steps under `if: runner.os == 'Windows'`:
  - `actions/cache` for `vcpkg_installed`, keyed on `hashFiles('vcpkg.json', 'vcpkg-configuration.json')`.
  - `"$VCPKG_INSTALLATION_ROOT/vcpkg" install --triplet x64-windows-static`, followed by `ls vcpkg_installed/x64-windows-static/lib` so the log shows the real lib names.
- [ ] **Step 8: Commit** (after owner approval), push, and dispatch CI.
  Expected:
  - No `rules_foreign_cc` errors in the Windows log.
  - `//googlesql/common:unicode_utils` and `//googlesql/public:parse_location` compile.
  - The `ls` output shows the three libs matched by the glob. If the names differ, fix the glob.
  - Remaining Windows failures are only the Task 3 files.

### Task 3: GoogleSQL source fixes and Windows-relevant tests

**Files:**
- Modify: `googlesql/common/multiprecision_int_impl.h` (also fixes `multiprecision_int.h`, which includes it)
- Modify: `googlesql/base/arena.cc`
- Modify: `googlesql/base/logging.cc`
- Modify: `googlesql/common/thread_stack.h`, `googlesql/common/thread_stack.cc`
- Test: `googlesql/ffi/googlesql_ffi_test.cc`

**Interfaces:**
- Consumes: `int check_gsql(const char* sql, size_t len, check_result* res)` and `void free_check_result(check_result* res)` from `googlesql/ffi/googlesql_ffi.h`.
- Produces: none. Each fix is local to its file.

- [ ] **Step 1: Write the tests** in `googlesql_ffi_test.cc`, following the existing `ParserFfiTest` style:
  - `TEST(ParserFfiTest, DeeplyNestedInputReportsErrorWithoutCrashing)`: input `"SELECT " + std::string(100000, '(') + "1;"`. Assert `check_gsql` returns success (match the existing tests' return check), `res.count >= 1`, and the first error's `start < sql.size()`. Free the result.
  - `TEST(ParserFfiTest, NonAsciiIdentifiersParseClean)`: input `"SELECT `Straße`, `ǅemal` FROM `ΣΊΣΥΦΟΣ`;"`. Assert zero errors. These are identifiers that NFKC case folding changes, so the test exercises ICU data.
- [ ] **Step 2: Run them on macOS:** `bazel test //googlesql/ffi:googlesql_ffi_test --test_output=errors`.
  Expected: PASS on macOS. These guard against regressions, not failures to fix first. If either fails on macOS, stop and report: the expectation is wrong, not Windows.
- [ ] **Step 3: Fix `multiprecision_int_impl.h`.** After `#include <sys/types.h>`, add:
  ```cpp
  #ifdef _WIN32
  #include <cstddef>
  using uint = unsigned int;
  using ssize_t = std::ptrdiff_t;
  #endif
  ```
  These go at global scope, matching POSIX `<sys/types.h>`, which supplies them on other OSes.
- [ ] **Step 4: Fix `arena.cc`.** On `_WIN32`, implement `aligned_malloc` with `_aligned_malloc(size, max(minimum_alignment, sizeof(void*)))` from `<malloc.h>`.
  - Add `static void* block_malloc(size_t size)`: on Windows `_aligned_malloc(size, alignof(std::max_align_t))`, else `malloc`.
  - Add `static void block_free(void* p)`: on Windows `_aligned_free`, else `free`.
  - Route every block allocation (the `malloc(block_size_)` and `malloc(adjusted_block_size)` calls, and the small-alignment `malloc` fallback inside `aligned_malloc`) through `block_malloc`.
  - Route every `free(...mem)` through `block_free`.
  - Don't free user-supplied first blocks if the existing code doesn't.
  - Non-Windows behavior stays byte-for-byte the same.
- [ ] **Step 5: Fix `logging.cc`.** Wrap `<libgen.h>` and `<unistd.h>` in `#ifndef _WIN32`. On `_WIN32`:
  - Include `<direct.h>` and `<io.h>`.
  - Map `mkdir(path, mode)` to `_mkdir(path)`.
  - Map `access(p, F_OK)` and `access(p, W_OK)` to `_access(p, 0)` and `_access(p, 2)`.

  Use small `#ifdef` blocks at the call sites or local helper functions, not global macros.
- [ ] **Step 6: Fix `thread_stack.h` and `thread_stack.cc`.**
  - In the header, wrap the `ThreadStackDumperThread` declaration (declared, never defined) and any `<pthread.h>` include in `#ifndef _WIN32`.
  - In the `.cc`, wrap `<pthread.h>` and `<unistd.h>` the same way.
  - Add an `#elif defined(_WIN32)` branch to `SetToCurrentThreadStackBoundaries` that calls `GetCurrentThreadStackLimits(&low, &high)` (`<windows.h>` with `WIN32_LEAN_AND_MEAN` and `NOMINMAX` defined first) and sets `thread_stack_low_`/`thread_stack_high_`.
- [ ] **Step 7: Run on macOS:** `bazel test //googlesql/ffi/... --test_output=errors`.
  Expected: all PASS. Rerun the Task 1 `aquery` diff: still no differences.
- [ ] **Step 8: Commit** (after owner approval), push, and dispatch CI.
  Expected: the Windows step `bazel test -c opt //googlesql/ffi:googlesql_ffi_test` passes, including both new tests. If new Windows-only compile errors appear in other files, fix them the same way and list each in the commit message. If one needs more than a local `#ifdef`, stop and ask the owner.

### Task 4: Merged `.lib`, smoke test, packaging

**Files:**
- Modify: `googlesql/ffi/BUILD` (`genrule(googlesql_ffi)` cmd, `link_smoke_test` linkopts and comment)
- Modify: `.github/workflows/ffi.yml` (Package step, CRT check step)

**Interfaces:**
- Consumes: `@icu_windows//:libs` (Task 2) and `:googlesql_ffi_core`.
- Produces: `bazel-bin/googlesql/ffi/libgooglesql_ffi.a` on every OS. On Windows it's a COFF archive (the genrule output name is fixed; `cc_import` accepts it). The Package step copies it to `lib/googlesql_ffi.lib` on Windows.

- [x] **Step 1: Add the Windows branch to the genrule `cmd`:**
  `"@platforms//os:windows": "$(AR) -NOLOGO -OUT:$@ $$(echo $(SRCS) | tr ' ' '\\n' | grep -E '\\.lib$$')"`
  Use dash-style switches, not `/OUT:`: genrules run under Git Bash, whose MSYS path conversion would rewrite `/`-prefixed arguments. `$(AR)` is the clang-cl toolchain's `llvm-lib`/`lib.exe`.
- [x] **Step 2: Update `link_smoke_test`.**
  - Add `"@platforms//os:windows": []` to `linkopts`.
  - Update the comment above it ("No default branch: an unsupported OS fails here, loudly.").
  - If the CI link reports unresolved Windows system symbols, add only the libs it names (e.g. `"advapi32.lib"`).
- [x] **Step 3: Update the Package step in `ffi.yml`.** Pick the file name by OS:
  ```bash
  if [[ $RUNNER_OS == Windows ]]; then lib=googlesql_ffi.lib; else lib=libgooglesql_ffi.a; fi
  cp bazel-bin/googlesql/ffi/libgooglesql_ffi.a $d/lib/$lib
  ```
  Everything else stays the same.
- [x] **Step 4: Add the CRT check step** (`if: runner.os == 'Windows'`, after the build):
  ```bash
  directives=$(dumpbin -nologo -directives bazel-bin/googlesql/ffi/libgooglesql_ffi.a)
  grep -qi 'libcmt' <<<"$directives"
  ! grep -qiE 'DEFAULTLIB:"?(msvcrt|msvcprt)' <<<"$directives"
  ```
  `dumpbin` isn't on PATH by default. Find it with `vswhere` or use `llvm-objdump --coff-directives` (LLVM is on the runner). Use whichever needs less setup.
- [x] **Step 5: Run on macOS:** `bazel test -c opt //googlesql/ffi:link_smoke_test`.
  Expected: PASS, with the macOS genrule output unchanged.
- [x] **Step 6: Commit** (after owner approval), push, and dispatch CI.
  Expected:
  - All five jobs green.
  - The Windows artifact `googlesql_ffi_windows_x86_64` contains `lib/googlesql_ffi.lib`.
  - The CRT check passes.
  - `link_smoke_test` passes on Windows.

### Task 5: Manifest, docs, licenses, cleanup

**Files:**
- Create: `googlesql/ffi/zig/windows_x86_64/build.zig.zon`
- Modify: `googlesql/ffi/README.md`
- Modify: `googlesql/ffi/LICENSES/licenses-metadata.json` (only if the vcpkg ICU version ≠ 76.1)
- Modify: `.bazelrc` (remove the TEMPORARY `--keep_going` line)

**Interfaces:**
- Consumes: the archive layout from Task 4.

- [ ] **Step 1: Create the Zig manifest.** Copy `googlesql/ffi/zig/macos_arm64/build.zig.zon`, set `.name = .googlesql_ffi_windows_x86_64`, and keep the version (`0.3.0-2026.9.1`) and `.paths`. Remove the `.fingerprint` line and run `zig build` in a temp dir containing only that file. Zig prints the correct fingerprint value, which you then insert.
  Expected: `zig build` in that dir no longer complains about the fingerprint.
- [ ] **Step 2: Update `README.md`:**
  - Add `windows_x86_64` to the platforms comment.
  - Say Windows needs LLVM and vcpkg (`vcpkg install --triplet x64-windows-static` from the repo root) and builds with clang-cl via `build:windows`.
  - Package snippet: on Windows, copy the output to `lib/googlesql_ffi.lib`.
  - Say the archive is MSVC ABI with a static CRT and links from Zig `x86_64-windows-msvc` on a machine with MSVC.
  - Say `check_linked.sh` is macOS/Linux only.
  - Give the vcpkg ICU version if it isn't 76.1.
- [ ] **Step 3: Update licenses.** If ICU ≠ 76.1, add a Windows ICU source entry (vcpkg port, version) to `licenses-metadata.json`. If the ICU license text differs between versions, follow the existing `fetch_licenses.py` flow.
- [ ] **Step 4: Remove the temporary `--keep_going` line** from `.bazelrc`.
- [ ] **Step 5: Commit** (after owner approval), push, and dispatch CI.
  Expected: all five jobs green, and the macOS/Linux artifacts are the same as a run on `ffi` (same file list, and the `.a` sizes match up to timestamps).
- [ ] **Step 6: Hand over to the owner:**
  - Ask them to delete the `spike/windows` branch: `git push origin --delete spike/windows`.
  - Ask them to run the manual Zig check from the spec, Verification step 5: the language server builds on its Windows runner for `x86_64-windows-msvc`, and `dumpbin /dependents` on the `.exe` shows no `msvcp140.dll` or `vcruntime140.dll`.
