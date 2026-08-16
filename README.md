# XGC2 ROS1 Tools Adapter

`xgc_ros1_tools_adapter` is a general Adapter Runtime application for ROS
Noetic. It gives Core or Agent two native ROS1 capabilities without embedding
ROS concepts in the Adapter base class:

- `xgc.ros1.topic.publish@1/publish`
- `xgc.ros1.service.call@1/call`

Both endpoints use the Runtime `operation` interaction, JSON payloads, required
deadlines, required idempotency keys, and non-idempotent side-effect metadata.
The generic C++ Runtime SDK owns registration, process/session fencing, paired
Control and Work streams, dispatch, cancellation, terminal replay, and bounded
queues. This product owns only ROS graph names, dynamic type resolution, JSON
encoding, publishing, and service invocation.

There is no product-facing private request protocol, socket listener, request
cache, workflow model, robot profile, or middleware-specific branch in Core.
Each ROS service call runs in a bounded, one-shot helper process so an
uninterruptible roscpp call can be killed and reaped without consuming an
Adapter dispatch worker forever. Its private inherited socketpair is an
in-package isolation boundary, never an API or legacy migration path.

## Installed identity

- Product: `xgc2-ros1-tools-adapter`
- Debian package: `ros-noetic-xgc2-ros1-tools-adapter`
- ROS package: `xgc_ros1_tools_adapter`
- Executable: `/opt/ros/noetic/lib/xgc_ros1_tools_adapter/xgc_ros1_tools_adapter_node`
- Service helper: `/opt/ros/noetic/lib/xgc_ros1_tools_adapter/xgc_ros1_tools_adapter_service_helper`
- Adapter definition: `/usr/share/xgc2/adapter-definitions/xgc2-ros1-tools-adapter.json`
- Internal process definition: `/usr/share/xgc2/process-definitions/xgc2-ros1-tools-adapter.json`

The Adapter definition is generated from the installed executable during
installation. Its `buildDigest` is therefore the SHA-256 of the exact packaged
ELF, while capability and manifest digests are calculated from their canonical
contracts. No placeholder digest is installed.

## Runtime bootstrap

The process is launched only by the target-local Process Supervisor:

```text
rosrun xgc_ros1_tools_adapter xgc_ros1_tools_adapter_node \
  --adapter-bootstrap-file /absolute/private/bootstrap.pb
```

The Supervisor resolves the executable through the installed ROS package
index. The Adapter remains one supervised process; no launch file owns a
second lifecycle beneath the Experiment Session.

The binary bootstrap supplies the exact Runtime target, instance/process
identity, capability contracts, full instance spec, and single-use credential.
The initial `xgc.ros1.tools.v1.NativeContext` JSON configuration is parsed
before `ros::init`, because roscpp fixes `ROS_MASTER_URI`, `ROS_IP`, and
`ROS_HOSTNAME` during initialization. A running instance rejects any spec that
attempts to change that native context.

Topic publishers and service-call helpers exist only while their capability is
enabled. Disabling a capability, clearing the instance spec, losing the Runtime
session, or stopping the process tears down its native resources. Service
cancellation or timeout before the helper's explicit commit fence is safe;
after that fence the operation terminates as `uncertain` because the ROS server
may already have executed the request.

Before publishing, the Adapter reads the ROS master's process identity. roscpp
fixes `ROS_MASTER_URI` during `ros::init`, so a new master at the same URI
cannot be rebound from this process. If the master PID changes or the master
stays gone, cached publishers are dropped and the process exits. Runtime then
starts a fresh generation that can `ros::init` against the new master. This
keeps repeated Experiment runs that restart `roscore` from publishing into a
dead graph.

## Build

The build requires ROS Noetic, JsonCpp,
`libxgc2-adapter-runtime-client-dev` `0.6.0-1~focal`, and
`xgc2-protobuf-dev` `0.5.0-3~focal`. Builds pin both common inputs exactly.
The resulting Debian package obtains its lower-bounded
`libxgc2-adapter-runtime-client2` dependency from shlibs and does not install
the SDK or protocol schema sources on deployed targets.

```bash
source /opt/ros/noetic/setup.bash
catkin_make -DCMAKE_BUILD_TYPE=RelWithDebInfo
catkin_make run_tests_xgc_ros1_tools_adapter
catkin_test_results --verbose build/test_results
```

The quality and package gates are:

```bash
.xgc2/scripts/check_cpp_quality.sh --work-dir /tmp/xgc2-ros1-tools-adapter-quality
.xgc2/scripts/build_debs_in_docker.sh \
  --work-dir /tmp/xgc2-ros1-tools-adapter-build \
  --output-dir "$PWD/debs"
```

The Docker gate builds protobuf from `v0.5.0-3` and the Runtime SDK from
`v0.6.0-1` by default. A release train can instead set
`XGC2_BOOTSTRAP_COMMON_FROM_GIT=false` and `XGC2_APT_OVERLAY_URL` to consume the
candidate Debian versions from its signed staging repository. The candidate
versions are resolved once, then installed and verified exactly before
compiling this Adapter; this makes `verify` exercise the staged SDK rather than
silently rebuilding against the previous production revision.
