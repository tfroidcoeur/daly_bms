#!/usr/bin/env bash
# Download hardware datasheets for offline reference.
# These are several MB each and are gitignored.
set -euo pipefail
cd "$(dirname "$0")"

fetch() {
    local url=$1 out=$2
    if [ -f "$out" ]; then
        echo "have    $out"
    else
        echo "fetch   $out"
        curl -fsSL --retry 3 -o "$out" "$url" || echo "FAILED  $out ($url)"
    fi
}

fetch https://files.waveshare.com/wiki/ESP32-S3-RLCD-4.2/ESP32-S3-RLCD-4.2-schematic.pdf \
      ESP32-S3-RLCD-4.2-schematic.pdf
fetch https://files.waveshare.com/wiki/common/ST_7305_V0_2.pdf \
      ST7305_V0.2.pdf
fetch https://files.waveshare.com/wiki/common/SHTC3_Datasheet.pdf \
      SHTC3.pdf
fetch https://files.waveshare.com/wiki/common/Pcf85063atl1118-NdPQpTGE-loeW7GbZ7.pdf \
      PCF85063A.pdf
fetch https://files.waveshare.com/wiki/common/ES8311.DS.pdf \
      ES8311.pdf
fetch https://documentation.espressif.com/esp32-s3_datasheet_en.pdf \
      ESP32-S3-datasheet.pdf
fetch https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf \
      ESP32-S3-TRM.pdf

echo
echo "The schematic is the one that matters: it has the 2x8 expansion header pinout."
