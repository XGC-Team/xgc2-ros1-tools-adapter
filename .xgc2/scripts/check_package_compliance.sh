#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BOOTSTRAP_SCRIPT="${SCRIPT_DIR}/bootstrap_common_dependencies.sh"
PACKAGE_SCRIPT="${SCRIPT_DIR}/package_debs.sh"
temporary="$(mktemp -d)"

cleanup() {
  rm -rf "${temporary}"
}
trap cleanup EXIT

for tool in bash python3 rg shellcheck; do
  command -v "${tool}" >/dev/null || {
    echo "missing required tool: ${tool}" >&2
    exit 1
  }
done

python3 - "${REPO_ROOT}/.xgc2/product.yml" <<'PY'
import sys
from pathlib import Path

import yaml

product_path = Path(sys.argv[1])
product = yaml.safe_load(product_path.read_text(encoding="utf-8"))
if not isinstance(product, dict):
    raise SystemExit(f"{product_path}: product metadata must be a mapping")
if product.get("id") != "xgc2-ros1-tools-adapter":
    raise SystemExit(f"{product_path}: product id mismatch")
if product.get("name") != "XGC2 ROS1 Tools Adapter":
    raise SystemExit(f"{product_path}: product name mismatch")

apt = product.get("apt")
if not isinstance(apt, dict):
    raise SystemExit(f"{product_path}: apt metadata must be a mapping")
depends = apt.get("depends")
if not isinstance(depends, list):
    raise SystemExit(f"{product_path}: apt.depends must be a list")
if "libxgc2-adapter-runtime-client2" not in depends:
    raise SystemExit(
        f"{product_path}: apt.depends is missing libxgc2-adapter-runtime-client2"
    )

release = product.get("release")
if not isinstance(release, dict):
    raise SystemExit(f"{product_path}: release metadata must be a mapping")
dependency_policy = release.get("dependency_policy")
if not isinstance(dependency_policy, dict):
    raise SystemExit(f"{product_path}: release.dependency_policy must be a mapping")
expected_policy = {
    "libxgc2-adapter-runtime-client-dev": "rebuild",
    "xgc2-protobuf": "rebuild",
}
for dependency, expected in expected_policy.items():
    actual = dependency_policy.get(dependency)
    if actual != expected:
        raise SystemExit(
            f"{product_path}: dependency policy for {dependency} must be {expected!r}, "
            f"got {actual!r}"
        )
PY
grep -q '<name>xgc_ros1_tools_adapter</name>' \
  "${REPO_ROOT}/src/xgc_ros1_tools_adapter/package.xml"
grep -q 'find_package(xgc2_adapter_runtime_client 0.6.0 EXACT REQUIRED CONFIG)' \
  "${REPO_ROOT}/src/xgc_ros1_tools_adapter/CMakeLists.txt"
grep -q 'xgc2::adapter_runtime_client' \
  "${REPO_ROOT}/src/xgc_ros1_tools_adapter/CMakeLists.txt"

test -x "${BOOTSTRAP_SCRIPT}"
# shellcheck disable=SC2016
grep -Fq 'xgc2-protobuf-dev=${XGC2_PROTOBUF_DEB_VERSION}' \
  "${BOOTSTRAP_SCRIPT}"
# shellcheck disable=SC2016
grep -Fq 'libxgc2-adapter-runtime-client-dev=${ADAPTER_RUNTIME_CLIENT_DEB_VERSION}' \
  "${BOOTSTRAP_SCRIPT}"
if rg -n 'git (clone|fetch)|BOOTSTRAP_COMMON_FROM_GIT|apt-get install.*build-essential' \
  "${BOOTSTRAP_SCRIPT}"; then
  echo "common dependencies must come from published XGC2 packages" >&2
  exit 1
fi
grep -Fq 'ADAPTER_RUNTIME_ABI_PACKAGE="libxgc2-adapter-runtime-client2"' \
  "${PACKAGE_SCRIPT}"
grep -Fq 'dpkg-shlibdeps -O' \
  "${PACKAGE_SCRIPT}"
grep -Fq \
  "copy_path \"\${PREFIX_ROOT}/lib/\${ROS_PACKAGE}/\${ROS_PACKAGE}_service_helper\"" \
  "${PACKAGE_SCRIPT}"
grep -Fq "Conflicts: \${REMOVED_PACKAGE}" "${PACKAGE_SCRIPT}"
grep -Fq "Replaces: \${REMOVED_PACKAGE}" "${PACKAGE_SCRIPT}"
grep -Fq 'libxgc2_adapter_runtime_client[.]so[.]2' \
  "${SCRIPT_DIR}/check_installed_packages.sh"
grep -Fq 'libxgc2_adapter_runtime_protocol[.]so[.]2' \
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
if rg -n '^(env:|[[:space:]]+(ADAPTER_RUNTIME_CLIENT_DEB_VERSION|XGC2_PROTOBUF_DEB_VERSION):)|-e (ADAPTER_RUNTIME_CLIENT_DEB_VERSION|XGC2_PROTOBUF_DEB_VERSION)' \
    "${REPO_ROOT}/.github/workflows/ci.yml"; then
  echo "ordinary push CI must resolve current published dependency candidates" >&2
  exit 1
fi
grep -Fq 'ADAPTER_RUNTIME_CLIENT_DEB_VERSION:' \
  "${REPO_ROOT}/.github/workflows/release.yml"
grep -Fq 'XGC2_PROTOBUF_DEB_VERSION:' \
  "${REPO_ROOT}/.github/workflows/release.yml"
# shellcheck disable=SC2016
grep -Fq 'XGC2_DEPENDENCY_SET_DIGEST="${XGC2_DEPENDENCY_SET_DIGEST:-}"' \
  "${REPO_ROOT}/.github/workflows/release.yml"
if rg -n 'inputs[.]run_(cpp_quality|source_tests)' \
    "${REPO_ROOT}/.github/workflows/release.yml"; then
  echo "release workflow must not reference removed optional quality inputs" >&2
  exit 1
fi

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
PYTHONPYCACHEPREFIX="${temporary}/pycache" python3 -m py_compile \
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
if root.findtext("version") != "0.2.0":
    raise SystemExit("ROS package version mismatch")
dependencies = {node.text for node in root if node.tag.endswith("depend")}
if "libxgc2-adapter-runtime-client-dev" not in dependencies:
    raise SystemExit("Adapter Runtime SDK dependency is missing")
if "libxgc2-adapter-runtime-client2" not in dependencies:
    raise SystemExit("Adapter Runtime ABI dependency is missing")
specialized_package = "mav" + "ros"
if any(specialized_package in dependency for dependency in dependencies if dependency):
    raise SystemExit("ROS Tools must not depend on a robot-specialized package")
PY
for schema in "${REPO_ROOT}"/schemas/*.schema.json; do
  python3 -m json.tool "${schema}" >/dev/null
done

python3 "${REPO_ROOT}/tools/generate_runtime_manifests.py" \
  --executable /bin/true \
  --ros-package xgc_ros1_tools_adapter \
  --ros-executable xgc_ros1_tools_adapter_node \
  --schema-dir "${REPO_ROOT}/schemas" \
  --version 0.2.0 \
  --adapter-output "${temporary}/adapter.json" \
  --process-output "${temporary}/process.json"
python3 "${REPO_ROOT}/tools/verify_runtime_manifests.py" \
  --executable /bin/true \
  --ros-package xgc_ros1_tools_adapter \
  --ros-executable xgc_ros1_tools_adapter_node \
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
  --ros-package xgc_ros1_tools_adapter \
  --ros-executable xgc_ros1_tools_adapter_node \
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
