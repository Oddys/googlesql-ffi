load("@rules_cc//cc:cc_import.bzl", "cc_import")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_static_library.bzl", "cc_static_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "googlesql_ffi_lib",
    srcs = [
        "helpers.cc",
        "googlesql_ffi.cc",
    ],
    hdrs = [
        "helpers.h",
        "googlesql_ffi.h",
    ],
    deps = [
        "//googlesql/parser",
        "//googlesql/parser:token",
        "//googlesql/parser:token_with_location",
        "//googlesql/parser:tokenizer",
        "//googlesql/public:error_helpers",
        "//googlesql/public:error_location_cc_proto",
        "//googlesql/public:language_options",
        "//googlesql/public:options_cc_proto",
        "//googlesql/public:parse_location",
        "//googlesql/public:parse_resume_location",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "googlesql_ffi_test",
    srcs = ["googlesql_ffi_test.cc"],
    deps = [
        ":googlesql_ffi_lib",
        "//googlesql/base/testing:googlesql_gtest_main",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest",
    ],
)

cc_static_library(
    name = "googlesql_ffi_core",
    deps = [":googlesql_ffi_lib"],
)

# The .a files among the srcs of googlesql_ffi: the core archive plus ICU's uc, i18n and data.
ARCHIVES = "$$(echo $(SRCS) | tr ' ' '\\n' | grep -E '(libgooglesql_ffi_core|libicu(uc|i18n|data))\\.a$$')"

# cc_static_library does not fold in ICU (a rules_foreign_cc target with
# prebuilt archives), so merge them in to get one self-contained archive.
# $(AR) is libtool on macOS and ar on Linux; ar takes an MRI script via -M.
genrule(
    name = "googlesql_ffi",
    srcs = [
        ":googlesql_ffi_core",
        "@icu//:icu",
    ],
    outs = ["libgooglesql_ffi.a"],
    cmd = select({
        "@platforms//os:macos": "$(AR) -static -no_warning_for_no_symbols -o $@ " + ARCHIVES,
        "@platforms//os:linux": "(echo 'create $@'; " +
                                "for a in " + ARCHIVES + "; do echo \"addlib $$a\"; done; " +
                                "echo save; echo end) | $(AR) -M",
    }),
    toolchains = ["@bazel_tools//tools/cpp:current_cc_toolchain"],
)

# Wraps the archive the way an external consumer (Zig) sees it: just the .a
# plus the header, with no Bazel dependency graph.
cc_import(
    name = "googlesql_ffi_import",
    hdrs = ["googlesql_ffi.h"],
    static_library = ":googlesql_ffi",
)

cc_test(
    name = "link_smoke_test",
    srcs = ["link_smoke_test.c"],
    # No default branch: an unsupported OS (Windows) fails here, loudly.
    linkopts = select({
        "@platforms//os:macos": [
            "-lc++",
            "-framework",
            "CoreFoundation",
        ],
        "@platforms//os:linux": [
            "-lstdc++",
            "-lm",
            "-lpthread",
        ],
    }),
    deps = [":googlesql_ffi_import"],
)

platform(
    name = "macos_x86_64",
    constraint_values = [
        "@platforms//cpu:x86_64",
        "@platforms//os:macos",
    ],
)

platform(
    name = "macos_arm64",
    constraint_values = [
        "@platforms//cpu:arm64",
        "@platforms//os:macos",
    ],
)

platform(
    name = "linux_x86_64",
    constraint_values = [
        "@platforms//cpu:x86_64",
        "@platforms//os:linux",
    ],
)

platform(
    name = "linux_aarch64",
    constraint_values = [
        "@platforms//cpu:aarch64",
        "@platforms//os:linux",
    ],
)
