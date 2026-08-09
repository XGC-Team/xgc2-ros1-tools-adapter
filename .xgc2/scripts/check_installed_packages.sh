#!/usr/bin/env bash
set -euo pipefail

ROS_DISTRO="${ROS_DISTRO:-noetic}"
PREFIX="/opt/ros/${ROS_DISTRO}"
PACKAGE="ros-${ROS_DISTRO}-xgc2-ros1-tools-adapter"
ROS_PACKAGE="xgc_ros1_tools_adapter"
EXECUTABLE="${PREFIX}/lib/${ROS_PACKAGE}/${ROS_PACKAGE}_node"
SERVICE_HELPER="${PREFIX}/lib/${ROS_PACKAGE}/${ROS_PACKAGE}_service_helper"
ADAPTER_MANIFEST="/usr/share/xgc2/adapter-definitions/xgc2-ros1-tools-adapter.json"
PROCESS_MANIFEST="/usr/share/xgc2/process-definitions/xgc2-ros1-tools-adapter.json"
ADAPTER_RUNTIME_CLIENT_DEB_VERSION="${ADAPTER_RUNTIME_CLIENT_DEB_VERSION:-$(
  dpkg-query -W -f='${Version}' libxgc2-adapter-runtime-client2
)}"
REMOVED_PACKAGE="ros-${ROS_DISTRO}-xgc2-ros1-automation-"'gate'"way"
REMOVED_ROS_PACKAGE="xgc_ros1_automation_"'gate'"way"
REMOVED_DEFINITION="xgc2-ros1-automation-"'gate'"way"

dpkg -s "${PACKAGE}" >/dev/null
dpkg -s libxgc2-adapter-runtime-client2 >/dev/null
test "$(dpkg-query -W -f='${Version}' libxgc2-adapter-runtime-client2)" = \
  "${ADAPTER_RUNTIME_CLIENT_DEB_VERSION}"
if dpkg -s "${REMOVED_PACKAGE}" >/dev/null 2>&1; then
  echo "removed ROS1 automation package is still installed" >&2
  exit 1
fi

depends="$(dpkg-query -W -f='${Depends}' "${PACKAGE}")"
for dependency in \
  libjsoncpp1 \
  libxgc2-adapter-runtime-client2 \
  ros-noetic-ros-babel-fish \
  ros-noetic-roscpp \
  ros-noetic-roslib \
  ros-noetic-std-msgs \
  ros-noetic-std-srvs; do
  grep -Eq "(^|, )[[:space:]]*${dependency}([[:space:](,]|$)" <<<"${depends}" || {
    echo "missing declared runtime dependency: ${dependency}" >&2
    exit 1
  }
done
if grep -Eq '(^|, )(libxgc2-adapter-runtime-client-dev|xgc2-protobuf-dev)( |[(,]|$)' \
    <<<"${depends}"; then
  echo "runtime package leaked SDK/schema dependencies" >&2
  exit 1
fi

# shellcheck disable=SC1090
source "${PREFIX}/setup.bash"

test "$(rospack find "${ROS_PACKAGE}")" = "${PREFIX}/share/${ROS_PACKAGE}"
test -f "${PREFIX}/share/${ROS_PACKAGE}/package.xml"
test -x "${EXECUTABLE}"
test -x "${SERVICE_HELPER}"
test -f "${ADAPTER_MANIFEST}"
test -f "${PROCESS_MANIFEST}"
file -b "${EXECUTABLE}" | grep -q '^ELF'
file -b "${SERVICE_HELPER}" | grep -q '^ELF'
if ! ldd "${EXECUTABLE}" | awk '/not found/ {missing=1} END {exit missing ? 1 : 0}'; then
  echo "missing shared-library dependency in ${EXECUTABLE}" >&2
  ldd "${EXECUTABLE}" >&2 || true
  exit 1
fi
if ! ldd "${SERVICE_HELPER}" | awk '/not found/ {missing=1} END {exit missing ? 1 : 0}'; then
  echo "missing shared-library dependency in ${SERVICE_HELPER}" >&2
  ldd "${SERVICE_HELPER}" >&2 || true
  exit 1
fi
runtime_libraries="$(ldd "${EXECUTABLE}")"
grep -Eq 'libxgc2_adapter_runtime_client[.]so[.]2 => /' \
  <<<"${runtime_libraries}"
grep -Eq 'libxgc2_adapter_runtime_protocol[.]so[.]2 => /' \
  <<<"${runtime_libraries}"
if grep -Eq 'libxgc2_adapter_runtime_(client|protocol)[.]so[.](0|1)([[:space:]]|$)' \
  <<<"${runtime_libraries}"; then
  echo "installed Adapter executable links a removed Runtime client ABI" >&2
  exit 1
fi

"${EXECUTABLE}" --help | grep -q -- '--adapter-bootstrap-file'
rosrun --prefix /bin/echo "${ROS_PACKAGE}" "${ROS_PACKAGE}_node" \
  | grep -Fq "${EXECUTABLE}"
removed_private_flag="--sock""et"
if "${EXECUTABLE}" "${removed_private_flag}" /tmp/forbidden.sock >/dev/null 2>&1; then
  echo "removed private socket argument was accepted" >&2
  exit 1
fi

test ! -e "${PREFIX}/share/${REMOVED_ROS_PACKAGE}"
test ! -e "${PREFIX}/lib/${REMOVED_ROS_PACKAGE}"
test ! -e "/usr/share/xgc2/adapter-definitions/${REMOVED_DEFINITION}.json"
test ! -e "/usr/share/xgc2/process-definitions/${REMOVED_DEFINITION}.json"

python3 - "${EXECUTABLE}" "${ADAPTER_MANIFEST}" "${PROCESS_MANIFEST}" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

executable, adapter_path, process_path = map(Path, sys.argv[1:])
adapter = json.loads(adapter_path.read_text(encoding="utf-8"))
process = json.loads(process_path.read_text(encoding="utf-8"))
installed = adapter["adapters"][0]
definition = installed["definition"]
actual_digest = "sha256:" + hashlib.sha256(executable.read_bytes()).hexdigest()
def require(condition, message):
    if not condition:
        raise SystemExit(message)

require(adapter["apiVersion"] == "xgc.adapter.definition/v1", "Adapter API version mismatch")
require(definition["id"] == "xgc2-ros1-tools-adapter", "Adapter identity mismatch")
require(definition["buildDigest"] == actual_digest, "Adapter build digest mismatch")
capabilities = installed["capabilityManifest"]["capabilities"]
require(
    {(item["ref"]["id"], item["endpoints"][0]["endpointId"]) for item in capabilities}
    == {
        ("xgc.ros1.topic.publish", "publish"),
        ("xgc.ros1.service.call", "call"),
    },
    "ROS Tools capability set mismatch",
)
entry = process["definitions"][0]
require(process["apiVersion"] == "xgc.execution.process/v1", "process API version mismatch")
require(entry["id"] == "xgc2-ros1-tools-adapter" and entry["internal"] is True, "internal process identity mismatch")
require(entry["command"]["executable"] == "rosrun", "process executable mismatch")
require(entry["command"]["args"] == [
    "xgc_ros1_tools_adapter",
    "xgc_ros1_tools_adapter_node",
    "--adapter-bootstrap-file",
    "${adapterBootstrapFile}",
], "process bootstrap arguments mismatch")
require(entry["command"].get("directExecutable", False) is False, "direct executable bypass returned")
PY

echo "Installed package check passed"
