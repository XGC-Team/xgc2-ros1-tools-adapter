#!/usr/bin/env bash
set -euo pipefail

ROS_DISTRO="${ROS_DISTRO:-noetic}"
PREFIX="/opt/ros/${ROS_DISTRO}"
PACKAGE="ros-${ROS_DISTRO}-xgc2-ros1-automation-gateway"
ROS_PACKAGE="xgc_ros1_automation_gateway"
EXECUTABLE="${PREFIX}/lib/${ROS_PACKAGE}/${ROS_PACKAGE}_node"

dpkg -s "${PACKAGE}" >/dev/null

depends="$(dpkg-query -W -f='${Depends}' "${PACKAGE}")"
for dependency in \
  libjsoncpp1 \
  ros-noetic-mavros-msgs \
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

# shellcheck disable=SC1090
source "${PREFIX}/setup.bash"

test "$(rospack find "${ROS_PACKAGE}")" = \
  "${PREFIX}/share/${ROS_PACKAGE}"
test -f "${PREFIX}/share/${ROS_PACKAGE}/package.xml"
test -x "${EXECUTABLE}"

for elf in "${EXECUTABLE}"; do
  file -b "${elf}" | grep -q '^ELF'
  if ! ldd "${elf}" | awk '/not found/ {missing=1} END {exit missing ? 1 : 0}'; then
    echo "missing shared-library dependency in ${elf}" >&2
    ldd "${elf}" >&2 || true
    exit 1
  fi
done

runtime_dir="$(mktemp -d)"
roscore_pid=""
gateway_pid=""
cleanup() {
  if [[ -n "${gateway_pid}" ]]; then
    kill "${gateway_pid}" >/dev/null 2>&1 || true
    wait "${gateway_pid}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${roscore_pid}" ]]; then
    kill "${roscore_pid}" >/dev/null 2>&1 || true
    wait "${roscore_pid}" >/dev/null 2>&1 || true
  fi
  rm -rf "${runtime_dir}"
}
trap cleanup EXIT

export ROS_HOME="${runtime_dir}/ros-home"
export ROS_MASTER_URI="http://127.0.0.1:11311"
mkdir -p "${ROS_HOME}"

roscore >"${runtime_dir}/roscore.log" 2>&1 &
roscore_pid=$!
for _ in $(seq 1 100); do
  if rosparam list >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "${roscore_pid}" >/dev/null 2>&1; then
    cat "${runtime_dir}/roscore.log" >&2
    exit 1
  fi
  sleep 0.1
done
rosparam list >/dev/null

socket_path="${runtime_dir}/gateway.sock"
"${EXECUTABLE}" --socket "${socket_path}" \
  >"${runtime_dir}/gateway.log" 2>&1 &
gateway_pid=$!

for _ in $(seq 1 100); do
  if [[ -S "${socket_path}" ]]; then
    break
  fi
  if ! kill -0 "${gateway_pid}" >/dev/null 2>&1; then
    cat "${runtime_dir}/gateway.log" >&2
    exit 1
  fi
  sleep 0.1
done

if [[ ! -S "${socket_path}" ]]; then
  echo "installed gateway did not create its Unix socket" >&2
  cat "${runtime_dir}/gateway.log" >&2
  exit 1
fi

echo "Installed package check passed"
