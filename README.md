# XGC2 ROS1 Automation Gateway

`xgc_ros1_automation_gateway` is the local ROS Noetic execution boundary
used by XGC2 workflow nodes. It accepts length-framed JSON requests over a Unix
domain socket, dynamically resolves installed ROS message and service
descriptions with `ros_babel_fish`, and performs publish or service-call
operations without requiring XGC2 Agent to link against ROS.

## Package

- Product id: `xgc2-ros1-automation-gateway`
- Source path: `products/ros1/communication/ros1-automation-gateway`
- Release branch: `noetic`
- Package type: `ros1-apt`
- Debian package: `ros-noetic-xgc2-ros1-automation-gateway`
- ROS package: `xgc_ros1_automation_gateway`
- Executable: `/opt/ros/noetic/lib/xgc_ros1_automation_gateway/xgc_ros1_automation_gateway_node`

The wire protocol uses a four-byte big-endian payload length followed by one
UTF-8 JSON object. Protocol version 1 limits a frame to 8 MiB. The socket is
local-only; the product does not open a TCP port.

## Install

```bash
sudo apt update
sudo apt install ros-noetic-xgc2-ros1-automation-gateway
```

## Smoke Test

```bash
source /opt/ros/noetic/setup.bash
rospack find xgc_ros1_automation_gateway
test -x /opt/ros/noetic/lib/xgc_ros1_automation_gateway/xgc_ros1_automation_gateway_node
```

The package includes the ROS descriptions needed for common `std_msgs`
and `std_srvs` checks and depends on `mavros_msgs` so MAVROS
messages and services are available to the dynamic type resolver. The
implementation has no message-name or service-name allowlist; a requested type
must be installed in the selected ROS environment.

## Runtime Ownership

XGC2 Agent starts and stops this process on demand. It supplies a context-local
socket path:

```bash
source /opt/ros/noetic/setup.bash
rosrun xgc_ros1_automation_gateway xgc_ros1_automation_gateway_node \
  --socket /run/xgc2/ros1-automation-gateway/<context-sha256>.sock
```

The socket path is required and XGC2 Agent assigns one deterministic socket per
ROS context. This package intentionally installs no systemd unit: lifecycle,
restart, audit, and per-context isolation belong to XGC2 Agent.

The product owns:

- conversion between JSON values and dynamically described ROS1 values;
- generic topic publication and service request/response handling;
- the local framed protocol, validation limits, and structured error replies;
- the installed node executable and ROS package metadata.

It does not own XGC2 workflow definitions, ROS master lifecycle, MAVROS node
lifecycle, robot authorization policy, or a remotely reachable bridge.

## Build And Test

The supported clean build path runs source tests, creates the Debian package,
installs it in the same disposable container, and starts the installed binary
against a temporary ROS master:

```bash
.xgc2/scripts/check_cpp_quality.sh \
  --work-dir /tmp/xgc2-ros1-automation-gateway-quality

.xgc2/scripts/build_debs_in_docker.sh \
  --work-dir /tmp/xgc2-ros1-automation-gateway-build \
  --output-dir "$PWD/debs"
```

The source test suite covers primitive and compound ROS values, variable and
fixed arrays, binary arrays, nested values, time and duration, protocol
framing, malformed input, standard publish/service round trips, and MAVROS
message/service schemas.

## Release

Push CI builds and install-checks native `amd64` and `arm64`
packages and retains trusted build manifests for 14 days. The product
repository never receives APT credentials and never publishes repository
indexes. Production APT promotion is performed only by the centralized
`xgc2-devops` release orchestrator.

- Supported ROS/Ubuntu: Noetic on Focal
- Architectures: amd64, arm64
- CI workflows: `.github/workflows/ci.yml` and
  `.github/workflows/release.yml`
- APT repository: `https://xgc2.apt.xiaokang.ink`
