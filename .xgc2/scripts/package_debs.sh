#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

INSTALL_ROOT=""
OUTPUT_DIR=""
ROS_DISTRO="${ROS_DISTRO:-noetic}"
PACKAGE="ros-${ROS_DISTRO}-xgc2-ros1-automation-gateway"
ROS_PACKAGE="xgc_ros1_automation_gateway"

product_version() {
  awk -F': *' '/^version:[[:space:]]*/ {print $2; exit}' \
    "${REPO_ROOT}/.xgc2/product.yml"
}

VERSION="${PACKAGE_VERSION:-$(product_version)}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install-root)
      INSTALL_ROOT="$2"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "${INSTALL_ROOT}" || -z "${OUTPUT_DIR}" ]]; then
  echo "--install-root and --output-dir are required" >&2
  exit 2
fi
if [[ -z "${VERSION}" ]]; then
  echo "package version is missing" >&2
  exit 1
fi

ARCH="$(dpkg --print-architecture)"
PREFIX="/opt/ros/${ROS_DISTRO}"
PREFIX_ROOT="${INSTALL_ROOT}${PREFIX}"
BUILD_DIR="$(mktemp -d)"

cleanup() {
  rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

mkdir -p "${OUTPUT_DIR}"
rm -f "${OUTPUT_DIR}/${PACKAGE}_"*.deb

pkg_root="${BUILD_DIR}/${PACKAGE}"
mkdir -p "${pkg_root}"

copy_path() {
  local src="$1"
  if [[ ! -e "${src}" ]]; then
    return
  fi
  mkdir -p "${pkg_root}$(dirname "${src#${INSTALL_ROOT}}")"
  cp -a "${src}" "${pkg_root}${src#${INSTALL_ROOT}}"
}

copy_path "${PREFIX_ROOT}/share/${ROS_PACKAGE}"
copy_path "${PREFIX_ROOT}/lib/${ROS_PACKAGE}/${ROS_PACKAGE}_node"

executable="${pkg_root}${PREFIX}/lib/${ROS_PACKAGE}/${ROS_PACKAGE}_node"
share="${pkg_root}${PREFIX}/share/${ROS_PACKAGE}"

test -x "${executable}" || {
  echo "missing installed gateway executable: ${executable}" >&2
  exit 1
}
test -f "${share}/package.xml" || {
  echo "missing installed ROS package metadata: ${share}/package.xml" >&2
  exit 1
}

mkdir -p "${pkg_root}/DEBIAN" "${pkg_root}/usr/share/doc/${PACKAGE}"
cat > "${pkg_root}/DEBIAN/control" <<EOF
Package: ${PACKAGE}
Version: ${VERSION}
Section: misc
Priority: optional
Architecture: ${ARCH}
Maintainer: XGC2 <lxk36@users.noreply.github.com>
Depends: libjsoncpp1, ros-noetic-mavros-msgs, ros-noetic-ros-babel-fish, ros-noetic-roscpp, ros-noetic-roslib, ros-noetic-std-msgs, ros-noetic-std-srvs
Description: XGC2 local ROS1 automation gateway
 Dynamically publishes installed ROS1 message types and invokes installed ROS1
 services through a bounded, length-framed local Unix socket protocol.
EOF

cp "${REPO_ROOT}/README.md" \
  "${pkg_root}/usr/share/doc/${PACKAGE}/README.md"
cp "${REPO_ROOT}/LICENSE" \
  "${pkg_root}/usr/share/doc/${PACKAGE}/copyright"

find "${pkg_root}" -type d -exec chmod 0755 {} +
find "${pkg_root}" -type f -exec chmod 0644 {} +
chmod 0755 "${pkg_root}/DEBIAN"
chmod 0755 "${executable}"

fakeroot dpkg-deb --build "${pkg_root}" \
  "${OUTPUT_DIR}/${PACKAGE}_${VERSION}_${ARCH}.deb" >/dev/null

find "${OUTPUT_DIR}" -maxdepth 1 -type f \
  -name "${PACKAGE}_*.deb" -print | sort
