load("@rules_cc//cc:cc_library.bzl", "cc_library")
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
