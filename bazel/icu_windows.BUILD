# Modified by Oddys: Windows ICU from a vcpkg x64-windows-static install (static libs, /MT).

package(default_visibility = ["//visibility:public"])

# Release libs only (lib/, not debug/lib/).
filegroup(
    name = "libs",
    srcs = glob(
        [
            "lib/*icuuc*.lib",
            "lib/*icuin*.lib",
            "lib/*icudt*.lib",
        ],
        allow_empty = False,
    ),
)

cc_library(
    name = "icu",
    srcs = [":libs"],
    hdrs = glob(["include/unicode/*.h"]),
    includes = ["include"],
    # Without this the ICU headers declare dllimport.
    defines = ["U_STATIC_IMPLEMENTATION"],
)
