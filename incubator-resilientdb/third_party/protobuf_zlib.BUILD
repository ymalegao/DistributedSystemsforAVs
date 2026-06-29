# zlib 1.3.1 for @zlib (protobuf_deps). Registered before protobuf_deps() in WORKSPACE.
# Kept local/gitignored on macOS alongside z.BUILD mac copts.

licenses(["notice"])

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "zlib",
    srcs = glob(["*.c"]),
    hdrs = glob(["*.h"]),
    copts = [
        "-w",
        "-Dverbose=-1",
        "-Wno-implicit-function-declaration",
        "-Wno-deprecated-non-prototype",
        "-Dfdopen=fdopen",
    ],
    includes = ["."],
)
