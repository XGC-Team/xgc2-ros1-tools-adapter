#!/usr/bin/env bash
set -euo pipefail

ADAPTER_RUNTIME_CLIENT_DEB_VERSION="${ADAPTER_RUNTIME_CLIENT_DEB_VERSION:-}"
XGC2_PROTOBUF_DEB_VERSION="${XGC2_PROTOBUF_DEB_VERSION:-}"

require_exact() {
  local name="$1"
  local actual="$2"
  local expected="$3"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "${name} must be exactly ${expected}; got ${actual}" >&2
    exit 1
  fi
}

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
