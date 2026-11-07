#!/bin/bash
# Apply CloverSdDxe into edk2-msm sdm660, so the platform grows a BlockIo over
# the SD slot at sdhci@c084000.  DiskIoDxe / PartitionDxe / Fat are already in
# sdm660.fdf, so this is the only missing link in the storage chain.
set -euo pipefail

EDK2="$1"   # path to the edk2-msm checkout
SRC="$2"    # dir holding CloverSdDxe.c and CloverSdDxe.inf

DEST="$EDK2/Platform/Qualcomm/sdm660/Drivers/CloverSdDxe"
mkdir -p "$DEST"
cp "$SRC/CloverSdDxe.c"   "$DEST/"
cp "$SRC/CloverSdDxe.inf" "$DEST/"

# --- 1. declare it in sdm660.dsc [Components.common]
DSC="$EDK2/Platform/Qualcomm/sdm660/sdm660.dsc"
python3 - "$DSC" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
if 'CloverSdDxe' not in s:
    s = s.replace('[Components.common]\n',
                  '[Components.common]\n  Platform/Qualcomm/sdm660/Drivers/CloverSdDxe/CloverSdDxe.inf\n')
    open(p, 'w').write(s)
    print('dsc: added CloverSdDxe component')
else:
    print('dsc: already present')
PY

# --- 2. add the FILE block to dxe.fdf.inc.
# Placed before the last driver rather than appended, so the whole storage
# chain sits together; order inside one FV does not decide dispatch order
# (Depex does), but a readable fdf is one less thing to doubt later.
FDF="$EDK2/Platform/Qualcomm/sdm660/dxe.fdf.inc"
python3 - "$FDF" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
if 'CloverSdDxe' not in s:
    block = '''
  FILE DRIVER = 7C3A1E52-9B44-4D0E-8E31-5A2C6F1D9B47 {
    SECTION DXE_DEPEX = Platform/Qualcomm/sdm660/Drivers/CloverSdDxe/CloverSdDxe.depex
    SECTION PE32 = Platform/Qualcomm/sdm660/Drivers/CloverSdDxe/CloverSdDxe.efi
    SECTION UI = "CloverSdDxe"
  }
'''
    s = s.rstrip('\n') + '\n' + block
    open(p, 'w').write(s)
    print('fdf: added CloverSdDxe INF block')
else:
    print('fdf: already present')
PY

# --- 3. keep the device tree drop that build.sh expects
DTB="$EDK2/Platform/Xiaomi/sdm660/FdtBlob_compat/clover.dtb"
if [ -f "$SRC/clover.dtb" ]; then
  mkdir -p "$(dirname "$DTB")"
  cp "$SRC/clover.dtb" "$DTB"
fi

echo "patch-sd done"
