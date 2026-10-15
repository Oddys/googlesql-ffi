# GoogleSQL parser FFI

C ABI for syntax diagnostics over BigQuery scripts. See
`docs/superpowers/specs/2026-10-15-parser-ffi-design.md`.

## Build

```
bazel build //googlesql/ffi:googlesql_parser_ffi
```

Artifacts: `bazel-bin/googlesql/ffi/libgooglesql_parser_ffi.a` and
`googlesql/ffi/parser_ffi.h`. The archive bundles ICU.

The archive is for the host OS and CPU. Build it once per target:

| Target | How |
|---|---|
| macOS arm64 | native build on Apple Silicon |
| macOS x86_64 | native on Intel, or `--cpu=darwin_x86_64` on Apple Silicon |
| Linux x86_64 / aarch64 | native build on that machine (or a container of that arch) |

Windows is not supported (GoogleSQL does not build there). The Linux link
flags and the Linux archive merge step are unverified.

## Linking from Zig

```zig
exe.addIncludePath(.{ .cwd_relative = "<googlesql>/googlesql/ffi" });
exe.addObjectFile(.{ .cwd_relative = "<googlesql>/bazel-bin/googlesql/ffi/libgooglesql_parser_ffi.a" });
// The archive is built against the system C++ runtime. Do NOT use
// exe.linkLibCpp() (Zig's bundled libc++); the two runtimes conflict.
switch (target.result.os.tag) {
    .macos => {
        exe.linkSystemLibrary("c++");
        exe.linkFramework("CoreFoundation");
    },
    .linux => {
        exe.linkSystemLibrary("stdc++");
        exe.linkSystemLibrary("m");
        exe.linkSystemLibrary("pthread");
    },
    else => @panic("googlesql_parser_ffi: unsupported OS"),
}
```

## Semantics

- `start`/`end` are byte offsets `[start, end)` into the buffer you passed.
  Convert to LSP positions on the Zig side.
- One diagnostic per broken top-level statement. An error inside a
  `BEGIN…END`/`IF…END IF`/loop block skips the whole block. A missing `END`
  skips to end of file.
