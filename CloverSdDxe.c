/** @file
  clover (Xiaomi Mi Pad 4, SDM660): a BlockIo over the SD card slot.

  WHY THIS FILE EXISTS

  edk2-msm's sdm660 platform ships no storage driver at all: the vendor's
  SdccDxe was a binary blob and this tree does not carry it, and nothing in
  Silicon/Qualcomm is a replacement.  What the platform DOES carry is the rest
  of the chain -- DiskIoDxe, PartitionDxe, Fat, UsbMassStorageDxe are all in
  sdm660.fdf -- so the only missing link is something that publishes
  EFI_BLOCK_IO_PROTOCOL over a card.  That is this file.

  The slot is sdhci@c084000 ("mmc1" in the unit's own /proc/iomem,
  0x0c084000-0x0c084fff), and clover.dtb describes it as
  compatible = "qcom,sdhci-msm-v5" with clock-names = "iface_clk","core_clk".
  There is no vmmc-supply and no vqmmc-supply property: the rail is not
  software controlled on this board, so no PMIC work is needed here.  Card
  detect is cd-gpios = <&tlmm 54 GPIO_ACTIVE_HIGH>, which this driver does not
  poll -- an empty slot simply fails CMD8 and the driver exits quietly, which
  is cheaper and cannot be wrong.

  WHAT IS HARD, AND WHY IT IS DONE THIS WAY

  sdhci-msm has no internal clock divider.  The SDHCI clock-control divider
  field is ignored by the hardware; the card clock IS the GCC root clock
  (sdcc2_apps_clk_src in gcc-sdm660.c).  So identification at 400 kHz means
  programming GCC, not SDHCI.  The values used below are not derived from the
  spec table but lifted from a driver that already ran on this exact silicon
  (CloverSdhci, the eMMC port for this board): cfg=0x2017, m=0x01, n=0xfc,
  d=0xfb, with CMD_RCGR at 0x14010 for SDCC2 (0x1602c is SDCC1's).  Those
  writes were photographed coming back correct on the unit, so they are the
  starting point rather than a guess.

  The one offset that had to be re-derived is the source-select field: SDCC1
  and SDCC2 use different parent maps, so P_GPLL0_EARLY_DIV is 6 on SDCC1 but
  2 on SDCC2 (gcc-sdm660.c: gcc_parent_map_xo_gpll0_gpll0_early_div_gpll4).
  Reusing SDCC1's 0x060b for the 50 MHz row here would select the wrong
  parent; 0x020b is the SDCC2 spelling of the same row.

  Data moves over ADMA2.  The controller advertises ADMA2 and 64-bit
  addressing but NOT SDMA, and the PIO buffer port is not trustworthy on this
  family (CORE_FIFO_ALT_EN hands the data path to Qualcomm's own FIFO).  One
  descriptor per transfer, END set, no interrupt.

  NOTHING HERE TOUCHES eMMC.  Different controller (0x0c0c4000), different
  card, and the boot path this tablet uses.  Reading a card in the SD slot
  cannot brick a unit that has no EDL.

  Copyright (c) 2026, clover-windows-uefi contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

//
// ---------------------------------------------------------------- addresses
//
// Both windows are already described by PlatformMemoryMapLib as MMAP_IO /
// UNCACHEABLE, so plain MmioRead/MmioWrite are correct and no GCD work is
// needed here:
//   {"GCC CLK CTL", 0x00100000, 0x000B0000, ...}   covers 0x14000..0x16000
//   {"PERIPH_SS",   0x0C000000, 0x00200000, ...}   covers 0x0C084000
//
#define CLOVER_GCC_BASE             ((UINTN)0x00100000)
#define CLOVER_SDCC2_BASE           ((UINTN)0x0C084000)
#define CLOVER_SDCC2_SIZE           ((UINTN)0x00001000)

//
// GCC sdcc2 clock block (gcc-sdm660.c).  cmd_rcgr = 0x14010, and CFG/M/N/D
// live at cmd_rcgr + 4/8/12/16, exactly as they do for SDCC1 at 0x1602c.
//
#define GCC_SDCC2_APPS_CBCR         0x14004
#define GCC_SDCC2_AHB_CBCR          0x14008
#define GCC_SDCC2_APPS_CMD          0x14010
#define GCC_SDCC2_APPS_CFG          0x14014
#define GCC_SDCC2_APPS_M            0x14018
#define GCC_SDCC2_APPS_N            0x1401C
#define GCC_SDCC2_APPS_D            0x14020

#define GCC_CBCR_ON                 0x00000001u
#define GCC_CBCR_OFF_BIT            0x80000000u
#define GCC_CMD_UPDATE              BIT0

//
// 400 kHz: XO(19.2 MHz) / 12, MND 1/4.  cfg[4:0] holds 2*h-1 with h=12, so
// 0x17; cfg[13:12]=2 selects the MND path; src=0 is XO.
//
#define GCC_CFG_400K                0x00002017u
#define GCC_M_400K                  0x00000001u
#define GCC_N_400K                  0x000000FCu
#define GCC_D_400K                  0x000000FBu

//
// 25 MHz: GPLL0_EARLY_DIV / 6, MND 1/2.  src=2 on SDCC2 (NOT 6 -- that is
// SDCC1's mapping).
//
#define GCC_CFG_25M                 0x0000220Bu
#define GCC_M_25M                   0x00000001u
#define GCC_N_25M                   0x000000FEu
#define GCC_D_25M                   0x000000FAu

//
// 50 MHz: GPLL0_EARLY_DIV / 6, no MND.  src=2 -> cfg[10:8]=0x2.
//
// This row is preferred over the 25 MHz one on purpose: it is a plain divider
// with m=n=0, while the 25 MHz row needs an MND triple whose exact encoding
// (the ~(n-m) / ~(d-1) complement form) cannot be checked against this silicon
// without a photo.  A divider that is off by one gives a slightly wrong clock;
// an MND that is off gives a wrong clock by a factor.  The card is asked to
// confirm either choice with CMD13 before the driver keeps it.
//
#define GCC_CFG_50M                 0x0000020Bu

//
// Vendor registers.  Offsets measured on this board by the eMMC port:
// VENDOR_SPEC (0x20c) reads back 2 << 8 (free-running MCLK select) and
// VENDOR_SPEC3 (0x250) reads 0x02626440.
//
#define SDHCI_VENDOR_SPEC           0x20C
#define SDHCI_VENDOR_SPEC3          0x250
#define VENDOR_SPEC_MCLK_SEL_MASK   (7u << 8)
#define VENDOR_SPEC_MCLK_SEL_DFLT   (2u << 8)
#define VENDOR_SPEC3_FIFO_ALT_EN    BIT10

//
// --------------------------------------------------------- SDHCI registers
//
#define SDHCI_DMA_ADDRESS           0x00
#define SDHCI_BLOCK_SIZE            0x04
#define SDHCI_BLOCK_COUNT           0x06
#define SDHCI_ARGUMENT              0x08
#define SDHCI_TRANSFER_MODE         0x0C
#define SDHCI_COMMAND               0x0E
#define SDHCI_RESPONSE              0x10
#define SDHCI_BUFFER                0x20
#define SDHCI_PRESENT_STATE         0x24
#define SDHCI_HOST_CONTROL          0x28
#define SDHCI_POWER_CONTROL         0x29
#define SDHCI_CLOCK_CONTROL         0x2C
#define SDHCI_TIMEOUT_CONTROL       0x2E
#define SDHCI_SOFTWARE_RESET        0x2F
#define SDHCI_NORMAL_INT_STATUS     0x30
#define SDHCI_ERROR_INT_STATUS      0x32
#define SDHCI_NORMAL_INT_STS_EN     0x34
#define SDHCI_ERROR_INT_STS_EN      0x36
#define SDHCI_NORMAL_INT_SIG_EN     0x38
#define SDHCI_ERROR_INT_SIG_EN      0x3A
#define SDHCI_AUTO_CMD12_ERR_STS    0x3C
#define SDHCI_HOST_CONTROL2         0x3E
#define SDHCI_CAPABILITIES          0x40
#define SDHCI_MAX_CURRENT           0x48
#define SDHCI_ADMA_ERROR            0x54
#define SDHCI_ADMA_ADDRESS          0x58
#define SDHCI_ADMA_ADDRESS_HI       0x5C
#define SDHCI_SLOT_INT_STATUS       0xFC
#define SDHCI_HOST_VERSION          0xFE

#define SDHCI_RESET_ALL             0x01
#define SDHCI_RESET_CMD             0x02
#define SDHCI_RESET_DAT             0x04

#define SDHCI_CLOCK_INT_EN          BIT0
#define SDHCI_CLOCK_INT_STABLE      BIT1
#define SDHCI_CLOCK_CARD_EN         BIT2

#define SDHCI_POWER_330             0x0E
#define SDHCI_POWER_ON              0x01

#define SDHCI_CMD_RESP_NO           0x00
#define SDHCI_CMD_RESP_LONG         0x01
#define SDHCI_CMD_RESP_SHORT        0x02
#define SDHCI_CMD_RESP_SHORT_BUSY   0x03

#define SDHCI_CMD_CRC               BIT3
#define SDHCI_CMD_INDEX             BIT4
#define SDHCI_CMD_DATA              BIT5

#define SDHCI_TRNS_DMA              BIT0
#define SDHCI_TRNS_BLK_CNT_EN       BIT1
#define SDHCI_TRNS_AUTO_CMD12       BIT2
#define SDHCI_TRNS_READ             BIT4
#define SDHCI_TRNS_MULTI            BIT5

#define SDHCI_INT_CMD_COMPLETE      BIT0
#define SDHCI_INT_TRANS_COMPLETE    BIT1
#define SDHCI_INT_DMA               BIT3
#define SDHCI_INT_BUF_WR_READY      BIT4
#define SDHCI_INT_BUF_RD_READY      BIT5
#define SDHCI_INT_ERROR             BIT15

#define SDHCI_ERR_CMD_TIMEOUT       BIT0
#define SDHCI_ERR_CMD_CRC           BIT1
#define SDHCI_ERR_CMD_END_BIT       BIT2
#define SDHCI_ERR_CMD_INDEX         BIT3
#define SDHCI_ERR_DATA_TIMEOUT      BIT4
#define SDHCI_ERR_DATA_CRC          BIT5
#define SDHCI_ERR_DATA_END_BIT      BIT6
#define SDHCI_ERR_CURRENT_LIMIT     BIT7
#define SDHCI_ERR_ACMD12            BIT8
#define SDHCI_ERR_ADMA              BIT9

#define SDHCI_STATE_CMD_INHIBIT     BIT0
#define SDHCI_STATE_DAT_INHIBIT     BIT1
#define SDHCI_STATE_CARD_INSERTED   BIT16
#define SDHCI_STATE_WRITE_PROTECT   BIT19
#define SDHCI_STATE_DAT_LINE_ACTIVE BIT2

#define SDHCI_HOST_CTRL_4BIT        BIT1
#define SDHCI_HOST_CTRL_HISPEED     BIT2
#define SDHCI_HOST_CTRL_DMA_SEL_ADMA2_64  ((UINT8)(2u << 3))

#define SDHCI_CAP_HISPEED           BIT13
#define SDHCI_CAP_ADMA2             BIT22
#define SDHCI_CAP_64BIT             BIT27
#define SDHCI_CAP_V18               BIT26
#define SDHCI_CAP_BASE_MHZ_SHIFT    8
#define SDHCI_CAP_BASE_MHZ_MASK     (0xFFu << 8)

//
// ADMA2 descriptor attributes
//
#define ADMA2_ATTR_VALID            BIT0
#define ADMA2_ATTR_END              BIT1
#define ADMA2_ATTR_INT              BIT2
#define ADMA2_ATTR_ACT_TRAN         (2u << 4)

//
// SD commands used here
//
#define SD_CMD_GO_IDLE              0
#define SD_CMD_ALL_SEND_CID         2
#define SD_CMD_SEND_REL_ADDR        3
#define SD_CMD_SWITCH_FUNC          6
#define SD_CMD_SELECT_CARD          7
#define SD_CMD_SEND_IF_COND         8
#define SD_CMD_SEND_CSD             9
#define SD_CMD_SEND_STATUS          13
#define SD_CMD_SET_BLOCKLEN         16
#define SD_CMD_READ_SINGLE          17
#define SD_CMD_READ_MULTIPLE        18
#define SD_CMD_WRITE_SINGLE         24
#define SD_CMD_WRITE_MULTIPLE       25
#define SD_CMD_APP_CMD              55

#define SD_ACMD_SET_BUS_WIDTH       6
#define SD_ACMD_SD_STATUS           13
#define SD_ACMD_SEND_OP_COND        41
#define SD_ACMD_SET_CLR_CARD_DETECT 42
#define SD_ACMD_SEND_SCR            51

#define SD_OCR_HCS                  BIT30
#define SD_OCR_VOLTAGE_3V2_3V3      0x00300000u
#define SD_OCR_VOLTAGE_3V3_3V4      0x00180000u

#define SD_R1_READY_FOR_DATA        BIT8
#define SD_R1_CURRENT_STATE_MASK    0x1E00u
#define SD_R1_STATE_TRAN            4

#define SD_BLOCK_SIZE               512

//
// ------------------------------------------------------------ driver state
//
typedef struct {
  UINT16  Attribute;
  UINT16  Length;
  UINT32  AddressLow;
  UINT32  AddressHigh;
} ADMA2_DESC64;

typedef struct {
  UINT32  CardInterfaceMode;
  UINT16  Rca;
  UINT16  BlockSize;
  UINT32  BaseClockMhz;
  UINT64  Capacity;         // bytes
  UINT32  LastBlock;
  UINT16  ClockKhz;         // what the root clock is currently set to
  BOOLEAN HighCapacity;
  BOOLEAN BusWidth4;
  BOOLEAN HighSpeed;
} CLOVER_SD_DEV;

typedef struct {
  SD_DEVICE_PATH             Sd;
  EFI_DEVICE_PATH_PROTOCOL   End;
} CLOVER_SD_DEVICE_PATH;

STATIC CLOVER_SD_DEV         mSd;
STATIC EFI_HANDLE            mSdHandle = NULL;
STATIC EFI_BLOCK_IO_MEDIA    mSdMedia;
STATIC EFI_BLOCK_IO_PROTOCOL mSdBlockIo;
STATIC CLOVER_SD_DEVICE_PATH mSdDevicePath;
STATIC ADMA2_DESC64          *mAdmaTable = NULL;
STATIC VOID                  *mAdmaBuffer = NULL;
STATIC UINTN                 mAdmaBufferPages = 0;

//
// ------------------------------------------------------------- MMIO access
//
STATIC UINT32
SdRead32 (
  IN UINTN  Offset
  )
{
  return MmioRead32 (CLOVER_SDCC2_BASE + Offset);
}

STATIC VOID
SdWrite32 (
  IN UINTN   Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 (CLOVER_SDCC2_BASE + Offset, Value);
}

STATIC UINT16
SdRead16 (
  IN UINTN  Offset
  )
{
  return MmioRead16 (CLOVER_SDCC2_BASE + Offset);
}

STATIC VOID
SdWrite16 (
  IN UINTN   Offset,
  IN UINT16  Value
  )
{
  MmioWrite16 (CLOVER_SDCC2_BASE + Offset, Value);
}

STATIC UINT8
SdRead8 (
  IN UINTN  Offset
  )
{
  return MmioRead8 (CLOVER_SDCC2_BASE + Offset);
}

STATIC VOID
SdWrite8 (
  IN UINTN  Offset,
  IN UINT8  Value
  )
{
  MmioWrite8 (CLOVER_SDCC2_BASE + Offset, Value);
}

STATIC UINT32
GccRead32 (
  IN UINTN  Offset
  )
{
  return MmioRead32 (CLOVER_GCC_BASE + Offset);
}

STATIC VOID
GccWrite32 (
  IN UINTN   Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 (CLOVER_GCC_BASE + Offset, Value);
}

//
// --------------------------------------------------------- GCC clock work
//
STATIC
EFI_STATUS
GccEnableBranch (
  IN UINTN  Offset
  )
{
  UINT32  Value;
  UINTN   Spin;

  Value = GccRead32 (Offset);
  if (((Value & GCC_CBCR_ON) != 0) && ((Value & GCC_CBCR_OFF_BIT) == 0)) {
    return EFI_SUCCESS;
  }

  GccWrite32 (Offset, Value | GCC_CBCR_ON);
  for (Spin = 0; Spin < 100000; Spin++) {
    Value = GccRead32 (Offset);
    if (((Value & GCC_CBCR_ON) != 0) && ((Value & GCC_CBCR_OFF_BIT) == 0)) {
      return EFI_SUCCESS;
    }
    gBS->Stall (1);
  }

  DEBUG ((DEBUG_ERROR, "CloverSd: GCC branch 0x%x never came on (read 0x%08x)\n", Offset, Value));
  return EFI_DEVICE_ERROR;
}

STATIC
VOID
GccSetRate (
  IN UINT32  Cfg,
  IN UINT32  M,
  IN UINT32  N,
  IN UINT32  D
  )
{
  UINTN  Spin;

  GccWrite32 (GCC_SDCC2_APPS_CFG, Cfg);
  GccWrite32 (GCC_SDCC2_APPS_M, M);
  GccWrite32 (GCC_SDCC2_APPS_N, N);
  GccWrite32 (GCC_SDCC2_APPS_D, D);

  GccWrite32 (GCC_SDCC2_APPS_CMD, GccRead32 (GCC_SDCC2_APPS_CMD) | GCC_CMD_UPDATE);
  for (Spin = 0; Spin < 100000; Spin++) {
    if ((GccRead32 (GCC_SDCC2_APPS_CMD) & GCC_CMD_UPDATE) == 0) {
      break;
    }
    gBS->Stall (1);
  }
}

//
// ------------------------------------------------------- controller bringup
//
STATIC
EFI_STATUS
SdHostReset (
  IN UINT8  Mask
  )
{
  UINTN   Spin;
  UINT8   Value;

  SdWrite8 (SDHCI_SOFTWARE_RESET, Mask);
  for (Spin = 0; Spin < 20000; Spin++) {
    Value = SdRead8 (SDHCI_SOFTWARE_RESET);
    if ((Value & Mask) == 0) {
      return EFI_SUCCESS;
    }
    gBS->Stall (10);
  }

  DEBUG ((DEBUG_ERROR, "CloverSd: software reset 0x%x did not self-clear (read 0x%02x)\n", Mask, Value));
  return EFI_TIMEOUT;
}

//
// The host has no usable internal divider, so the divider field stays 0 and
// only the internal/card clock are enabled here.  What actually sets the
// frequency is GccSetRate() above.
//
STATIC
EFI_STATUS
SdHostSetClock (
  IN BOOLEAN  Enable
  )
{
  UINTN   Spin;
  UINT16  Value;

  Value = SdRead16 (SDHCI_CLOCK_CONTROL);

  if (!Enable) {
    SdWrite16 (SDHCI_CLOCK_CONTROL, (UINT16)(Value & ~(SDHCI_CLOCK_CARD_EN | SDHCI_CLOCK_INT_EN)));
    return EFI_SUCCESS;
  }

  Value = (UINT16)((Value & 0xFF00u) | SDHCI_CLOCK_INT_EN);
  SdWrite16 (SDHCI_CLOCK_CONTROL, Value);

  for (Spin = 0; Spin < 20000; Spin++) {
    if ((SdRead16 (SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE) != 0) {
      SdWrite16 (SDHCI_CLOCK_CONTROL, (UINT16)(SdRead16 (SDHCI_CLOCK_CONTROL) | SDHCI_CLOCK_CARD_EN));
      return EFI_SUCCESS;
    }
    gBS->Stall (10);
  }

  DEBUG ((DEBUG_ERROR, "CloverSd: internal clock never stabilised\n"));
  return EFI_TIMEOUT;
}

STATIC
VOID
SdHostLedOff (
  VOID
  )
{
  UINT8  Value;

  //
  // Host control 1 bit 0 is LED_Control.  Leaving it set keeps the slot LED
  // (if any) lit; not important, but the vendor driver clears it and a clean
  // initial value makes the first register dump readable.
  //
  Value = SdRead8 (SDHCI_HOST_CONTROL);
  if ((Value & 0x01) != 0) {
    SdWrite8 (SDHCI_HOST_CONTROL, (UINT8)(Value & ~0x01));
  }
}

//
// Clear every status bit.  All of them are write-1-to-clear, so 0xFFFFFFFF on
// a 32-bit window is the documented way to do it in one go (the upper half of
// the normal/error pair is reserved).
//
STATIC
VOID
SdClearStatus (
  VOID
  )
{
  SdWrite16 (SDHCI_NORMAL_INT_STATUS, 0xFFFF);
  SdWrite16 (SDHCI_ERROR_INT_STATUS, 0xFFFF);
  SdWrite16 (SDHCI_AUTO_CMD12_ERR_STS, 0xFFFF);
}

//
// ------------------------------------------------------------- command send
//
typedef struct {
  BOOLEAN  Sent;
  UINT32   Status;
  UINT32   Response[4];
  UINT32   Spins;
} SD_CMD_RESULT;

STATIC
EFI_STATUS
SdSendCommand (
  IN  UINT8         Index,
  IN  UINT32        Argument,
  IN  UINT16        RespType,
  IN  UINT16        Flags,
  OUT SD_CMD_RESULT *Result
  )
{
  UINTN   Spin;
  UINT16  Command;
  UINT32  IntStatus;
  UINT32  ErrStatus;

  ZeroMem (Result, sizeof (*Result));

  //
  // Wait for the command line to be free.  A controller left mid-command by
  // firmware is the one failure that looks like "no card", so it is reported
  // separately rather than folded into a timeout later.
  //
  for (Spin = 0; Spin < 100000; Spin++) {
    if ((SdRead32 (SDHCI_PRESENT_STATE) & SDHCI_STATE_CMD_INHIBIT) == 0) {
      break;
    }
    gBS->Stall (1);
  }

  if ((SdRead32 (SDHCI_PRESENT_STATE) & SDHCI_STATE_CMD_INHIBIT) != 0) {
    DEBUG ((DEBUG_ERROR, "CloverSd: CMD%d: command line inhibited\n", Index));
    return EFI_NOT_READY;
  }

  SdClearStatus ();
  SdWrite32 (SDHCI_ARGUMENT, Argument);

  Command = (UINT16)((Index << 8) | RespType | Flags);
  SdWrite16 (SDHCI_COMMAND, Command);

  for (Spin = 0; Spin < 200000; Spin++) {
    IntStatus = SdRead32 (SDHCI_NORMAL_INT_STATUS);
    if ((IntStatus & SDHCI_INT_ERROR) != 0) {
      ErrStatus = SdRead32 (SDHCI_ERROR_INT_STATUS);
      SdClearStatus ();
      Result->Status = IntStatus;
      Result->Spins  = (UINT32)Spin;
      DEBUG ((
        DEBUG_ERROR,
        "CloverSd: CMD%d error int=0x%08x err=0x%08x\n",
        Index, IntStatus, ErrStatus
        ));
      return EFI_DEVICE_ERROR;
    }

    if ((IntStatus & SDHCI_INT_CMD_COMPLETE) != 0) {
      break;
    }
    gBS->Stall (1);
  }

  Result->Spins  = (UINT32)Spin;
  Result->Status = SdRead32 (SDHCI_NORMAL_INT_STATUS);

  if ((Result->Status & SDHCI_INT_CMD_COMPLETE) == 0) {
    SdClearStatus ();
    DEBUG ((DEBUG_ERROR, "CloverSd: CMD%d never completed (state 0x%08x)\n", Index, SdRead32 (SDHCI_PRESENT_STATE)));
    return EFI_TIMEOUT;
  }

  Result->Sent = TRUE;
  if (RespType != SDHCI_CMD_RESP_NO) {
    Result->Response[0] = SdRead32 (SDHCI_RESPONSE + 0x00);
    Result->Response[1] = SdRead32 (SDHCI_RESPONSE + 0x04);
    Result->Response[2] = SdRead32 (SDHCI_RESPONSE + 0x08);
    Result->Response[3] = SdRead32 (SDHCI_RESPONSE + 0x0C);
  }

  SdWrite16 (SDHCI_NORMAL_INT_STATUS, SDHCI_INT_CMD_COMPLETE);
  return EFI_SUCCESS;
}

//
// --------------------------------------------------------------- ADMA2 I/O
//
STATIC
VOID
AdmaBuild (
  IN  UINT64  Address,
  IN  UINT32  Length,
  IN  BOOLEAN Last
  )
{
  mAdmaTable[0].Attribute   = (UINT16)(ADMA2_ATTR_VALID | ADMA2_ATTR_ACT_TRAN | (Last ? ADMA2_ATTR_END : 0));
  //
  // The length field is 16 bits where 0 encodes 65536, so a transfer that is
  // an exact multiple of 64 KiB spells itself and anything smaller is copied
  // verbatim.
  //
  mAdmaTable[0].Length      = (UINT16)(Length & 0xFFFFu);
  mAdmaTable[0].AddressLow  = (UINT32)(Address & 0xFFFFFFFFULL);
  mAdmaTable[0].AddressHigh = (UINT32)(Address >> 32);
}

STATIC
EFI_STATUS
SdTransfer (
  IN  UINT32        Address,
  IN  UINT16        BlockCount,
  IN  VOID          *Buffer,
  IN  BOOLEAN       Read
  )
{
  EFI_STATUS     Status;
  SD_CMD_RESULT  CmdResult;
  UINTN          Spin;
  UINT32         IntStatus;
  UINT32         ErrStatus;
  UINT32         Length;
  UINT16         TransferMode;
  UINT8          HostControl;

  Length = (UINT32)BlockCount * SD_BLOCK_SIZE;

  //
  // Cache coherency.  The ADMA2 engine is a DMA master that reads and writes
  // physical memory directly, and it has no view of the CPU cache.  The DDR
  // heap here is write-back (WRITE_BACK_XN in PlatformMemoryMapLib), so the
  // bounce buffer must be flushed/invalidated around every transfer or the CPU
  // and the card end up looking at two different copies:
  //
  //   read:  invalidate first so the copy-out below sees what the DMA wrote,
  //          not a stale cache line.
  //   write: copy in, then write-back so the DMA reads what the CPU wrote.
  //
  if (Read) {
    InvalidateDataCacheRange (mAdmaBuffer, Length);
  } else {
    CopyMem (mAdmaBuffer, Buffer, Length);
    WriteBackDataCacheRange (mAdmaBuffer, Length);
  }

  AdmaBuild ((UINT64)(UINTN)mAdmaBuffer, Length, TRUE);

  //
  // The descriptor table itself is also read by the DMA engine, so its cache
  // lines must be written back before the transfer starts.
  //
  WriteBackDataCacheRange (mAdmaTable, sizeof (ADMA2_DESC64));

  SdWrite32 (SDHCI_ADMA_ADDRESS, (UINT32)((UINTN)mAdmaTable & 0xFFFFFFFFu));
  if ((SdRead32 (SDHCI_CAPABILITIES) & SDHCI_CAP_64BIT) != 0) {
    SdWrite32 (SDHCI_ADMA_ADDRESS_HI, (UINT32)((UINT64)(UINTN)mAdmaTable >> 32));
  }

  //
  // Host control 1 bits [4:3] pick the DMA engine.  Leaving whatever the
  // previous owner selected here is how an ADMA2 descriptor table gets read by
  // an SDMA state machine, so it is set every transfer rather than once.
  //
  HostControl  = SdRead8 (SDHCI_HOST_CONTROL);
  HostControl &= (UINT8)~0x18u;
  HostControl |= (UINT8)(((SdRead32 (SDHCI_CAPABILITIES) & SDHCI_CAP_64BIT) != 0) ? (3u << 3) : (2u << 3));
  SdWrite8 (SDHCI_HOST_CONTROL, HostControl);

  SdClearStatus ();

  SdWrite16 (SDHCI_BLOCK_SIZE, SD_BLOCK_SIZE);
  SdWrite16 (SDHCI_BLOCK_COUNT, BlockCount);

  TransferMode = (UINT16)(SDHCI_TRNS_DMA | SDHCI_TRNS_BLK_CNT_EN);
  if (Read) {
    TransferMode |= SDHCI_TRNS_READ;
  }

  if (BlockCount > 1) {
    TransferMode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
  }

  SdWrite16 (SDHCI_TRANSFER_MODE, TransferMode);

  Status = SdSendCommand (
             Read ? (BlockCount > 1 ? SD_CMD_READ_MULTIPLE : SD_CMD_READ_SINGLE)
                  : (BlockCount > 1 ? SD_CMD_WRITE_MULTIPLE : SD_CMD_WRITE_SINGLE),
             Address,
             SDHCI_CMD_RESP_SHORT,
             (UINT16)(SDHCI_CMD_DATA | SDHCI_CMD_CRC | SDHCI_CMD_INDEX),
             &CmdResult
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Spin = 0; Spin < 1000000; Spin++) {
    IntStatus = SdRead32 (SDHCI_NORMAL_INT_STATUS);
    if ((IntStatus & SDHCI_INT_ERROR) != 0) {
      ErrStatus = SdRead32 (SDHCI_ERROR_INT_STATUS);
      SdClearStatus ();
      DEBUG ((DEBUG_ERROR, "CloverSd: data error int=0x%08x err=0x%08x\n", IntStatus, ErrStatus));
      return EFI_DEVICE_ERROR;
    }

    if ((IntStatus & SDHCI_INT_TRANS_COMPLETE) != 0) {
      break;
    }
    gBS->Stall (1);
  }

  if ((SdRead32 (SDHCI_NORMAL_INT_STATUS) & SDHCI_INT_TRANS_COMPLETE) == 0) {
    SdClearStatus ();
    DEBUG ((DEBUG_ERROR, "CloverSd: transfer never completed (state 0x%08x)\n", SdRead32 (SDHCI_PRESENT_STATE)));
    return EFI_TIMEOUT;
  }

  SdClearStatus ();

  if (Read) {
    //
    // The DMA has written fresh card data into the bounce buffer; invalidate
    // again so the copy-out reads physical memory, not the stale lines that
    // were there before the transfer.
    //
    InvalidateDataCacheRange (mAdmaBuffer, Length);
    CopyMem (Buffer, mAdmaBuffer, Length);
  }

  (VOID)HostControl;
  return EFI_SUCCESS;
}

//
// -------------------------------------------------------- card bringup (SD)
//
STATIC
EFI_STATUS
SdCardSendAppCmd (
  IN  UINT8   Index,
  IN  UINT32  Argument,
  OUT SD_CMD_RESULT *Result
  )
{
  SD_CMD_RESULT  AppResult;
  EFI_STATUS     Status;

  Status = SdSendCommand (
             SD_CMD_APP_CMD,
             (UINT32)mSd.Rca << 16,
             SDHCI_CMD_RESP_SHORT,
             (UINT16)(SDHCI_CMD_CRC | SDHCI_CMD_INDEX),
             &AppResult
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Any R1 without the APP_CMD bit means the card refused to enter app mode;
  // continuing would send the ACMD as a plain command with a different meaning.
  //
  if ((AppResult.Response[0] & BIT5) == 0) {
    DEBUG ((DEBUG_ERROR, "CloverSd: card refused APP_CMD before ACMD%d (r1=0x%08x)\n", Index, AppResult.Response[0]));
    return EFI_DEVICE_ERROR;
  }

  return SdSendCommand (
           Index,
           Argument,
           SDHCI_CMD_RESP_SHORT,
           (UINT16)(SDHCI_CMD_CRC | SDHCI_CMD_INDEX),
           Result
           );
}

//
// Wait until the card reports "transfer" and ready-for-data.  Used after any
// R1b-style operation, and as the health check after a clock change.
//
STATIC
EFI_STATUS
SdWaitCardReady (
  IN UINT32  TimeoutUs
  )
{
  SD_CMD_RESULT  Result;
  EFI_STATUS     Status;
  UINT32         Elapsed;
  UINT32         R1;

  for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 1000) {
    Status = SdSendCommand (
               SD_CMD_SEND_STATUS,
               (UINT32)mSd.Rca << 16,
               SDHCI_CMD_RESP_SHORT,
               (UINT16)(SDHCI_CMD_CRC | SDHCI_CMD_INDEX),
               &Result
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    R1 = Result.Response[0];
    if (((R1 & SD_R1_CURRENT_STATE_MASK) == (SD_R1_STATE_TRAN << 9)) &&
        ((R1 & SD_R1_READY_FOR_DATA) != 0))
    {
      return EFI_SUCCESS;
    }

    gBS->Stall (1000);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SdDecodeCsd (
  IN UINT32  *Csd
  )
{
  UINT32  Structure;

  //
  // The CSD arrives as a 128-bit R2 response.  SDHCI stores it big-endian:
  // Csd[0] holds CSD[127:96], Csd[1] holds CSD[95:64], Csd[2] holds
  // CSD[63:32], Csd[3] holds CSD[31:0].  Every field below is read with that
  // layout (the bit numbers in the SD spec are MSB-first), which is the exact
  // opposite of what a naive "low word = low bits" reading would produce.
  //
  Structure = (Csd[0] >> 30) & 0x3;   // CSD_STRUCTURE [127:126]

  if (Structure == 0) {
    //
    // CSD 1.0: SDSC.  C_SIZE[73:62], C_SIZE_MULT[49:47], READ_BL_LEN[83:80].
    // Not expected on a 64 GB card, but a card that answers this way is still
    // a card and the arithmetic is three lines.
    //
    UINT32  CSize     = ((Csd[1] & 0x3FFu) << 2) | (Csd[2] >> 30);   // [73:62]
    UINT32  CSizeMult = (Csd[2] >> 15) & 0x7;                          // [49:47]
    UINT32  ReadBlLen = (Csd[1] >> 16) & 0xF;                          // [83:80]

    mSd.HighCapacity = FALSE;
    mSd.Capacity     = ((UINT64)(CSize + 1))
                       * (UINT64)(1u << (CSizeMult + 2))
                       * (UINT64)(1u << ReadBlLen);
  } else if (Structure == 1) {
    //
    // CSD 2.0: SDHC/SDXC.  C_SIZE[69:48], capacity = (C_SIZE+1) * 512 KB.
    // C_SIZE high 6 bits [69:64] live in Csd[1][5:0]; the low 16 bits
    // [63:48] live in Csd[2][31:16].
    //
    UINT32  CSize = ((Csd[1] & 0x3Fu) << 16) | ((Csd[2] >> 16) & 0xFFFFu);

    mSd.HighCapacity = TRUE;
    mSd.Capacity     = ((UINT64)(CSize + 1)) * 512ULL * 1024ULL;
  } else {
    DEBUG ((DEBUG_ERROR, "CloverSd: unknown CSD structure %d\n", Structure));
    return EFI_UNSUPPORTED;
  }

  if (mSd.Capacity == 0) {
    return EFI_DEVICE_ERROR;
  }

  mSd.LastBlock = (UINT32)((mSd.Capacity / SD_BLOCK_SIZE) - 1);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SdCardInit (
  VOID
  )
{
  EFI_STATUS     Status;
  SD_CMD_RESULT  Result;
  UINT32         Ocr;
  UINTN          Attempt;
  UINT32         Cid[4];
  UINT32         Csd[4];
  UINT16         Rca;
  BOOLEAN        SdscV2;

  ZeroMem (&mSd, sizeof (mSd));

  //
  // CMD0 has no response, so success here says nothing about the card.  CMD8
  // is the first command that can actually fail, and it is also the one that
  // tells v1 cards from v2.
  //
  SdSendCommand (SD_CMD_GO_IDLE, 0, SDHCI_CMD_RESP_NO, 0, &Result);
  gBS->Stall (2000);

  SdscV2 = FALSE;
  Status = SdSendCommand (
             SD_CMD_SEND_IF_COND,
             0x1AA,
             SDHCI_CMD_RESP_SHORT,
             (UINT16)(SDHCI_CMD_CRC | SDHCI_CMD_INDEX),
             &Result
             );
  if (!EFI_ERROR (Status) && ((Result.Response[0] & 0xFFF) == 0x1AA)) {
    SdscV2 = TRUE;
    DEBUG ((DEBUG_INFO, "CloverSd: CMD8 echo 0x%08x -> SD v2 card\n", Result.Response[0]));
  } else {
    //
    // No CMD8 answer means either a v1 card or no card.  ACMD41 decides: a v1
    // card answers it, an empty slot times out on both.
    //
    DEBUG ((DEBUG_INFO, "CloverSd: CMD8 gave 0x%08x (v1 card or empty slot)\n", Result.Response[0]));
  }

  //
  // ACMD41 until the card clears its busy bit.  HCS is only meaningful on v2,
  // and the OCR voltage window is the 3.2-3.4 V pair this slot can supply.
  //
  Ocr = SD_OCR_VOLTAGE_3V2_3V3 | SD_OCR_VOLTAGE_3V3_3V4;
  if (SdscV2) {
    Ocr |= SD_OCR_HCS;
  }

  for (Attempt = 0; Attempt < 100; Attempt++) {
    Status = SdCardSendAppCmd (SD_ACMD_SEND_OP_COND, Ocr, &Result);
    if (EFI_ERROR (Status)) {
      if (Attempt < 3) {
        gBS->Stall (10000);
        continue;
      }
      DEBUG ((DEBUG_ERROR, "CloverSd: ACMD41 failed, no card in the slot\n"));
      return EFI_NOT_FOUND;
    }

    if ((Result.Response[0] & BIT31) != 0) {
      break;
    }
    gBS->Stall (10000);
  }

  if (Attempt >= 100) {
    DEBUG ((DEBUG_ERROR, "CloverSd: ACMD41 never went ready\n"));
    return EFI_TIMEOUT;
  }

  mSd.HighCapacity = (Result.Response[0] & SD_OCR_HCS) != 0;
  DEBUG ((DEBUG_INFO, "CloverSd: OCR 0x%08x high-capacity=%d\n", Result.Response[0], mSd.HighCapacity));

  Status = SdSendCommand (SD_CMD_ALL_SEND_CID, 0, SDHCI_CMD_RESP_LONG, (UINT16)(SDHCI_CMD_CRC | SDHCI_CMD_INDEX), &Result);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  CopyMem (Cid, Result.Response, sizeof (Cid));

  //
  // The CID is the only identity the card offers before it is selected, and
  // it is what tells two otherwise identical 64 GB cards apart in a log.
  //
  DEBUG ((
    DEBUG_INFO,
    "CloverSd: CID %08x%08x%08x%08x\n",
    Cid[3], Cid[2], Cid[1], Cid[0]
    ));

  Status = SdSendCommand (SD_CMD_SEND_REL_ADDR, 0, SDHCI_CMD_RESP_SHORT, (UINT16)(SDHCI_CMD_CRC | SDHCI_CMD_INDEX), &Result);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Rca = (UINT16)(Result.Response[0] >> 16);
  if (Rca == 0) {
    DEBUG ((DEBUG_ERROR, "CloverSd: card handed out RCA 0\n"));
    return EFI_DEVICE_ERROR;
  }
  mSd.Rca = Rca;

  Status = SdSendCommand (SD_CMD_SEND_CSD, (UINT32)Rca << 16, SDHCI_CMD_RESP_LONG, (UINT16)(SDHCI_CMD_CRC | SDHCI_CMD_INDEX), &Result);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  CopyMem (Csd, Result.Response, sizeof (Csd));

  Status = SdDecodeCsd (Csd);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SdSendCommand (SD_CMD_SELECT_CARD, (UINT32)Rca << 16, SDHCI_CMD_RESP_SHORT_BUSY, (UINT16)(SDHCI_CMD_CRC | SDHCI_CMD_INDEX), &Result);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  SdWaitCardReady (100000);

  DEBUG ((
    DEBUG_INFO,
    "CloverSd: RCA=0x%04x capacity=%ld MB lastBlock=%d\n",
    Rca, (UINT64)(mSd.Capacity / (1024 * 1024)), mSd.LastBlock
    ));

  //
  // 512-byte blocks and 4-bit bus.  A high-capacity card ignores CMD16, so a
  // failure there is not fatal.
  //
  SdSendCommand (SD_CMD_SET_BLOCKLEN, SD_BLOCK_SIZE, SDHCI_CMD_RESP_SHORT, (UINT16)(SDHCI_CMD_CRC | SDHCI_CMD_INDEX), &Result);

  Status = SdCardSendAppCmd (SD_ACMD_SET_CLR_CARD_DETECT, 0, &Result);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "CloverSd: ACMD42 not accepted (harmless)\n"));
  }

  Status = SdCardSendAppCmd (SD_ACMD_SET_BUS_WIDTH, 2, &Result);
  if (!EFI_ERROR (Status)) {
    UINT8  HostControl = SdRead8 (SDHCI_HOST_CONTROL);
    HostControl |= SDHCI_HOST_CTRL_4BIT;
    SdWrite8 (SDHCI_HOST_CONTROL, HostControl);
    mSd.BusWidth4 = TRUE;
    DEBUG ((DEBUG_INFO, "CloverSd: 4-bit bus enabled\n"));
  } else {
    DEBUG ((DEBUG_INFO, "CloverSd: 4-bit bus refused, staying 1-bit\n"));
  }

  return EFI_SUCCESS;
}

//
// Raise the root clock and prove the card still answers.  Any failure puts the
// clock back to the identification speed, because a card that cannot be
// reached at 25 MHz can still be read at 400 kHz and that is strictly better
// than no block device.
//
STATIC
VOID
SdRaiseClock (
  VOID
  )
{
  UINT8  HostControl;

  SdHostSetClock (FALSE);
  GccSetRate (GCC_CFG_50M, 0, 0, 0);
  gBS->Stall (1000);
  SdHostSetClock (TRUE);

  //
  // High-speed timing has to be declared to the host as well as clocked: the
  // bit changes the sampling point, and a card running at 50 MHz with
  // default-speed sampling is exactly the failure mode that reads as a bad
  // card rather than a bad setting.
  //
  HostControl  = SdRead8 (SDHCI_HOST_CONTROL);
  HostControl |= SDHCI_HOST_CTRL_HISPEED;
  SdWrite8 (SDHCI_HOST_CONTROL, HostControl);

  gBS->Stall (2000);

  if (!EFI_ERROR (SdWaitCardReady (50000))) {
    mSd.ClockKhz = 50000;
    DEBUG ((DEBUG_INFO, "CloverSd: running at 50 MHz, card answers CMD13\n"));
    return;
  }

  DEBUG ((DEBUG_INFO, "CloverSd: 50 MHz lost the card, falling back to 400 kHz\n"));

  SdHostSetClock (FALSE);
  HostControl  = SdRead8 (SDHCI_HOST_CONTROL);
  HostControl &= (UINT8)~SDHCI_HOST_CTRL_HISPEED;
  SdWrite8 (SDHCI_HOST_CONTROL, HostControl);
  GccSetRate (GCC_CFG_400K, GCC_M_400K, GCC_N_400K, GCC_D_400K);
  gBS->Stall (1000);
  SdHostSetClock (TRUE);
  gBS->Stall (1000);
  mSd.ClockKhz = 400;

  if (EFI_ERROR (SdWaitCardReady (50000))) {
    DEBUG ((DEBUG_ERROR, "CloverSd: card did not survive the clock round trip\n"));
  }
}

//
// ------------------------------------------------------------- BlockIo API
//
STATIC
EFI_STATUS
EFIAPI
SdBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  (VOID)This;
  (VOID)ExtendedVerification;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SdBlockRead (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  OUT VOID                  *Buffer
  )
{
  EFI_STATUS  Status;
  UINTN       Offset;
  UINTN       Chunk;
  UINTN       Remaining;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }
  if ((BufferSize % SD_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (MediaId != mSdMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (Lba > mSdMedia.LastBlock) {
    return EFI_INVALID_PARAMETER;
  }

  Remaining = BufferSize;
  Offset    = 0;

  while (Remaining > 0) {
    //
    // One descriptor covers the whole transfer, so the chunk size is bounded
    // by the descriptor length field (65536 blocks is the encoding limit, but
    // 64 blocks keeps a single ADMA buffer inside the pages allocated below).
    //
    Chunk = Remaining / SD_BLOCK_SIZE;
    if (Chunk > 64) {
      Chunk = 64;
    }

    Status = SdTransfer (
               (UINT32)((Lba + (Offset / SD_BLOCK_SIZE)) * (mSd.HighCapacity ? 1 : SD_BLOCK_SIZE)),
               (UINT16)Chunk,
               (UINT8 *)Buffer + Offset,
               TRUE
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Offset    += Chunk * SD_BLOCK_SIZE;
    Remaining -= Chunk * SD_BLOCK_SIZE;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SdBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  EFI_STATUS  Status;
  UINTN       Offset;
  UINTN       Chunk;
  UINTN       Remaining;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }
  if ((BufferSize % SD_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (MediaId != mSdMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (mSdMedia.ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }
  if (Lba > mSdMedia.LastBlock) {
    return EFI_INVALID_PARAMETER;
  }

  Remaining = BufferSize;
  Offset    = 0;

  while (Remaining > 0) {
    Chunk = Remaining / SD_BLOCK_SIZE;
    if (Chunk > 64) {
      Chunk = 64;
    }

    Status = SdTransfer (
               (UINT32)((Lba + (Offset / SD_BLOCK_SIZE)) * (mSd.HighCapacity ? 1 : SD_BLOCK_SIZE)),
               (UINT16)Chunk,
               (UINT8 *)Buffer + Offset,
               FALSE
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Offset    += Chunk * SD_BLOCK_SIZE;
    Remaining -= Chunk * SD_BLOCK_SIZE;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SdBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  (VOID)This;
  //
  // No cache on the host side; the card commits on its own.  Waiting for
  // ready-for-data is the closest thing to a flush this class supports.
  //
  return SdWaitCardReady (1000000);
}

//
// ------------------------------------------------------------------ driver
//
STATIC
VOID
SdInitDevicePath (
  VOID
  )
{
  ZeroMem (&mSdDevicePath, sizeof (mSdDevicePath));

  //
  // Use the standard SD device path node (Messaging / SD_DP), not a vendor
  // node.  SimpleInit and ConvertDevicePathToText know how to render this one,
  // and a vendor node with a made-up GUID is exactly the kind of input that
  // makes a device-path walker compute a wrong string and then abort in a
  // string routine.
  //
  mSdDevicePath.Sd.Header.Type    = MESSAGING_DEVICE_PATH;
  mSdDevicePath.Sd.Header.SubType = MSG_SD_DP;
  mSdDevicePath.Sd.Header.Length[0] = (UINT8)(sizeof (SD_DEVICE_PATH) & 0xFF);
  mSdDevicePath.Sd.Header.Length[1] = (UINT8)(sizeof (SD_DEVICE_PATH) >> 8);
  mSdDevicePath.Sd.SlotNumber       = 0;

  mSdDevicePath.End.Type    = END_DEVICE_PATH_TYPE;
  mSdDevicePath.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  mSdDevicePath.End.Length[0] = (UINT8)(sizeof (EFI_DEVICE_PATH_PROTOCOL));
  mSdDevicePath.End.Length[1] = 0;
}

/**
  Entry point.

  Order matters: clocks before the host is touched at all, because every
  register read in this window is gated by both GCC branches and an ungated
  read returns 0 (which would look exactly like "no controller here").

  @param[in] ImageHandle  Image handle of this driver.
  @param[in] SystemTable  Pointer to the EFI System Table.

  @retval EFI_SUCCESS     A BlockIo was published.
  @retval EFI_NOT_FOUND   No card in the slot, or the controller did not answer.
**/
EFI_STATUS
EFIAPI
CloverSdDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Caps;
  UINT16      Version;

  DEBUG ((DEBUG_INFO, "\nCloverSd: probing sdhci@c084000\n"));

  //
  // 1. Enable the SDCC2 clocks FIRST.  This driver runs after ClockDxe (its
  //    Depex is gEfiClockProtocolGuid), but ClockDxe only brings up the clocks
  //    the boot path uses -- eMMC's SDCC1 -- and leaves SDCC2 gated.  Reading
  //    a gated controller is not a harmless zero: the AHB decode returns a
  //    Synchronous external abort (FAR 0xC0840FE was the HOST_VERSION read),
  //    which is exactly the crash this fixes.  So the AHB branch must be on
  //    before the first load from the host window, with no probe first.
  //
  //    The GCC block itself (0x100000) is always powered and mapped, so these
  //    writes are safe; they only touch SDCC2's two branch registers.
  //
  GccEnableBranch (GCC_SDCC2_AHB_CBCR);
  GccEnableBranch (GCC_SDCC2_APPS_CBCR);
  GccSetRate (GCC_CFG_400K, GCC_M_400K, GCC_N_400K, GCC_D_400K);
  gBS->Stall (1000);

  //
  // 2. Now the first host access is safe.
  //
  Version = SdRead16 (SDHCI_HOST_VERSION);
  DEBUG ((DEBUG_INFO, "CloverSd: host version 0x%04x\n", Version));

  if (Version == 0 || Version == 0xFFFF) {
    DEBUG ((DEBUG_ERROR, "CloverSd: host did not answer at 0x%x (read 0x%04x)\n", CLOVER_SDCC2_BASE, Version));
    return EFI_NOT_FOUND;
  }

  //
  // 3. Vendor registers, read-only unless the read looks like a real register.
  //
  //    0x20c / 0x250 are the sdhci-msm-v5 vendor offsets measured on this
  //    SoC's eMMC controller (SDCC1), where VENDOR_SPEC reads 2 << 8 and
  //    VENDOR_SPEC3 reads 0x02626440.  SDCC2 is the same IP and should answer
  //    the same way, but "should" is not a reading: a zero or all-ones value
  //    means this offset is not what we think, and writing a guess into an
  //    unknown register on a board with no EDL is not a risk worth taking.
  //
  //    Nothing here is required for the ADMA2 path -- MCLK select defaults to
  //    the free-running clock and the alternative FIFO only matters to PIO --
  //    so on doubt the driver just prints and moves on.
  //
  {
    UINT32  VendorSpec  = SdRead32 (SDHCI_VENDOR_SPEC);
    UINT32  VendorSpec3 = SdRead32 (SDHCI_VENDOR_SPEC3);

    DEBUG ((DEBUG_INFO, "CloverSd: vendor_spec=0x%08x vendor_spec3=0x%08x\n", VendorSpec, VendorSpec3));

    if ((VendorSpec != 0) && (VendorSpec != 0xFFFFFFFF)) {
      if ((VendorSpec & VENDOR_SPEC_MCLK_SEL_MASK) != VENDOR_SPEC_MCLK_SEL_DFLT) {
        DEBUG ((DEBUG_INFO, "CloverSd: MCLK select was 0x%x, forcing free-running\n",
                (VendorSpec & VENDOR_SPEC_MCLK_SEL_MASK) >> 8));
        VendorSpec &= ~VENDOR_SPEC_MCLK_SEL_MASK;
        VendorSpec |= VENDOR_SPEC_MCLK_SEL_DFLT;
        SdWrite32 (SDHCI_VENDOR_SPEC, VendorSpec);
      }
    } else {
      DEBUG ((DEBUG_INFO, "CloverSd: vendor_spec looks unimplemented, leaving it alone\n"));
    }

    if ((VendorSpec3 != 0) && (VendorSpec3 != 0xFFFFFFFF)) {
      if ((VendorSpec3 & VENDOR_SPEC3_FIFO_ALT_EN) != 0) {
        SdWrite32 (SDHCI_VENDOR_SPEC3, VendorSpec3 & ~VENDOR_SPEC3_FIFO_ALT_EN);
      }
    }
  }

  //
  // 4. Reset.  RESET_ALL is allowed to fail on this family (the eMMC port saw
  //    it never self-clear once); CMD/DAT reset is the fallback that is always
  //    honoured.
  //
  SdHostSetClock (FALSE);
  if (EFI_ERROR (SdHostReset (SDHCI_RESET_ALL))) {
    DEBUG ((DEBUG_INFO, "CloverSd: RESET_ALL did not clear, using CMD+DAT reset\n"));
    SdHostReset ((UINT8)(SDHCI_RESET_CMD | SDHCI_RESET_DAT));
  }
  gBS->Stall (2000);

  SdHostLedOff ();

  SdWrite8 (SDHCI_TIMEOUT_CONTROL, 0x0E);
  SdClearStatus ();
  SdWrite16 (SDHCI_NORMAL_INT_STS_EN, 0xFFFF);
  SdWrite16 (SDHCI_ERROR_INT_STS_EN, 0xFFFF);
  SdWrite16 (SDHCI_NORMAL_INT_SIG_EN, 0x0000);
  SdWrite16 (SDHCI_ERROR_INT_SIG_EN, 0x0000);

  Caps = SdRead32 (SDHCI_CAPABILITIES);
  DEBUG ((DEBUG_INFO, "CloverSd: capabilities=0x%08x\n", Caps));

  if ((Caps & SDHCI_CAP_ADMA2) == 0) {
    DEBUG ((DEBUG_ERROR, "CloverSd: no ADMA2 on this host, refusing to guess a data path\n"));
    return EFI_UNSUPPORTED;
  }

  mSd.BaseClockMhz = (Caps & SDHCI_CAP_BASE_MHZ_MASK) >> SDHCI_CAP_BASE_MHZ_SHIFT;
  if (mSd.BaseClockMhz == 0) {
    mSd.BaseClockMhz = 200;
  }

  //
  // 5. Power.  3.3 V, then on.  The rail itself is board-fixed (no
  //    vqmmc-supply in the device tree), so this only selects the signalling
  //    voltage the host drives.
  //
  SdWrite8 (SDHCI_POWER_CONTROL, SDHCI_POWER_330);
  gBS->Stall (1000);
  SdWrite8 (SDHCI_POWER_CONTROL, (UINT8)(SDHCI_POWER_330 | SDHCI_POWER_ON));
  gBS->Stall (2000);

  SdHostSetClock (FALSE);
  GccSetRate (GCC_CFG_400K, GCC_M_400K, GCC_N_400K, GCC_D_400K);
  gBS->Stall (1000);
  if (EFI_ERROR (SdHostSetClock (TRUE))) {
    return EFI_DEVICE_ERROR;
  }
  gBS->Stall (2000);

  //
  // 6. ADMA2 scratch: one descriptor (16 bytes) has to be 8-byte aligned, and
  //    the bounce buffer is 64 blocks so a chunk never needs a second
  //    descriptor.
  //
  mAdmaBufferPages = EFI_SIZE_TO_PAGES (64 * SD_BLOCK_SIZE);
  mAdmaBuffer      = AllocatePages (mAdmaBufferPages);
  if (mAdmaBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  ZeroMem (mAdmaBuffer, 64 * SD_BLOCK_SIZE);

  mAdmaTable = AllocateAlignedPages (EFI_SIZE_TO_PAGES (sizeof (ADMA2_DESC64)), 64);
  if (mAdmaTable == NULL) {
    FreePages (mAdmaBuffer, mAdmaBufferPages);
    return EFI_OUT_OF_RESOURCES;
  }
  ZeroMem (mAdmaTable, sizeof (ADMA2_DESC64));

  //
  // 7. The card.
  //
  Status = SdCardInit ();

  if (EFI_ERROR (Status)) {
    //
    // Nothing was published, so the scratch memory goes back.  On the success
    // path it stays: every later BlockIo call runs through this descriptor
    // table and this bounce buffer.
    //
    FreePages (mAdmaBuffer, mAdmaBufferPages);
    FreePages (mAdmaTable, EFI_SIZE_TO_PAGES (sizeof (ADMA2_DESC64)));
    mAdmaBuffer = NULL;
    mAdmaTable  = NULL;
    DEBUG ((DEBUG_INFO, "CloverSd: no usable card (%r), nothing published\n", Status));
    Print (L"\n[CloverSd] no card / init failed (%r)\n", Status);
    return Status;
  }

  //
  // 8. Try the faster clock.  A refusal costs throughput and nothing else,
  //    because SdRaiseClock puts the identification clock back itself.
  //
  SdRaiseClock ();

  //
  // 9. Read LBA 0 once to prove the data path.  A card whose first sector does
  //    not look like a partition table (GPT "EFI PART" magic, or an MBR
  //    0x55AA at offset 510) is either unreadable through ADMA or genuinely
  //    blank.  Either way, publishing a BlockIo that returns garbage is what
  //    made SimpleInit walk a bad pointer and abort -- so a failed self-test
  //    must NOT publish.  Report with DEBUG_ERROR, which RELEASE keeps and
  //    FrameBufferSerialPortLib paints, so the verdict is visible on screen.
  //
  {
    UINT8        Probe[SD_BLOCK_SIZE];
    BOOLEAN      LooksPartitioned;
    EFI_STATUS   ProbeStatus;

    ZeroMem (Probe, sizeof (Probe));
    ProbeStatus = SdTransfer (0, 1, Probe, TRUE);

    LooksPartitioned = FALSE;
    if (!EFI_ERROR (ProbeStatus)) {
      //
      // GPT magic is the ASCII "EFI PART" at byte 0.
      //
      if ((Probe[0] == 'E') && (Probe[1] == 'F') && (Probe[2] == 'I') &&
          (Probe[3] == ' ') && (Probe[4] == 'P') && (Probe[5] == 'A') &&
          (Probe[6] == 'R') && (Probe[7] == 'T'))
      {
        LooksPartitioned = TRUE;
      }

      //
      // MBR boot signature is 0x55 0xAA at bytes 510..511.
      //
      if ((Probe[510] == 0x55) && (Probe[511] == 0xAA)) {
        LooksPartitioned = TRUE;
      }
    }

    if (!LooksPartitioned) {
      DEBUG ((
        DEBUG_ERROR,
        "CloverSd: LBA0 self-test failed (status %r), first bytes "
        "%02x %02x %02x %02x %02x %02x %02x %02x -- NOT publishing BlockIo\n",
        ProbeStatus,
        Probe[0], Probe[1], Probe[2], Probe[3],
        Probe[4], Probe[5], Probe[6], Probe[7]
        ));

      FreePages (mAdmaBuffer, mAdmaBufferPages);
      FreePages (mAdmaTable, EFI_SIZE_TO_PAGES (sizeof (ADMA2_DESC64)));
      mAdmaBuffer = NULL;
      mAdmaTable  = NULL;
      return EFI_DEVICE_ERROR;
    }

    DEBUG ((DEBUG_ERROR, "CloverSd: LBA0 self-test OK (GPT or MBR magic present)\n"));
  }

  ZeroMem (&mSdMedia, sizeof (mSdMedia));
  mSdMedia.MediaId     = 0x5D0C10E5;
  mSdMedia.RemovableMedia = TRUE;
  mSdMedia.MediaPresent   = TRUE;
  mSdMedia.LogicalPartition = FALSE;
  mSdMedia.ReadOnly       = FALSE;
  mSdMedia.WriteCaching   = FALSE;
  mSdMedia.BlockSize      = SD_BLOCK_SIZE;
  mSdMedia.IoAlign        = 0;
  mSdMedia.LastBlock      = mSd.LastBlock;

  mSdBlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION2;
  mSdBlockIo.Media       = &mSdMedia;
  mSdBlockIo.Reset       = SdBlockReset;
  mSdBlockIo.ReadBlocks  = SdBlockRead;
  mSdBlockIo.WriteBlocks = SdBlockWrite;
  mSdBlockIo.FlushBlocks = SdBlockFlush;

  SdInitDevicePath ();

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mSdHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mSdDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mSdBlockIo,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CloverSd: install failed %r\n", Status));
    return Status;
  }

  DEBUG ((
    DEBUG_ERROR,
    "CloverSd: published BlockIo, %ld MB, %d blocks, RCA 0x%04x, %d-bit, %d kHz\n",
    (UINT64)(mSd.Capacity / (1024 * 1024)),
    mSd.LastBlock + 1,
    mSd.Rca,
    mSd.BusWidth4 ? 4 : 1,
    mSd.ClockKhz
    ));

  //
  // Print the verdict through the standard text console too.  ConOut goes to
  // the framebuffer once SimpleFbDxe installs, so this line is readable on the
  // panel; DEBUG_ERROR above goes to the same place via the serial port lib.
  //
  Print (L"\n[CloverSd] SD card OK: %ld MB, %d blocks, RCA 0x%04x, %d-bit\n",
         (UINT64)(mSd.Capacity / (1024 * 1024)),
         mSd.LastBlock + 1,
         mSd.Rca,
         mSd.BusWidth4 ? 4 : 1);

  return EFI_SUCCESS;
}
