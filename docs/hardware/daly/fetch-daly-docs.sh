#!/usr/bin/env bash
# Download Daly's BMS documents for offline reference.
# Several are tens of MB of scanned pages, so they are gitignored.
# See README.md for what each one covers and where it comes from.
set -euo pipefail
cd "$(dirname "$0")"

fetch() {
    local url=$1 out=$2
    if [ -f "$out" ]; then
        echo "have    $out"
    else
        echo "fetch   $out"
        curl -fsSL --retry 3 -A "Mozilla/5.0" -o "$out" "$url" || echo "FAILED  $out ($url)"
    fi
}

# The manual for the packs' blue Smart Active Balance BMS, from Daly
fetch "https://www.dalyelec.com/download/%E4%B8%BB%E5%8A%A8%E5%9D%87%E8%A1%A1%E4%BF%9D%E6%8A%A4%E6%9D%BF%E8%AF%B4%E6%98%8E%E4%B9%A6%20%E7%94%B5%E5%AD%90%E7%89%88%20%EF%BC%88%E8%8B%B1%E6%96%87%EF%BC%89.pdf" \
      Daly-Smart-Active-Balance-BMS-User-Manual.pdf

# Daly's protocol documents; Daly does not host these, these are reseller mirrors
fetch https://robu-prod-media.s3.ap-south-1.amazonaws.com/uploads/2022/02/Daly-CAN-Communications-Protocol-V1.0-1.pdf \
      Daly-CAN-Communications-Protocol-V1.0.pdf
fetch https://robu.in/wp-content/uploads/2021/10/Daly-UART_485-Communications-Protocol-V1.21-1.pdf \
      Daly-UART-485-Communications-Protocol-V1.2.pdf
