#!/bin/bash
# Build clover UEFI locally (in WSL), with CloverGopDxe + CloverSdDxe patched in.
#
#   bash build-local.sh          # patch + build
#   bash build-local.sh --fast   # skip re-patching (sources unchanged)
#
# Output lands in ~/edk2-msm/out/ and is copied next to this script as
# boot-clover.img when build.sh produces one.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
E="$HOME/edk2-msm"
FAST=0
for a in "$@"; do [ "$a" = "--fast" ] && FAST=1; done

[ -d "$E" ] || { echo "FATAL: $E missing (clone edk2-msm first)"; exit 1; }

cd "$E"

if [ "$FAST" -eq 0 ]; then
  bash "$SRC/patch-gop.sh" "$E" "$SRC"
  bash "$SRC/patch-sd.sh"  "$E" "$SRC"
  grep -n "CloverSdDxe" Platform/Qualcomm/sdm660/sdm660.dsc
  grep -n "CloverSdDxe" Platform/Qualcomm/sdm660/dxe.fdf.inc
else
  # still refresh the C sources, a rebuild should not need a re-patch
  cp "$SRC/CloverSdDxe.c"   Platform/Qualcomm/sdm660/Drivers/CloverSdDxe/ 2>/dev/null || true
  cp "$SRC/CloverSdDxe.inf" Platform/Qualcomm/sdm660/Drivers/CloverSdDxe/ 2>/dev/null || true
fi

touch ramdisk

echo "=== build ==="
set +e
./build.sh --device clover > "$SRC/build-local.log" 2>&1
RC=$?
set -e
tail -40 "$SRC/build-local.log"

if [ $RC -ne 0 ]; then
  echo "BUILD FAILED (rc=$RC) -- full log: $SRC/build-local.log"
  grep -nE "error:|Error [0-9]|\.c:[0-9]+:[0-9]+" "$SRC/build-local.log" | head -40 || true
  exit $RC
fi

IMG=$(find "$E" -maxdepth 4 -name "boot*.img" | head -1)
if [ -n "$IMG" ]; then
  cp "$IMG" "$SRC/boot-clover.img"
  echo "OK: $IMG -> $SRC/boot-clover.img ($(stat -c%s "$SRC/boot-clover.img") bytes)"
else
  echo "build ok but no boot*.img found"
  find "$E" -maxdepth 4 \( -name "*.fd" -o -name "*.img" \) | head -10
fi
