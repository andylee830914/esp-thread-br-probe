#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RCP_PROJECT_DIR="${RCP_PROJECT_DIR:-${IDF_PATH:-}/examples/openthread/ot_rcp}"
RCP_BUILD_DIR="${RCP_BUILD_DIR:-${RCP_PROJECT_DIR}/build}"

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set. Run: source /path/to/esp-idf/export.sh" >&2
    exit 1
fi

if [[ ! -d "${RCP_PROJECT_DIR}" ]]; then
    echo "RCP project not found: ${RCP_PROJECT_DIR}" >&2
    exit 1
fi

echo "Building ESP32-H2 ot_rcp from: ${RCP_PROJECT_DIR}"
(
    cd "${RCP_PROJECT_DIR}"
    idf.py set-target esp32h2
    idf.py build
)

echo "Generating RCP update image from: ${RCP_BUILD_DIR}"
rm -rf "${PROJECT_DIR}/rcp_fw"
mkdir -p "${PROJECT_DIR}/rcp_fw/ot_rcp_0" "${PROJECT_DIR}/rcp_fw/ot_rcp_1"

python3 "${PROJECT_DIR}/tools/create_rcp_image.py" \
    --rcp-build-dir "${RCP_BUILD_DIR}" \
    --target-file "${PROJECT_DIR}/rcp_fw/ot_rcp_0/rcp_image"

cp "${PROJECT_DIR}/rcp_fw/ot_rcp_0/rcp_image" "${PROJECT_DIR}/rcp_fw/ot_rcp_1/rcp_image"

echo "Prepared:"
echo "  ${PROJECT_DIR}/rcp_fw/ot_rcp_0/rcp_image"
echo "  ${PROJECT_DIR}/rcp_fw/ot_rcp_1/rcp_image"
echo
echo "Next:"
echo "  cd ${PROJECT_DIR}"
echo "  idf.py set-target esp32s3"
echo "  idf.py build"
echo "  idf.py -p <S3_USB_PORT> erase-flash flash monitor"
