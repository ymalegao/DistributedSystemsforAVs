# Same as //third_party:protobuf_zlib.BUILD in the main repo; duplicated because this
# ecosystem WORKSPACE resolves labels against ecosystem/graphql.

load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])

_ZLIB_HEADERS = [
    "crc32.h",
    "deflate.h",
    "gzguts.h",
    "inffast.h",
    "inffixed.h",
    "inflate.h",
    "inftrees.h",
    "trees.h",
    "zconf.h",
    "zlib.h",
    "zutil.h",
]

_ZLIB_PREFIXED_HEADERS = ["zlib/include/" + hdr for hdr in _ZLIB_HEADERS]

genrule(
    name = "copy_public_headers",
    srcs = _ZLIB_HEADERS,
    outs = _ZLIB_PREFIXED_HEADERS,
    cmd = "cp $(SRCS) $(@D)/zlib/include/",
)

cc_library(
    name = "zlib",
    srcs = [
        "adler32.c",
        "compress.c",
        "crc32.c",
        "deflate.c",
        "gzclose.c",
        "gzlib.c",
        "gzread.c",
        "gzwrite.c",
        "infback.c",
        "inffast.c",
        "inflate.c",
        "inftrees.c",
        "trees.c",
        "uncompr.c",
        "zutil.c",
    ] + _ZLIB_HEADERS,
    hdrs = _ZLIB_PREFIXED_HEADERS,
    copts = select({
        "@bazel_tools//src/conditions:windows": [],
        "//conditions:default": [
            "-Wno-unused-variable",
            "-Wno-implicit-function-declaration",
            "-Wno-deprecated-non-prototype",
            "-Dfdopen=fdopen",
        ],
    }),
    includes = ["zlib/include/"],
    visibility = ["//visibility:public"],
)
