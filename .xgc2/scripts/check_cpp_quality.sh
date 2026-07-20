#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/.work/cpp-quality}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --work-dir)
      WORK_DIR="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

for tool in catkin_make clang-format clang-tidy python3 rsync; do
  command -v "${tool}" >/dev/null || {
    echo "missing required tool: ${tool}" >&2
    exit 1
  }
done

rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"
rsync -a --delete \
  --exclude .git \
  --exclude .ci \
  --exclude .work \
  --exclude build \
  --exclude debs \
  --exclude devel \
  --exclude install \
  --exclude install-root \
  "${REPO_ROOT}/" "${WORK_DIR}/"

"${WORK_DIR}/.xgc2/scripts/check_package_compliance.sh"

for schema in "${WORK_DIR}"/schemas/*.schema.json; do
  python3 -m json.tool "${schema}" >/dev/null
done

mapfile -t format_files < <(
  find "${WORK_DIR}/src/xgc_ros1_tools_adapter" -type f \
    \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) -print | sort
)
if [[ "${#format_files[@]}" -eq 0 ]]; then
  echo "no C++ files found" >&2
  exit 1
fi
clang-format --dry-run --Werror --style=Google "${format_files[@]}"

# shellcheck disable=SC1091
source /opt/ros/noetic/setup.bash
cd "${WORK_DIR}"
catkin_make \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

mapfile -t tidy_files < <(
  find "${WORK_DIR}/src/xgc_ros1_tools_adapter" -type f \
    -name "*.cpp" -print | sort
)
for file in "${tidy_files[@]}"; do
  clang-tidy --quiet "${file}" -p "${WORK_DIR}/build"
done

echo "C++ quality check passed"
