#!/bin/bash
set -e

# Target: Windows 11 64-bit Korean (default)
PRODUCT_EDITION_ID="${1:-3813}"
LANGUAGE="${2:-Korean}"
LOCALE="${3:-ko-kr}"
PROFILE_ID="606624d44113"

USER_AGENT="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"

# 1. Generate session ID
SESSION_ID="$(cat /proc/sys/kernel/random/uuid 2>/dev/null || od -x /dev/urandom | head -1 | awk '{print $2$3$4$5}')"

# 2. Query SKU ID dynamically from modern JSON API
SKU_JSON="$(curl -s --user-agent "$USER_AGENT" \
     --referer "https://www.microsoft.com/software-download/windows11" \
     --max-time 15 --proto =https --tlsv1.2 \
     "https://www.microsoft.com/software-download-connector/api/getskuinformationbyproductedition?profile=${PROFILE_ID}&productEditionId=${PRODUCT_EDITION_ID}&SKU=undefined&friendlyFileName=undefined&Locale=${LOCALE}&sessionID=${SESSION_ID}")"

# Check Sentinel rejection on SKU endpoint
if echo "$SKU_JSON" | grep -q "SentinelReject"; then
    echo "ERROR: Microsoft Sentinel rejected the request (Error 715-123130). IP is flagged as datacenter/VPN." >&2
    exit 3
fi

# Extract SKU ID for target language
SKU_ID="$(echo "$SKU_JSON" | grep -o '{"Id":"[0-9]\+","Language":"'"$LANGUAGE"'"' | grep -o '[0-9]\+' | head -1 || true)"

# Fallback: known static SKU for Korean (Windows 11) is 27140
if [ -z "$SKU_ID" ] && [ "$LANGUAGE" = "Korean" ]; then
    SKU_ID="27140"
fi

if [ -z "$SKU_ID" ]; then
    echo "ERROR: Failed to extract SKU ID for $LANGUAGE from Microsoft API. Server response:" >&2
    echo "$SKU_JSON" | head -n 10 >&2
    exit 1
fi

# 3. Request temporary download link
LINK_JSON="$(curl -s --user-agent "$USER_AGENT" \
     --referer "https://www.microsoft.com/software-download/windows11" \
     --max-time 20 --proto =https --tlsv1.2 \
     "https://www.microsoft.com/software-download-connector/api/GetProductDownloadLinksBySku?profile=${PROFILE_ID}&productEditionId=undefined&SKU=${SKU_ID}&friendlyFileName=undefined&Locale=${LOCALE}&sessionID=${SESSION_ID}")"

# Check Sentinel rejection on Link endpoint
if echo "$LINK_JSON" | grep -q "SentinelReject"; then
    echo "ERROR: Microsoft Sentinel rejected download link request (Error 715-123130). IP is flagged as datacenter/VPN." >&2
    exit 3
fi

# 4. Parse final direct ISO URL
FINAL_URL="$(echo "$LINK_JSON" | grep -o 'https://software.download.prss.microsoft.com[^"'"'"' ]*' | head -1 | sed 's/\\u0026/\&/g')"

if [ -z "$FINAL_URL" ]; then
    echo "ERROR: Failed to parse direct download URL from Microsoft API. Server response:" >&2
    echo "$LINK_JSON" | head -n 15 >&2
    exit 2
fi

echo "$FINAL_URL"
