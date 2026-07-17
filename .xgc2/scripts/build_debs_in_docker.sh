#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

DOCKER_IMAGE="${DOCKER_IMAGE:-ros:noetic-ros-base-focal}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/.work/docker}"
OUTPUT_DIR="${OUTPUT_DIR:-${REPO_ROOT}/debs}"
INSTALL_CHECK="${INSTALL_CHECK:-true}"
XGC2_DEPENDENCY_SET_DIGEST="${XGC2_DEPENDENCY_SET_DIGEST:-}"
ADAPTER_RUNTIME_CLIENT_DEB_VERSION="${ADAPTER_RUNTIME_CLIENT_DEB_VERSION:-0.5.0-1~focal}"
XGC2_PROTOBUF_DEB_VERSION="${XGC2_PROTOBUF_DEB_VERSION:-0.5.0-1~focal}"
XGC2_BOOTSTRAP_COMMON_FROM_GIT="${XGC2_BOOTSTRAP_COMMON_FROM_GIT:-true}"
XGC2_PROTOBUF_GIT_URL="${XGC2_PROTOBUF_GIT_URL:-https://github.com/lxk36/xgc2-protobuf.git}"
XGC2_PROTOBUF_GIT_TAG="${XGC2_PROTOBUF_GIT_TAG:-v0.5.0-1}"
XGC2_ADAPTER_RUNTIME_CLIENT_GIT_URL="${XGC2_ADAPTER_RUNTIME_CLIENT_GIT_URL:-https://github.com/lxk36/xgc2-adapter-runtime-client-cpp.git}"
XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG="${XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG:-v0.5.0-1}"

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

if [[ -n "${XGC2_DEPENDENCY_SET_DIGEST}" &&
      ! "${XGC2_DEPENDENCY_SET_DIGEST}" =~ ^[0-9a-f]{64}$ ]]; then
  echo "XGC2_DEPENDENCY_SET_DIGEST must be empty or 64 lowercase hex characters" >&2
  exit 1
fi
if [[ -n "${XGC2_APT_OVERLAY_URL:-}" ]]; then
  "${SCRIPT_DIR}/configure_xgc2_apt.sh" \
    --validate-url "${XGC2_APT_OVERLAY_URL}"
  if [[ -z "${XGC2_DEPENDENCY_SET_DIGEST}" ]]; then
    echo "XGC2_APT_OVERLAY_URL requires XGC2_DEPENDENCY_SET_DIGEST" >&2
    exit 1
  fi
fi

mkdir -p "${WORK_DIR}" "$(dirname "${OUTPUT_DIR}")"
OUTPUT_DIR="$(realpath -m "${OUTPUT_DIR}")"
mkdir -p "${OUTPUT_DIR}"
if find "${OUTPUT_DIR}" -mindepth 1 -name "*.deb" -print -quit | grep -q .; then
  echo "Debian output directory is not isolated: ${OUTPUT_DIR}" >&2
  echo "remove its existing Deb files or select an empty output directory" >&2
  exit 1
fi
STAGING_OUTPUT_DIR="$(mktemp -d "${OUTPUT_DIR}.staging.XXXXXX")"
cleanup() {
  rm -rf "${STAGING_OUTPUT_DIR}"
}
trap cleanup EXIT

docker_env_args=(
  -e "ADAPTER_RUNTIME_CLIENT_DEB_VERSION=${ADAPTER_RUNTIME_CLIENT_DEB_VERSION}"
  -e "XGC2_APT_OVERLAY_URL=${XGC2_APT_OVERLAY_URL:-}"
  -e "XGC2_PROTOBUF_DEB_VERSION=${XGC2_PROTOBUF_DEB_VERSION}"
  -e "XGC2_BOOTSTRAP_COMMON_FROM_GIT=${XGC2_BOOTSTRAP_COMMON_FROM_GIT}"
  -e "XGC2_PROTOBUF_GIT_URL=${XGC2_PROTOBUF_GIT_URL}"
  -e "XGC2_PROTOBUF_GIT_TAG=${XGC2_PROTOBUF_GIT_TAG}"
  -e "XGC2_ADAPTER_RUNTIME_CLIENT_GIT_URL=${XGC2_ADAPTER_RUNTIME_CLIENT_GIT_URL}"
  -e "XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG=${XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG}"
  -e "XGC2_DEPENDENCY_SET_DIGEST=${XGC2_DEPENDENCY_SET_DIGEST}"
  -e "DEBIAN_FRONTEND=noninteractive"
  -e "INSTALL_CHECK=${INSTALL_CHECK}"
)
for proxy_var in HTTP_PROXY HTTPS_PROXY NO_PROXY http_proxy https_proxy no_proxy; do
  if [[ -n "${!proxy_var:-}" ]]; then
    docker_env_args+=(-e "${proxy_var}=${!proxy_var}")
  fi
done

docker pull "${DOCKER_IMAGE}"
# The following single-quoted argument is parsed by the inner bash process.
# shellcheck disable=SC1004
docker run --rm \
  "${docker_env_args[@]}" \
  -v "${REPO_ROOT}:/workspace/source:ro" \
  -v "${WORK_DIR}:/workspace/work" \
  -v "${STAGING_OUTPUT_DIR}:/workspace/out" \
  "${DOCKER_IMAGE}" \
  bash -lc '
    set -euo pipefail

    export DEBIAN_FRONTEND=noninteractive
    /workspace/source/.xgc2/scripts/configure_xgc2_apt.sh focal
    apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      dpkg-dev \
      fakeroot \
      file \
      libjsoncpp-dev \
      pkg-config \
      ripgrep \
      rsync \
      ros-noetic-geometry-msgs \
      ros-noetic-ros-babel-fish \
      ros-noetic-roscpp \
      ros-noetic-roslaunch \
      ros-noetic-roslib \
      ros-noetic-rospack \
      ros-noetic-rostest \
      ros-noetic-rosunit \
      ros-noetic-std-msgs \
      ros-noetic-std-srvs \
      shellcheck

    /workspace/source/.xgc2/scripts/bootstrap_common_dependencies.sh

    if [[ -n "${XGC2_APT_OVERLAY_URL:-}" &&
          ! "${XGC2_DEPENDENCY_SET_DIGEST:-}" =~ ^[0-9a-f]{64}$ ]]; then
      echo "release-scoped APT build is missing its dependency-set digest" >&2
      exit 1
    fi
    if [[ -n "${XGC2_DEPENDENCY_SET_DIGEST:-}" ]]; then
      echo "Release dependency-set digest: ${XGC2_DEPENDENCY_SET_DIGEST}"
    fi
    test "$(dpkg-query -W -f="\${Version}" libxgc2-adapter-runtime-client-dev)" = \
      "${ADAPTER_RUNTIME_CLIENT_DEB_VERSION}"
    test "$(dpkg-query -W -f="\${Version}" xgc2-protobuf-dev)" = \
      "${XGC2_PROTOBUF_DEB_VERSION}"

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

    /workspace/source/.xgc2/scripts/check_package_compliance.sh

    catkin_make run_tests_xgc_ros1_tools_adapter \
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
        /workspace/out/ros-noetic-xgc2-ros1-tools-adapter_*.deb
      /workspace/source/.xgc2/scripts/check_installed_packages.sh
    fi
  '

mapfile -t built_debs < <(
  find "${STAGING_OUTPUT_DIR}" -maxdepth 1 -type f \
    -name "ros-noetic-xgc2-ros1-tools-adapter_*.deb" -print | sort
)
if [[ "${#built_debs[@]}" -ne 1 ]]; then
  echo "expected exactly one staged ROS1 Tools Adapter Deb, found ${#built_debs[@]}" >&2
  exit 1
fi
if find "${STAGING_OUTPUT_DIR}" -mindepth 1 -maxdepth 1 \
  ! -path "${built_debs[0]}" -print -quit | grep -q .; then
  echo "staged output contains an unexpected artifact" >&2
  find "${STAGING_OUTPUT_DIR}" -mindepth 1 -maxdepth 1 -print >&2
  exit 1
fi

if find "${OUTPUT_DIR}" -mindepth 1 -name "*.deb" -print -quit | grep -q .; then
  echo "Debian output directory changed during the build: ${OUTPUT_DIR}" >&2
  exit 1
fi
final_deb="${OUTPUT_DIR}/$(basename "${built_debs[0]}")"
install -m 0644 "${built_debs[0]}" "${final_deb}"
mapfile -t final_debs < <(
  find "${OUTPUT_DIR}" -mindepth 1 -name "*.deb" -print | sort
)
if [[ "${#final_debs[@]}" -ne 1 || "${final_debs[0]}" != "${final_deb}" ]]; then
  echo "Debian output directory lost its unique product identity" >&2
  exit 1
fi

echo "Debian package output:"
echo "${final_deb}"
