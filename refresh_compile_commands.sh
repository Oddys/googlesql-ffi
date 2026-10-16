#!/usr/bin/env bash
# Regenerates compile_commands.json (used by clangd) for //googlesql/ffi/...
# Usage: ./refresh_compile_commands.sh (from the repo root).
#
# How it works:
#   - temporarily appends the dependency on Hedron's compile-commands extractor
#     (https://github.com/hedronvision/bazel-compile-commands-extractor) to MODULE.bazel.
#   - adds a refresh_compile_commands target to googlesql/ffi/BUILD and runs it.
#   - reverts both files with `git checkout`, also on failure or Ctrl-C.
#
# Gotchas:
#   - `-c opt` must match the options you build with, or clangd's flags drift.
#   - `--features=-parse_headers` is required: abseil's header-parsing actions (`-xc++-header -fsyntax-only`)
#     crash Hedron with "AssertionError: No source files found in compile args".
#   - `--host_features=-parse_headers` does the same for the exec configuration (tools Bazel builds for itself),
#     which --features alone doesn't cover.
#   - Rerun after adding, removing, or renaming files.
#   - Headers without an entry get flags guessed from a neighbor, e.g.:
#     in googlesql/ffi/ that can be link_smoke_test.c, so C++ headers get parsed as C
#     ("'string' file not found", "Unknown type name 'namespace'").
#   - Restart clangd in your editor afterwards.
set -euo pipefail
cd "$(dirname "$0")"

git diff --quiet -- MODULE.bazel googlesql/ffi/BUILD ||
  { echo "MODULE.bazel or googlesql/ffi/BUILD has local changes; aborting." >&2; exit 1; }
trap 'git checkout -- MODULE.bazel googlesql/ffi/BUILD' EXIT

cat >> MODULE.bazel <<'EOF'

bazel_dep(name = "hedron_compile_commands", dev_dependency = True)
git_override(
    module_name = "hedron_compile_commands",
    remote = "https://github.com/hedronvision/bazel-compile-commands-extractor.git",
    commit = "4f28899228fb3ad0126897876f147ca15026151e",
)
EOF

cat >> googlesql/ffi/BUILD <<'EOF'

load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")
refresh_compile_commands(
    name = "refresh_compile_commands",
    targets = {"//googlesql/ffi/...": "-c opt --features=-parse_headers --host_features=-parse_headers"},
)
EOF

bazel run //googlesql/ffi:refresh_compile_commands
