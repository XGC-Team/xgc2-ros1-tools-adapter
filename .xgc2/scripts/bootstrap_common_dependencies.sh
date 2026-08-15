#!/usr/bin/env bash
set -euo pipefail

EXPECTED_ADAPTER_RUNTIME_CLIENT_DEB_VERSION="0.6.0-1~focal"
EXPECTED_XGC2_PROTOBUF_DEB_VERSION="0.5.0-3~focal"
EXPECTED_XGC2_PROTOBUF_GIT_TAG="v0.5.0-3"
EXPECTED_XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG="v0.6.0-1"

ADAPTER_RUNTIME_CLIENT_DEB_VERSION="${ADAPTER_RUNTIME_CLIENT_DEB_VERSION:-}"
XGC2_PROTOBUF_DEB_VERSION="${XGC2_PROTOBUF_DEB_VERSION:-}"
XGC2_PROTOBUF_GIT_TAG="${XGC2_PROTOBUF_GIT_TAG:-${EXPECTED_XGC2_PROTOBUF_GIT_TAG}}"
XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG="${XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG:-${EXPECTED_XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG}}"
XGC2_PROTOBUF_GIT_URL="${XGC2_PROTOBUF_GIT_URL:-https://github.com/XGC-Team/xgc2-protobuf.git}"
XGC2_ADAPTER_RUNTIME_CLIENT_GIT_URL="${XGC2_ADAPTER_RUNTIME_CLIENT_GIT_URL:-https://github.com/XGC-Team/xgc2-adapter-runtime-client-cpp.git}"
XGC2_BOOTSTRAP_COMMON_FROM_GIT="${XGC2_BOOTSTRAP_COMMON_FROM_GIT:-false}"
BOOTSTRAP_WORK_DIR=""

cleanup_bootstrap_work() {
  if [[ -n "${BOOTSTRAP_WORK_DIR}" &&
        -d "${BOOTSTRAP_WORK_DIR}" &&
        "${BOOTSTRAP_WORK_DIR}" == /tmp/xgc2-common-bootstrap.?????? ]]; then
    rm -rf -- "${BOOTSTRAP_WORK_DIR}"
  fi
}
trap cleanup_bootstrap_work EXIT

require_exact() {
  local name="$1"
  local actual="$2"
  local expected="$3"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "${name} must be exactly ${expected}; got ${actual}" >&2
    exit 1
  fi
}

case "${XGC2_BOOTSTRAP_COMMON_FROM_GIT}" in
  true|false) ;;
  *)
    echo "XGC2_BOOTSTRAP_COMMON_FROM_GIT must be true or false" >&2
    exit 1
    ;;
esac

if [[ "${XGC2_BOOTSTRAP_COMMON_FROM_GIT}" == "true" ]]; then
  ADAPTER_RUNTIME_CLIENT_DEB_VERSION="${ADAPTER_RUNTIME_CLIENT_DEB_VERSION:-${EXPECTED_ADAPTER_RUNTIME_CLIENT_DEB_VERSION}}"
  XGC2_PROTOBUF_DEB_VERSION="${XGC2_PROTOBUF_DEB_VERSION:-${EXPECTED_XGC2_PROTOBUF_DEB_VERSION}}"
  require_exact \
    "ADAPTER_RUNTIME_CLIENT_DEB_VERSION" \
    "${ADAPTER_RUNTIME_CLIENT_DEB_VERSION}" \
    "${EXPECTED_ADAPTER_RUNTIME_CLIENT_DEB_VERSION}"
  require_exact \
    "XGC2_PROTOBUF_DEB_VERSION" \
    "${XGC2_PROTOBUF_DEB_VERSION}" \
    "${EXPECTED_XGC2_PROTOBUF_DEB_VERSION}"
  require_exact \
    "XGC2_PROTOBUF_GIT_TAG" \
    "${XGC2_PROTOBUF_GIT_TAG}" \
    "${EXPECTED_XGC2_PROTOBUF_GIT_TAG}"
  require_exact \
    "XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG" \
    "${XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG}" \
    "${EXPECTED_XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG}"

  apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    dpkg-dev \
    fakeroot \
    git \
    libgrpc++-dev \
    libprotobuf-dev \
    libre2-dev \
    pkg-config \
    protobuf-compiler \
    protobuf-compiler-grpc \
    python3-protobuf \
    python3-yaml

  BOOTSTRAP_WORK_DIR="$(mktemp -d /tmp/xgc2-common-bootstrap.XXXXXX)"
  mkdir -p "${BOOTSTRAP_WORK_DIR}/debs/protobuf" \
    "${BOOTSTRAP_WORK_DIR}/debs/client"

  git clone --depth 1 --branch "${XGC2_PROTOBUF_GIT_TAG}" \
    "${XGC2_PROTOBUF_GIT_URL}" \
    "${BOOTSTRAP_WORK_DIR}/protobuf"
  require_exact \
    "checked-out XGC2 protobuf tag" \
    "$(git -C "${BOOTSTRAP_WORK_DIR}/protobuf" describe --tags --exact-match)" \
    "${XGC2_PROTOBUF_GIT_TAG}"
  PACKAGE_DISTRIBUTION=focal \
  PACKAGE_VERSION="${XGC2_PROTOBUF_DEB_VERSION}" \
  XGC2_PROTOBUF_DEB_OUTPUT_DIR="${BOOTSTRAP_WORK_DIR}/debs/protobuf" \
    "${BOOTSTRAP_WORK_DIR}/protobuf/.xgc2/scripts/build_deb.sh"
  apt-get install -y \
    "${BOOTSTRAP_WORK_DIR}"/debs/protobuf/xgc2-protobuf-dev_*.deb

  git clone --depth 1 --branch "${XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG}" \
    "${XGC2_ADAPTER_RUNTIME_CLIENT_GIT_URL}" \
    "${BOOTSTRAP_WORK_DIR}/adapter-runtime-client-cpp"
  require_exact \
    "checked-out Adapter Runtime client tag" \
    "$(git -C "${BOOTSTRAP_WORK_DIR}/adapter-runtime-client-cpp" describe --tags --exact-match)" \
    "${XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG}"
  PACKAGE_DISTRIBUTION=focal \
  PACKAGE_VERSION="${ADAPTER_RUNTIME_CLIENT_DEB_VERSION}" \
  XGC2_PROTOBUF_DEB_VERSION="${XGC2_PROTOBUF_DEB_VERSION}" \
  XGC2_ADAPTER_RUNTIME_DEB_OUTPUT_DIR="${BOOTSTRAP_WORK_DIR}/debs/client" \
    "${BOOTSTRAP_WORK_DIR}/adapter-runtime-client-cpp/.xgc2/scripts/build_deb.sh"
  apt-get install -y \
    "${BOOTSTRAP_WORK_DIR}"/debs/client/libxgc2-adapter-runtime-client2_*.deb \
    "${BOOTSTRAP_WORK_DIR}"/debs/client/libxgc2-adapter-runtime-client-dev_*.deb
else
  apt_candidate_version() {
    local package="$1"
    local candidate
    candidate="$(apt-cache policy "${package}" | awk '/Candidate:/ {print $2; exit}')"
    if [[ -z "${candidate}" || "${candidate}" == "(none)" ]]; then
      echo "APT has no candidate for ${package}" >&2
      exit 1
    fi
    printf '%s\n' "${candidate}"
  }
  ADAPTER_RUNTIME_CLIENT_DEB_VERSION="${ADAPTER_RUNTIME_CLIENT_DEB_VERSION:-$(apt_candidate_version libxgc2-adapter-runtime-client-dev)}"
  XGC2_PROTOBUF_DEB_VERSION="${XGC2_PROTOBUF_DEB_VERSION:-$(apt_candidate_version xgc2-protobuf-dev)}"
  apt-get install -y \
    "xgc2-protobuf-dev=${XGC2_PROTOBUF_DEB_VERSION}" \
    "libxgc2-adapter-runtime-client-dev=${ADAPTER_RUNTIME_CLIENT_DEB_VERSION}"
fi

installed_client_version="$(
  dpkg-query -W -f='${Version}' libxgc2-adapter-runtime-client-dev
)"
installed_runtime_version="$(
  dpkg-query -W -f='${Version}' libxgc2-adapter-runtime-client2
)"
installed_protobuf_version="$(dpkg-query -W -f='${Version}' xgc2-protobuf-dev)"
require_exact \
  "installed Adapter Runtime client" \
  "${installed_client_version}" \
  "${ADAPTER_RUNTIME_CLIENT_DEB_VERSION}"
require_exact \
  "installed Adapter Runtime ABI" \
  "${installed_runtime_version}" \
  "${ADAPTER_RUNTIME_CLIENT_DEB_VERSION}"
require_exact \
  "installed XGC2 protobuf" \
  "${installed_protobuf_version}" \
  "${XGC2_PROTOBUF_DEB_VERSION}"

version_header="/usr/include/xgc2/adapter_runtime/version.hpp"
test -f "${version_header}"
grep -q '^#define XGC2_ADAPTER_RUNTIME_CLIENT_VERSION_MAJOR 0$' "${version_header}"
grep -q '^#define XGC2_ADAPTER_RUNTIME_CLIENT_VERSION_MINOR 6$' "${version_header}"
grep -q '^#define XGC2_ADAPTER_RUNTIME_CLIENT_VERSION_PATCH 0$' "${version_header}"
grep -q '^#define XGC2_ADAPTER_RUNTIME_CLIENT_ABI_VERSION 2$' "${version_header}"
grep -q '^constexpr std::uint32_t kAdapterBootstrapFormatVersion = 2;$' \
  "${version_header}"
grep -q '^constexpr std::uint32_t kRuntimeLinkProtocolVersion = 2;$' \
  "${version_header}"

echo "Exact XGC2 Proto and Adapter Runtime dependencies are installed"
