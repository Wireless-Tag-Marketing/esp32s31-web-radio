#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "${script_dir}/.." && pwd)"
idf_root="${ESP_IDF_ROOT:-/home/biubiu/.espressif/master/esp-idf}"
merged_bin="${project_dir}/build/esp32s31-korvo1-webradio-merged.bin"

if [[ ! -f "${idf_root}/export.sh" ]]; then
    echo "ESP-IDF not found: ${idf_root}" >&2
    exit 1
fi

source "${idf_root}/export.sh" >/dev/null
cd "${project_dir}"

if [[ ! -f sdkconfig ]] || ! grep -q '^CONFIG_IDF_TARGET="esp32s31"$' sdkconfig; then
    idf.py --preview set-target esp32s31
fi

idf.py --preview build
idf.py --preview merge-bin -f raw -o "${merged_bin}"

echo
echo "Merged firmware: ${merged_bin}"
sha256sum "${merged_bin}"
echo "Flash Download Tool: ESP32-S31, address 0x0, DIO, 80 MHz, 16 MB"
