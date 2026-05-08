package(default_visibility = ["//visibility:public"])

# intersection.proto kept for reference; not compiled into the bridge.
# The wire format is defined as C structs in resdb_omnet_bridge.h.

cc_library(
    name = "resdb_bridge",
    srcs = ["resdb_omnet_bridge.cc"],
    hdrs = ["resdb_omnet_bridge.h"],
    alwayslink = True,
    deps = [
        "//chain/storage:memory_db",
        "//executor/common:transaction_manager",
        "//platform/networkstrate:service_network",
        "//service/utils:server_factory",
    ],
)

cc_binary(
    name = "resdb_omnet_bridge",
    srcs = [],
    linkshared = True,
    deps = [
        "//chain/state:chain_state",
        "//chain/storage:memory_db",
        "//platform/consensus/ordering/pbft:consensus_manager_pbft",
        "//platform/networkstrate:service_network",
        "//service/utils:server_factory",
        ":resdb_bridge",
    ],
)
