#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

DOCKER_IMAGE="${DOCKER_IMAGE:-ros:noetic-ros-base-focal}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/.work/docker}"
OUTPUT_DIR="${OUTPUT_DIR:-${REPO_ROOT}/debs}"
INSTALL_CHECK="${INSTALL_CHECK:-true}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --image)
      DOCKER_IMAGE="$2"
      shift 2
      ;;
    --work-dir)
      WORK_DIR="$2"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --skip-install-check)
      INSTALL_CHECK=false
      shift
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

mkdir -p "${WORK_DIR}" "${OUTPUT_DIR}"
rm -f "${OUTPUT_DIR}/ros-noetic-xgc2-ros1-automation-gateway_"*.deb

docker pull "${DOCKER_IMAGE}"
# The following single-quoted argument is parsed by the inner bash process.
# shellcheck disable=SC1004
docker run --rm \
  -e XGC2_APT_OVERLAY_URL="${XGC2_APT_OVERLAY_URL:-}" \
  -e DEBIAN_FRONTEND=noninteractive \
  -e INSTALL_CHECK="${INSTALL_CHECK}" \
  -v "${REPO_ROOT}:/workspace/source:ro" \
  -v "${WORK_DIR}:/workspace/work" \
  -v "${OUTPUT_DIR}:/workspace/out" \
  "${DOCKER_IMAGE}" \
  bash -lc '
    set -euo pipefail

    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      dpkg-dev \
      fakeroot \
      file \
      libjsoncpp-dev \
      pkg-config \
      rsync \
      ros-noetic-mavros-msgs \
      ros-noetic-ros-babel-fish \
      ros-noetic-roscpp \
      ros-noetic-roslaunch \
      ros-noetic-roslib \
      ros-noetic-rospack \
      ros-noetic-rostest \
      ros-noetic-rosunit \
      ros-noetic-std-msgs \
      ros-noetic-std-srvs

    rm -rf \
      /workspace/work/src \
      /workspace/work/build \
      /workspace/work/devel \
      /workspace/work/install \
      /workspace/work/install-root
    rsync -a --delete \
      --exclude .git \
      --exclude .ci \
      --exclude .work \
      --exclude build \
      --exclude debs \
      --exclude devel \
      --exclude install \
      --exclude install-root \
      /workspace/source/ /workspace/work/

    cd /workspace/work
    source /opt/ros/noetic/setup.bash

    catkin_make run_tests_xgc_ros1_automation_gateway \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
    catkin_test_results --verbose build/test_results

    DESTDIR=/workspace/work/install-root catkin_make install \
      -DCMAKE_INSTALL_PREFIX=/opt/ros/noetic \
      -DCMAKE_BUILD_TYPE=Release \
      -DCATKIN_ENABLE_TESTING=OFF \
      -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" \
      -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG"

    /workspace/source/.xgc2/scripts/package_debs.sh \
      --install-root /workspace/work/install-root \
      --output-dir /workspace/out

    if [[ "$INSTALL_CHECK" == "true" ]]; then
      apt-get install -y \
        /workspace/out/ros-noetic-xgc2-ros1-automation-gateway_*.deb
      /workspace/source/.xgc2/scripts/check_installed_packages.sh
    fi
  '

echo "Debian package output:"
find "${OUTPUT_DIR}" -maxdepth 1 -type f -name "*.deb" -print | sort
