#!/usr/bin/env bash
set -euo pipefail

firmware_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
esp_dl_dir="${firmware_dir}/managed_components/espressif__esp-dl"
esp_dl_patch="${firmware_dir}/patches/esp-dl-3.3.0-cmake.patch"

cd "${firmware_dir}"
python3 ./fetch_repos.py
idf.py reconfigure

if patch --batch --forward --dry-run --silent -d "${esp_dl_dir}" -p1 < "${esp_dl_patch}"; then
    patch --batch --forward -d "${esp_dl_dir}" -p1 < "${esp_dl_patch}"
elif patch --batch --reverse --dry-run --silent -d "${esp_dl_dir}" -p1 < "${esp_dl_patch}"; then
    echo "ESP-DL optimization patch is already applied."
else
    echo "ESP-DL 3.3.0 does not match the expected source; refusing to build." >&2
    exit 1
fi

idf.py build
