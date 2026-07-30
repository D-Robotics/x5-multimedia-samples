#!/bin/bash
# Download LVGL 9.5.0 into <project>/third_party/lvgl
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}"
LVGL_DIR="${ROOT_DIR}/third_party/lvgl"
LVGL_VERSION="v9.5.0"

if [ -f "${LVGL_DIR}/lvgl.h" ]; then
	echo "LVGL already present at ${LVGL_DIR}"
	grep -E 'LVGL_VERSION_(MAJOR|MINOR|PATCH)' "${LVGL_DIR}/lv_version.h" 2>/dev/null || true
	exit 0
fi

mkdir -p "${ROOT_DIR}/third_party"
if command -v curl >/dev/null 2>&1; then
	TMP_TGZ="$(mktemp /tmp/lvgl-XXXXXX.tar.gz)"
	curl -L --fail --retry 3 -o "${TMP_TGZ}" \
		"https://github.com/lvgl/lvgl/archive/refs/tags/${LVGL_VERSION}.tar.gz"
	tar -xzf "${TMP_TGZ}" -C "${ROOT_DIR}/third_party"
	rm -f "${TMP_TGZ}"
	# GitHub archive extracts as lvgl-9.5.0
	mv "${ROOT_DIR}/third_party/lvgl-${LVGL_VERSION#v}" "${LVGL_DIR}"
elif command -v git >/dev/null 2>&1; then
	git clone --depth 1 --branch "${LVGL_VERSION}" \
		https://github.com/lvgl/lvgl.git "${LVGL_DIR}"
else
	echo "curl/git not found; download LVGL ${LVGL_VERSION} manually to ${LVGL_DIR}"
	exit 1
fi

echo "LVGL ${LVGL_VERSION} ready at ${LVGL_DIR}"
