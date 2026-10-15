load("@rules_cc//cc:cc_import.bzl", "cc_import")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_static_library.bzl", "cc_static_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "parser_ffi_lib",
    srcs = [
        "parser_ffi.cc",
        "statement_skipper.cc",
        "statement_skipper.h",
    ],
    hdrs = ["parser_ffi.h"],
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
    name = "parser_ffi_test",
    srcs = ["parser_ffi_test.cc"],
    deps = [
        ":parser_ffi_lib",
        "//googlesql/base/testing:googlesql_gtest_main",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest",
    ],
)

cc_static_library(
    name = "googlesql_parser_ffi_core",
    deps = [":parser_ffi_lib"],
)

# NOTE: the Linux branch is unverified (no Linux machine was available); only
# the macOS branch has been built and tested.
# cc_static_library does not fold in ICU (a rules_foreign_cc target with
# prebuilt archives), so merge them in to get one self-contained archive.
genrule(
    name = "googlesql_parser_ffi",
    srcs = [
        ":googlesql_parser_ffi_core",
        "@icu//:icu",
    ],
    outs = ["libgooglesql_parser_ffi.a"],
    cmd = select({
        "@platforms//os:macos": "libtool -static -o $@ $(locations :googlesql_parser_ffi_core) $$(echo $(locations @icu//:icu) | tr ' ' '\\n' | grep -E 'libicu(uc|i18n|data)\\.a$$')",
        "@platforms//os:linux": "mkdir -p $(@D)/_merge && i=0 && for f in $(locations :googlesql_parser_ffi_core) $$(echo $(locations @icu//:icu) | tr ' ' '\\n' | grep -E 'libicu(uc|i18n|data)\\.a$$'); do i=$$((i+1)); cp $$f $(@D)/_merge/$$i.a; done && (echo 'CREATE $@'; for g in $(@D)/_merge/*.a; do echo \"ADDLIB $$g\"; done; echo SAVE; echo END) | ar -M",
    }),
)

# Wraps the archive the way an external consumer (Zig) sees it: just the .a
# plus the header, with no Bazel dependency graph.
cc_import(
    name = "googlesql_parser_ffi_import",
    hdrs = ["parser_ffi.h"],
    static_library = ":googlesql_parser_ffi",
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
    deps = [":googlesql_parser_ffi_import"],
)
