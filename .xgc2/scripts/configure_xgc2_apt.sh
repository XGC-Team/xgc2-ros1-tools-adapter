#!/usr/bin/env bash
set -euo pipefail

validate_https_url() {
  local label="$1"
  local value="$2"
  local safe_https_url='^https://[A-Za-z0-9.-]+(:[0-9]{1,5})?(/[A-Za-z0-9._~%+:/=-]*)?$'

  if [[ -z "${value}" || "${value}" =~ [[:space:][:cntrl:]] ||
        ! "${value}" =~ ${safe_https_url} ]]; then
    echo "${label} must be an HTTPS URL without credentials, whitespace, query, or fragment" >&2
    return 1
  fi
}

if [[ "${1:-}" == "--validate-url" ]]; then
  if [[ $# -ne 2 ]]; then
    echo "usage: $0 --validate-url <https-url>" >&2
    exit 2
  fi
  validate_https_url "XGC2 APT URL" "$2"
  exit 0
fi

distribution="${1:-focal}"
case "${distribution}" in
  focal) ;;
  *)
    echo "unsupported XGC2 APT distribution: ${distribution}" >&2
    exit 1
    ;;
esac

production_url="https://xgc2.apt.xiaokang.ink"
overlay_url="${XGC2_APT_OVERLAY_URL:-}"
overlay_url="${overlay_url%/}"
key_url="${XGC2_APT_KEY_URL:-https://xgc2.apt.xiaokang.ink/xgc2-archive-keyring.gpg}"
validate_https_url "XGC2 APT production URL" "${production_url}"
validate_https_url "XGC2 APT key URL" "${key_url}"
if [[ -n "${overlay_url}" ]]; then
  validate_https_url "XGC2 APT overlay URL" "${overlay_url}"
fi

key_file="$(mktemp /tmp/xgc2-archive-keyring.XXXXXX)"
cleanup() {
  rm -f "${key_file}"
}
trap cleanup EXIT

curl -fsSL "${key_url}" -o "${key_file}"
gpg --show-keys --with-fingerprint --with-colons \
  "${key_file}" 2>&1 \
  | grep -q '^fpr:.*:2A8E11B36F56D307ADF626D85E5FDC30979EA43F:$'
install -d -m 0755 /etc/apt/keyrings
install -m 0644 "${key_file}" \
  /etc/apt/keyrings/xgc2-archive-keyring.gpg
echo "deb [signed-by=/etc/apt/keyrings/xgc2-archive-keyring.gpg] ${production_url} ${distribution} main" \
  > /etc/apt/sources.list.d/xgc2.list
if [[ -n "${overlay_url}" ]]; then
  echo "deb [signed-by=/etc/apt/keyrings/xgc2-archive-keyring.gpg] ${overlay_url} ${distribution} main" \
    > /etc/apt/sources.list.d/00-xgc2-release-train.list
fi
apt-get update
