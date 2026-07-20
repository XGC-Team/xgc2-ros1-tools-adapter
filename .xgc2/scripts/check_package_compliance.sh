#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
EXPECTED_ADAPTER_RUNTIME_CLIENT_DEB_VERSION="0.5.0-2~focal"
EXPECTED_XGC2_PROTOBUF_DEB_VERSION="0.5.0-1~focal"
EXPECTED_PROTOBUF_GIT_TAG="v0.5.0-1"
EXPECTED_RUNTIME_GIT_TAG="v0.5.0-2"
BOOTSTRAP_SCRIPT="${SCRIPT_DIR}/bootstrap_common_dependencies.sh"
PACKAGE_SCRIPT="${SCRIPT_DIR}/package_debs.sh"

for tool in bash python3 rg shellcheck; do
  command -v "${tool}" >/dev/null || {
    echo "missing required tool: ${tool}" >&2
    exit 1
  }
done

grep -q '^id: xgc2-ros1-tools-adapter$' "${REPO_ROOT}/.xgc2/product.yml"
grep -q '^name: XGC2 ROS1 Tools Adapter$' "${REPO_ROOT}/.xgc2/product.yml"
grep -q '<name>xgc_ros1_tools_adapter</name>' \
  "${REPO_ROOT}/src/xgc_ros1_tools_adapter/package.xml"
grep -q 'find_package(xgc2_adapter_runtime_client 0.5.0 EXACT REQUIRED CONFIG)' \
  "${REPO_ROOT}/src/xgc_ros1_tools_adapter/CMakeLists.txt"
grep -q 'xgc2::adapter_runtime_client' \
  "${REPO_ROOT}/src/xgc_ros1_tools_adapter/CMakeLists.txt"
grep -q '^    - libxgc2-adapter-runtime-client1$' \
  "${REPO_ROOT}/.xgc2/product.yml"
grep -q '^    libxgc2-adapter-runtime-client-dev: verify$' \
  "${REPO_ROOT}/.xgc2/product.yml"
grep -q '^    xgc2-protobuf: verify$' "${REPO_ROOT}/.xgc2/product.yml"

test -x "${BOOTSTRAP_SCRIPT}"
grep -Fq \
  "EXPECTED_ADAPTER_RUNTIME_CLIENT_DEB_VERSION=\"${EXPECTED_ADAPTER_RUNTIME_CLIENT_DEB_VERSION}\"" \
  "${BOOTSTRAP_SCRIPT}"
grep -Fq \
  "EXPECTED_XGC2_PROTOBUF_DEB_VERSION=\"${EXPECTED_XGC2_PROTOBUF_DEB_VERSION}\"" \
  "${BOOTSTRAP_SCRIPT}"
grep -Fq \
  "EXPECTED_XGC2_PROTOBUF_GIT_TAG=\"${EXPECTED_PROTOBUF_GIT_TAG}\"" \
  "${BOOTSTRAP_SCRIPT}"
grep -Fq \
  "EXPECTED_XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG=\"${EXPECTED_RUNTIME_GIT_TAG}\"" \
  "${BOOTSTRAP_SCRIPT}"
grep -Fq 'mktemp -d /tmp/xgc2-common-bootstrap.XXXXXX' \
  "${BOOTSTRAP_SCRIPT}"
grep -Fq 'trap cleanup_bootstrap_work EXIT' "${BOOTSTRAP_SCRIPT}"
if rg -n 'XGC2_COMMON_BOOTSTRAP_WORK_DIR' "${BOOTSTRAP_SCRIPT}"; then
  echo "common dependency bootstrap must not accept a caller-selected delete path" >&2
  exit 1
fi
grep -Fq 'ADAPTER_RUNTIME_ABI_PACKAGE="libxgc2-adapter-runtime-client1"' \
  "${PACKAGE_SCRIPT}"
grep -Fq 'dpkg-shlibdeps -O' \
  "${PACKAGE_SCRIPT}"
grep -Fq \
  "copy_path \"\${PREFIX_ROOT}/lib/\${ROS_PACKAGE}/\${ROS_PACKAGE}_service_helper\"" \
  "${PACKAGE_SCRIPT}"
grep -Fq "Conflicts: \${REMOVED_PACKAGE}" "${PACKAGE_SCRIPT}"
grep -Fq "Replaces: \${REMOVED_PACKAGE}" "${PACKAGE_SCRIPT}"
grep -Fq 'libxgc2_adapter_runtime_client[.]so[.]1' \
  "${SCRIPT_DIR}/check_installed_packages.sh"
grep -Fq 'libxgc2_adapter_runtime_protocol[.]so[.]1' \
  "${SCRIPT_DIR}/check_installed_packages.sh"
grep -Fq "test -x \"\${SERVICE_HELPER}\"" \
  "${SCRIPT_DIR}/check_installed_packages.sh"
grep -Fq '/bootstrap_common_dependencies.sh' \
  "${SCRIPT_DIR}/build_debs_in_docker.sh"
grep -Fq '/workspace/work/.xgc2/scripts/check_package_compliance.sh' \
  "${SCRIPT_DIR}/build_debs_in_docker.sh"
grep -Fq '/bootstrap_common_dependencies.sh' \
  "${REPO_ROOT}/.github/workflows/ci.yml"
grep -Fq '/bootstrap_common_dependencies.sh' \
  "${REPO_ROOT}/.github/workflows/release.yml"

removed_identity_pattern='automation[._-]gate''way|\bgate''way\b|mav''ros|adapter[_ -]?li''nk|--sock''et'
if rg -n -i \
  "${removed_identity_pattern}" \
  "${REPO_ROOT}/README.md" \
  "${REPO_ROOT}/src" \
  "${REPO_ROOT}/schemas" \
  "${REPO_ROOT}/tools" \
  "${REPO_ROOT}/.xgc2/product.yml" \
  "${REPO_ROOT}/.xgc2/scripts" \
  "${REPO_ROOT}/.github"; then
  echo "removed product/protocol identity remains in the source tree" >&2
  exit 1
fi

removed_sdk_version_pattern='0[.]'"4"
if rg -n \
  "${removed_sdk_version_pattern}" \
  "${REPO_ROOT}/README.md" \
  "${REPO_ROOT}/src" \
  "${REPO_ROOT}/schemas" \
  "${REPO_ROOT}/tools" \
  "${REPO_ROOT}/.xgc2" \
  "${REPO_ROOT}/.github"; then
  echo "removed Adapter Runtime dependency version remains in the source tree" >&2
  exit 1
fi

test ! -e "${REPO_ROOT}/src/xgc_ros1_tools_adapter/include/xgc_ros1_tools_adapter/request_cache.hpp"
test ! -e "${REPO_ROOT}/src/xgc_ros1_tools_adapter/include/xgc_ros1_tools_adapter/uds_server.hpp"
test ! -e "${REPO_ROOT}/src/xgc_ros1_tools_adapter/src/request_cache.cpp"
test ! -e "${REPO_ROOT}/src/xgc_ros1_tools_adapter/src/uds_server.cpp"

for script in "${REPO_ROOT}"/.xgc2/scripts/*.sh; do
  bash -n "${script}"
done
shellcheck "${REPO_ROOT}"/.xgc2/scripts/*.sh
python3 -m py_compile \
  "${REPO_ROOT}/tools/generate_contract_metadata.py" \
  "${REPO_ROOT}/tools/generate_runtime_manifests.py" \
  "${REPO_ROOT}/tools/verify_runtime_manifests.py" \
  "${REPO_ROOT}/.xgc2/scripts/xgc2_artifact_manifest.py"
python3 -m unittest discover -v -s "${REPO_ROOT}/test" -p 'test_*.py'
python3 - "${REPO_ROOT}/src/xgc_ros1_tools_adapter/package.xml" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
if root.findtext("name") != "xgc_ros1_tools_adapter":
    raise SystemExit("ROS package identity mismatch")
if root.findtext("version") != "0.1.2":
    raise SystemExit("ROS package version mismatch")
dependencies = {node.text for node in root if node.tag.endswith("depend")}
if "libxgc2-adapter-runtime-client-dev" not in dependencies:
    raise SystemExit("Adapter Runtime SDK dependency is missing")
if "libxgc2-adapter-runtime-client1" not in dependencies:
    raise SystemExit("Adapter Runtime ABI dependency is missing")
specialized_package = "mav" + "ros"
if any(specialized_package in dependency for dependency in dependencies if dependency):
    raise SystemExit("ROS Tools must not depend on a robot-specialized package")
PY
for schema in "${REPO_ROOT}"/schemas/*.schema.json; do
  python3 -m json.tool "${schema}" >/dev/null
done

temporary="$(mktemp -d)"
cleanup() {
  rm -rf "${temporary}"
}
trap cleanup EXIT

removed_deb_version="0.""4.0-1~focal"
if ADAPTER_RUNTIME_CLIENT_DEB_VERSION="${removed_deb_version}" \
  "${BOOTSTRAP_SCRIPT}" >"${temporary}/old-deb.out" 2>"${temporary}/old-deb.err"; then
  echo "common dependency bootstrap accepted a removed Runtime client version" >&2
  exit 1
fi
grep -Fq \
  "must be exactly ${EXPECTED_ADAPTER_RUNTIME_CLIENT_DEB_VERSION}" \
  "${temporary}/old-deb.err"

removed_git_tag="v0.""4.0-1"
if XGC2_ADAPTER_RUNTIME_CLIENT_GIT_TAG="${removed_git_tag}" \
  "${BOOTSTRAP_SCRIPT}" >"${temporary}/old-tag.out" 2>"${temporary}/old-tag.err"; then
  echo "common dependency bootstrap accepted a removed Runtime client tag" >&2
  exit 1
fi
grep -Fq "must be exactly ${EXPECTED_RUNTIME_GIT_TAG}" \
  "${temporary}/old-tag.err"

python3 "${REPO_ROOT}/tools/generate_runtime_manifests.py" \
  --executable /bin/true \
  --artifact-path /opt/ros/noetic/lib/xgc_ros1_tools_adapter/xgc_ros1_tools_adapter_node \
  --schema-dir "${REPO_ROOT}/schemas" \
  --version 0.1.2 \
  --adapter-output "${temporary}/adapter.json" \
  --process-output "${temporary}/process.json"
python3 "${REPO_ROOT}/tools/verify_runtime_manifests.py" \
  --executable /bin/true \
  --artifact-path /opt/ros/noetic/lib/xgc_ros1_tools_adapter/xgc_ros1_tools_adapter_node \
  --schema-dir "${REPO_ROOT}/schemas" \
  --adapter-manifest "${temporary}/adapter.json" \
  --process-manifest "${temporary}/process.json"

python3 - "${temporary}/adapter.json" "${temporary}/tampered-adapter.json" <<'PY'
import json
import sys

source, destination = sys.argv[1:]
document = json.load(open(source, encoding="utf-8"))
document["adapters"][0]["definition"]["artifact"] = "/tmp/removed-legacy-field"
with open(destination, "w", encoding="utf-8") as stream:
    json.dump(document, stream)
PY
if PYTHONOPTIMIZE=1 python3 "${REPO_ROOT}/tools/verify_runtime_manifests.py" \
  --executable /bin/true \
  --artifact-path /opt/ros/noetic/lib/xgc_ros1_tools_adapter/xgc_ros1_tools_adapter_node \
  --schema-dir "${REPO_ROOT}/schemas" \
  --adapter-manifest "${temporary}/tampered-adapter.json" \
  --process-manifest "${temporary}/process.json" >/dev/null 2>&1; then
  echo "optimized Python accepted a tampered Runtime manifest" >&2
  exit 1
fi
if rg -n '^\s*assert\b' "${REPO_ROOT}/tools/verify_runtime_manifests.py"; then
  echo "Runtime manifest verification must not depend on removable Python assertions" >&2
  exit 1
fi

echo "Package compliance check passed"
