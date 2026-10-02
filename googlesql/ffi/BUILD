#
# Copyright 2019 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_static_library.bzl", "cc_static_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

package(default_visibility = ["//googlesql/base:googlesql_implementation"])

licenses(["notice"])

refresh_compile_commands(
    name = "refresh_compile_commands",
    targets = {"//googlesql/ffi/..." : "-c opt"}, # must be same options that used for `bazel build`
)

# Produces .o files
cc_library(
    name = "googlesql_parser_ffi",
    srcs = ["googlesql_parser.cc"],
    hdrs = ["googlesql_parser.h"],
    linkstatic = 1,
    deps = [
        "//googlesql/public:error_helpers",
        "//googlesql/public:error_location_cc_proto",
        "//googlesql/public:language_options",
        "//googlesql/public:options_cc_proto",
        "//googlesql/public:parse_helpers",
        "//googlesql/public:parse_location",
        "//googlesql/public:parse_resume_location",
        "@com_google_absl//absl/base:no_destructor",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

# All .o built from source in one arhive, except for a dependency that
# ships prebuilt archives instead of object files - ICU here
# Still behind a flag on Bazel 7.x; it becomes the default in Bazel 8.
# Requires `build --experimental_cc_static_library` in `.bazelrc`
cc_static_library(
    name = "googlesql_parser_nodeps",
    deps = [":googlesql_parser_ffi"],
)

# The .a files among the srcs of googlesql_parser
ARCHIVES = "$$(echo $(SRCS) | tr ' ' '\\n' | grep '\\.a$$')"

# The shipped artifact
# Command (macos_x86_64 example):
#   bazel build -c opt --platforms=//googlesql/ffi:macos_x86_64 //googlesql/ffi:googlesql_parser
# Merges .a produced by googlesql_parser_nodeps and .a of ICU:
# - cmd
#   - $(AR) is the archive tool.
#     - On macOS it's libtool
#     - On Linux it's ar
#   - $@ is the output path, libgooglesql_parser.a.
#   - $$ escapes a literal $ for the shell, because Bazel uses $ for its own substitutions.
# - toolchains: this is what makes $(AR) available. It gives the command the same archiver the C++ compiler setup uses.
genrule(
    name = "googlesql_parser",
    srcs = [
        ":googlesql_parser_nodeps",
        "@icu//:icu",
    ],
    outs = ["libgooglesql_parser.a"],
    cmd = select({
        "@platforms//os:macos": "$(AR) -static -no_warning_for_no_symbols -o $@ " + ARCHIVES,
        # ar takes an MRI script via -M:
        # create libgooglesql_parser.a
        # addlib (for each .a file)
        # save
        # end
        "@platforms//os:linux": "(echo 'create $@'; " +
                                "for a in " + ARCHIVES + "; do echo \"addlib $$a\"; done; " +
                                "echo save; echo end) | $(AR) -M",
    }),
    toolchains = ["@bazel_tools//tools/cpp:current_cc_toolchain"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "googlesql_parser_test",
    srcs = ["googlesql_parser_test.cc"],
    deps = [
        ":googlesql_parser_ffi",
        "//googlesql/base/testing:googlesql_gtest_main",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest",
    ],
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
