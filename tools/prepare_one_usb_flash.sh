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

IDF_PY="${IDF_PATH}/tools/idf.py"
if [[ ! -f "${IDF_PY}" ]]; then
    echo "idf.py not found: ${IDF_PY}" >&2
    exit 1
fi

echo "Building ESP32-H2 ot_rcp from: ${RCP_PROJECT_DIR}"
(
    cd "${RCP_PROJECT_DIR}"
    python3 "${IDF_PY}" set-target esp32h2
    python3 "${IDF_PY}" build
)

echo "Prepared H2 RCP build directory:"
echo "  ${RCP_BUILD_DIR}"
echo
echo "The managed esp_rcp_update component will pack this RCP build into the S3 rcp_fw partition during idf.py build."
echo
echo "Next:"
echo "  cd ${PROJECT_DIR}"
echo "  idf.py set-target esp32s3"
echo "  idf.py build"
echo "  idf.py -p <S3_USB_PORT> erase-flash flash monitor"
