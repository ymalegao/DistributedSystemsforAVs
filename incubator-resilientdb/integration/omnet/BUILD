package(default_visibility = ["//visibility:public"])

# intersection.proto kept for reference; not compiled into the bridge.
# The wire format is defined as C structs in resdb_omnet_bridge.h.

cc_library(
    name = "resdb_bridge",
    srcs = [
        "resdb_intersection_scheduler.cc",
        "resdb_omnet_bridge.cc",
    ],
    hdrs = [
        "resdb_intersection_scheduler.h",
        "resdb_omnet_bridge.h",
    ],
    alwayslink = True,
    deps = [
        "//chain/storage:memory_db",
        "//executor/common:transaction_manager",
        "//platform/consensus/ordering/pbft:consensus_manager_pbft",
        "//platform/networkstrate:service_network",
        "//platform/statistic:stats",
        "//service/utils:server_factory",
    ],
)

cc_binary(
    name = "resdb_omnet_bridge",
    srcs = [],
    linkshared = True,
    # Without this, macOS records LC_ID_DYLIB as a relative bazel-out/... path; dyld then
    # looks for that path from cwd and fails when loading libveins. @rpath matches -rpath
    # baked into libveins (see veins src/makefrag).
    linkopts = select({
        "@bazel_tools//src/conditions:darwin": [
            "-Wl,-install_name,@rpath/libresdb_omnet_bridge.dylib",
        ],
        "//conditions:default": [],
    }),
    deps = [
        "//chain/state:chain_state",
        "//chain/storage:memory_db",
        "//platform/consensus/ordering/pbft:consensus_manager_pbft",
        "//platform/networkstrate:service_network",
        "//service/utils:server_factory",
        ":resdb_bridge",
    ],
)
