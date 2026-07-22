/** @file

  NOR Flash Driver - Common functions shared between DXE and StandaloneMM.

  SPDX-FileCopyrightText: Copyright (c) 2018-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <NorFlashPrivate.h>

STATIC BOOLEAN  TimeOutMessage = FALSE;

/**
  Read a register in the NOR Flash

  @param[in]  Private               Driver's private data
  @param[in]  Cmd                   Register to be read.
  @param[in]  CmdSize               Length of command.
  @param[out] Resp                  Pointer for register data.

  @retval EFI_SUCCESS              Operation successful.
  @retval others                   Error occurred
**/
EFI_STATUS
ReadNorFlashRegister (
  IN  NOR_FLASH_PRIVATE_DATA  *Private,
  IN  UINT8                   *Cmd,
  IN  UINT32                  CmdSize,
  OUT UINT8                   *Resp
  )
{
  EFI_STATUS               Status;
  QSPI_TRANSACTION_PACKET  Packet;

  if ((Private == NULL) ||
      (Resp == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Packet.TxBuf      = Cmd;
  Packet.RxBuf      = Resp;
  Packet.TxLen      = CmdSize;
  Packet.RxLen      = sizeof (UINT8);
  Packet.WaitCycles = 0;
  Packet.ChipSelect = Private->QspiChipSelect;
  Packet.Control    = QSPI_CONTROLLER_CONTROL_FAST_MODE;

  Status = Private->QspiController->PerformTransaction (Private->QspiController, &Packet);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Could not read NOR flash register.\n", __FUNCTION__));
  }

  return Status;
}

/**
  Wait for Write Complete

  @param[in] Private               Driver's private data

  @retval EFI_SUCCESS              Operation successful.
  @retval others                   Error occurred
**/
EFI_STATUS
WaitNorFlashWriteComplete (
  IN NOR_FLASH_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;
  UINT8       RegCmd;
  UINT8       Resp;
  UINT32      Count;

  if (Private == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  RegCmd = NOR_READ_SR1;

  Count = 0;

  do {
    // Error out of retry count exceeds NOR_SR1_WIP_RETRY_CNT
    if (Count == NOR_SR1_WIP_RETRY_CNT) {
      Count = 0;
      if (TimeOutMessage == FALSE) {
        DEBUG ((DEBUG_ERROR, "%a: NOR flash write transactions slower than usual.\n", __FUNCTION__));
        TimeOutMessage = TRUE;
      }
    }

    // Read WIP status
    Status = ReadNorFlashRegister (Private, &RegCmd, sizeof (RegCmd), &Resp);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Could not read NOR flash status 1 register.\n", __FUNCTION__));
      return Status;
    }

    Count++;
  } while ((Resp & NOR_SR1_WIP_BMSK) != 0);

  DEBUG ((DEBUG_INFO, "%a: NOR flash write complete.\n", __FUNCTION__));
  return Status;
}

/**
  Configure write enable latch

  @param[in] Private               Driver's private data
  @param[in] Enable                Enable or disable latching

  @retval EFI_SUCCESS              Operation successful.
  @retval others                   Error occurred
**/
EFI_STATUS
ConfigureNorFlashWriteEnLatch (
  IN NOR_FLASH_PRIVATE_DATA  *Private,
  IN BOOLEAN                 Enable
  )
{
  EFI_STATUS               Status;
  UINT8                    Cmd;
  UINT8                    RegCmd;
  QSPI_TRANSACTION_PACKET  Packet;
  UINT8                    Resp;
  UINT8                    Cmp;
  UINT32                   Count;

  if (Private == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Cmd = Enable ? NOR_WREN_ENABLE : NOR_WREN_DISABLE;
  Cmp = Enable ? NOR_SR1_WEL_BMSK : 0;

  Packet.TxBuf      = &Cmd;
  Packet.RxBuf      = NULL;
  Packet.TxLen      = sizeof (Cmd);
  Packet.RxLen      = 0;
  Packet.WaitCycles = 0;
  Packet.ChipSelect = Private->QspiChipSelect;
  Packet.Control    = QSPI_CONTROLLER_CONTROL_FAST_MODE;

  RegCmd = NOR_READ_SR1;

  Count = 0;

  do {
    // Error out of retry count exceeds NOR_SR1_WEL_RETRY_CNT
    if (Count == NOR_SR1_WEL_RETRY_CNT) {
      Count = 0;
      if (TimeOutMessage == FALSE) {
        DEBUG ((DEBUG_ERROR, "%a: NOR flash write enable latch slower than usual.\n", __FUNCTION__));
        TimeOutMessage = TRUE;
      }
    }

    // Configure WREN
    Status = Private->QspiController->PerformTransaction (Private->QspiController, &Packet);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Could not program WREN latch.\n", __FUNCTION__));
      return Status;
    }

    // Read WREN status
    Status = ReadNorFlashRegister (Private, &RegCmd, sizeof (RegCmd), &Resp);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Could not read NOR flash status 1 register.\n", __FUNCTION__));
      return Status;
    }

    Count++;
  } while ((Resp & NOR_SR1_WEL_BMSK) != Cmp);

  DEBUG ((DEBUG_INFO, "%a: NOR flash WREN %s.\n", __FUNCTION__, Enable ? L"enabled" : L"disabled"));
  return Status;
}

/**
  Read NOR Flash's SFDP

  @param[in] Private               Driver's private data

  @retval EFI_SUCCESS              Operation successful.
  @retval others                   Error occurred
**/
EFI_STATUS
ReadNorFlashSFDP (
  IN NOR_FLASH_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS                        Status;
  UINT8                             *Cmd;
  UINT32                            CmdSize;
  INT32                             Count;
  UINT32                            AddressShift;
  UINT32                            Offset;
  UINT32                            SFDPSignature;
  NOR_SFDP_HDR                      SFDPHeader;
  NOR_SFDP_PARAM_TBL_HDR            *SFDPParamTblHeaders;
  NOR_SFDP_PARAM_TBL_HDR            *SFDPParamBasicTblHeader;
  NOR_SFDP_PARAM_TBL_HDR            *SFDPParam4ByteInstructionTblHeader;
  NOR_SFDP_PARAM_TBL_HDR            *SFDPParamSectorTblHeader;
  NOR_SFDP_PARAM_BASIC_TBL          *SFDPParamBasicTbl;
  UINT32                            SFDPParamBasicTblSize;
  NOR_SFDP_PARAM_4BI_TBL            *SFDPParam4ByteInstructionTbl;
  UINT32                            SFDPParam4ByteInstructionTblSize;
  NOR_SFDP_PARAM_SECTOR_DESCRIPTOR  *SFDPParamSectorTbl;
  UINT32                            SFDPParamSectorTblSize;
  NOR_SFDP_PARAM_SECTOR_REGION      *SFDPParamSectorTblRegion;
  NOR_SFDP_PARAM_SECTOR_REGION      *SFDPParamSectorTblFirstRegion;
  UINT8                             NumRegions;
  UINT32                            MemoryDensity;
  QSPI_TRANSACTION_PACKET           Packet;

  if (Private == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Cmd                          = NULL;
  SFDPParamTblHeaders          = NULL;
  SFDPParamBasicTbl            = NULL;
  SFDPParam4ByteInstructionTbl = NULL;
  SFDPParamSectorTbl           = NULL;

  // Read SFDP Header
  CmdSize = NOR_CMD_SIZE + NOR_SFDP_ADDR_SIZE;
  Cmd     = AllocateZeroPool (CmdSize);
  if (Cmd == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Cmd[0] = NOR_READ_SFDP_CMD;

  ZeroMem (&SFDPHeader, sizeof (SFDPHeader));

  Packet.TxBuf      = Cmd;
  Packet.RxBuf      = &SFDPHeader;
  Packet.TxLen      = CmdSize;
  Packet.RxLen      = sizeof (SFDPHeader);
  Packet.WaitCycles = NOR_SFDP_WAIT_CYCLES;
  Packet.ChipSelect = Private->QspiChipSelect;
  Packet.Control    = 0;

  Status = Private->QspiController->PerformTransaction (Private->QspiController, &Packet);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Could not read NOR flash's SFDP header.\n", __FUNCTION__));
    goto ErrorExit;
  }

  // Verify the read SFDP signature
  SFDPSignature = NOR_SFDP_SIGNATURE;
  if (0 != CompareMem (&SFDPHeader.SFDPSignature, &SFDPSignature, sizeof (SFDPHeader.SFDPSignature))) {
    DEBUG ((DEBUG_ERROR, "%a: NOR flash's SFDP signature invalid.\n", __FUNCTION__));
    Status = EFI_NOT_FOUND;
    goto ErrorExit;
  }

  // Read all parameter table headers
  Offset       = sizeof (SFDPHeader);
  AddressShift = 0;
  for (Count = (CmdSize - 1); Count > 0; Count--) {
    Cmd[Count]    = (Offset & (0xFF << AddressShift)) >> AddressShift;
    AddressShift += 8;
  }

  Cmd[0] = NOR_READ_SFDP_CMD;

  SFDPParamTblHeaders = AllocateZeroPool ((SFDPHeader.NumParamHdrs + 1) * sizeof (NOR_SFDP_PARAM_TBL_HDR));
  if (SFDPParamTblHeaders == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorExit;
  }

  Packet.TxBuf      = Cmd;
  Packet.RxBuf      = SFDPParamTblHeaders;
  Packet.TxLen      = CmdSize;
  Packet.RxLen      = (SFDPHeader.NumParamHdrs + 1) * sizeof (NOR_SFDP_PARAM_TBL_HDR);
  Packet.WaitCycles = NOR_SFDP_WAIT_CYCLES;
  Packet.ChipSelect = Private->QspiChipSelect;
  Packet.Control    = 0;

  Status = Private->QspiController->PerformTransaction (Private->QspiController, &Packet);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Could not read NOR flash's SFDP parameter table headers.\n", __FUNCTION__));
    goto ErrorExit;
  }

  // Find the last basic parameter table header
  for (Count = SFDPHeader.NumParamHdrs; Count >= 0; Count--) {
    if ((SFDPParamTblHeaders[Count].ParamIDLSB == NOR_SFDP_PRM_TBL_BSC_HDR_LSB) &&
        (SFDPParamTblHeaders[Count].ParamIDMSB == NOR_SFDP_PRM_TBL_HDR_MSB))
    {
      break;
    }
  }

  if (Count < 0) {
    DEBUG ((DEBUG_ERROR, "%a: Could not find compatible NOR flash's SFDP parameter table header.\n", __FUNCTION__));
    Status = EFI_UNSUPPORTED;
    goto ErrorExit;
  }

  SFDPParamBasicTblHeader = &SFDPParamTblHeaders[Count];

  // Use this basic parameter table header to load the full table
  Offset       = SFDPParamBasicTblHeader->ParamTblOffset;
  AddressShift = 0;
  for (Count = (CmdSize - 1); Count > 0; Count--) {
    Cmd[Count]    = (Offset & (0xFF << AddressShift)) >> AddressShift;
    AddressShift += 8;
  }

  Cmd[0] = NOR_READ_SFDP_CMD;

  SFDPParamBasicTblSize = SFDPParamBasicTblHeader->ParamTblLen * sizeof (UINT32);
  SFDPParamBasicTbl     = AllocateZeroPool (SFDPParamBasicTblSize);
  if (SFDPParamBasicTbl == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorExit;
  }

  Packet.TxBuf      = Cmd;
  Packet.RxBuf      = SFDPParamBasicTbl;
  Packet.TxLen      = CmdSize;
  Packet.RxLen      = SFDPParamBasicTblSize;
  Packet.WaitCycles = NOR_SFDP_WAIT_CYCLES;
  Packet.ChipSelect = Private->QspiChipSelect;
  Packet.Control    = 0;

  Status = Private->QspiController->PerformTransaction (Private->QspiController, &Packet);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Could not read NOR flash's SFDP parameters.\n", __FUNCTION__));
    goto ErrorExit;
  }

  // Calculate memory density in bytes.
  MemoryDensity = SFDPParamBasicTbl->MemoryDensity;

  if (MemoryDensity & BIT31) {
    MemoryDensity &= ~BIT31;
    if (MemoryDensity < 32) {
      DEBUG ((DEBUG_ERROR, "%a: NOR flash's memory density unsupported.\n", __FUNCTION__));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
    }

    Private->PrivateFlashAttributes.FlashAttributes.MemoryDensity = (UINT64)1 << (MemoryDensity - 3);
  } else {
    MemoryDensity++;
    MemoryDensity                                               >>= 3;
    Private->PrivateFlashAttributes.FlashAttributes.MemoryDensity = MemoryDensity;
  }

  // Determine address mode from SFDP AddressBytes field and flash density.
  // AddressBytes: 0 = 3-byte only, 1 = 3 or 4 byte, 2 = 4-byte only.
  // When both are supported, 3-byte suffices for densities up to 16MB.
  switch (SFDPParamBasicTbl->AddressBytes) {
    case 0:
      Private->PrivateFlashAttributes.AddrSize = 3;
      break;
    case 1:
      Private->PrivateFlashAttributes.AddrSize =
        (Private->PrivateFlashAttributes.FlashAttributes.MemoryDensity <= SIZE_16MB) ? 3 : 4;
      break;
    case 2:
      Private->PrivateFlashAttributes.AddrSize = 4;
      break;
    default:
      DEBUG ((
        DEBUG_ERROR,
        "%a: NOR flash SFDP AddressBytes reserved value %u.\n",
        __FUNCTION__,
        SFDPParamBasicTbl->AddressBytes
        ));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: AddressBytes=%u MemoryDensity=0x%lx AddrSize=%u\n",
    __FUNCTION__,
    SFDPParamBasicTbl->AddressBytes,
    Private->PrivateFlashAttributes.FlashAttributes.MemoryDensity,
    Private->PrivateFlashAttributes.AddrSize
    ));

  if (Private->PrivateFlashAttributes.AddrSize == 4) {
    // Find the 4 byte instruction parameter table header
    for (Count = SFDPHeader.NumParamHdrs; Count >= 0; Count--) {
      if ((SFDPParamTblHeaders[Count].ParamIDLSB == NOR_SFDP_PRM_TBL_4BI_HDR_LSB) &&
          (SFDPParamTblHeaders[Count].ParamIDMSB == NOR_SFDP_PRM_TBL_HDR_MSB))
      {
        break;
      }
    }

    if (Count < 0) {
      DEBUG ((DEBUG_ERROR, "%a: Could not find compatible NOR flash's SFDP 4 byte instruction parameter table header.\n", __FUNCTION__));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
    }

    SFDPParam4ByteInstructionTblHeader = &SFDPParamTblHeaders[Count];

    // Use this 4 byte instruction parameter table header to load the full table
    Offset       = SFDPParam4ByteInstructionTblHeader->ParamTblOffset;
    AddressShift = 0;
    for (Count = (CmdSize - 1); Count > 0; Count--) {
      Cmd[Count]    = (Offset & (0xFF << AddressShift)) >> AddressShift;
      AddressShift += 8;
    }

    Cmd[0] = NOR_READ_SFDP_CMD;

    SFDPParam4ByteInstructionTblSize = SFDPParam4ByteInstructionTblHeader->ParamTblLen * sizeof (UINT32);
    SFDPParam4ByteInstructionTbl     = AllocateZeroPool (SFDPParam4ByteInstructionTblSize);
    if (SFDPParam4ByteInstructionTbl == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto ErrorExit;
    }

    Packet.TxBuf      = Cmd;
    Packet.RxBuf      = SFDPParam4ByteInstructionTbl;
    Packet.TxLen      = CmdSize;
    Packet.RxLen      = SFDPParam4ByteInstructionTblSize;
    Packet.WaitCycles = NOR_SFDP_WAIT_CYCLES;
    Packet.ChipSelect = Private->QspiChipSelect;
    Packet.Control    = 0;

    Status = Private->QspiController->PerformTransaction (Private->QspiController, &Packet);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Could not read NOR flash's SFDP 4 byte instruction parameters.\n", __FUNCTION__));
      goto ErrorExit;
    }

    // Atleast one Read type should be supported
    if ((SFDPParam4ByteInstructionTbl->ReadCmd0C == FALSE) &&
        (SFDPParam4ByteInstructionTbl->ReadCmd13 == FALSE))
    {
      DEBUG ((DEBUG_ERROR, "%a: NOR flash's single bit Read unsupported.\n", __FUNCTION__));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
    }

    // If Fast Read isn't supported override the FastReadSupport capability bool even if
    // the support
    if (SFDPParam4ByteInstructionTbl->ReadCmd0C == FALSE) {
      Private->PrivateFlashAttributes.FastReadSupport = FALSE;
    }

    // Page write has to be supported
    if (SFDPParam4ByteInstructionTbl->WriteCmd12 == FALSE) {
      DEBUG ((DEBUG_ERROR, "%a: NOR flash's single Page Write unsupported.\n", __FUNCTION__));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
    }
  }

  // Find fast read dummy cycles.
  if (SFDPParamBasicTbl->DualIOInstruction != NOR_DUAL_IO_UNSUPPORTED) {
    Private->PrivateFlashAttributes.ReadWaitCycles = SFDPParamBasicTbl->DualIODummyCycles;
  } else {
    Private->PrivateFlashAttributes.ReadWaitCycles = NOR_SFDP_FAST_READ_DEF_WAIT;
  }

  // If uniform 4K erase is supported, use that mode.
  if ((SFDPParamBasicTbl->EraseSupport4KB == NOR_SFDP_4KB_ERS_SUPPORTED) &&
      (SFDPParamBasicTbl->EraseInstruction4KB != NOR_SFDP_4KB_ERS_UNSUPPORTED))
  {
    Private->PrivateFlashAttributes.FlashAttributes.BlockSize = SIZE_4KB;
  } else {
    // Find the sector map parameter table header
    for (Count = SFDPHeader.NumParamHdrs; Count >= 0; Count--) {
      if ((SFDPParamTblHeaders[Count].ParamIDLSB == NOR_SFDP_PRM_TBL_SEC_HDR_LSB) &&
          (SFDPParamTblHeaders[Count].ParamIDMSB == NOR_SFDP_PRM_TBL_HDR_MSB))
      {
        break;
      }
    }

    if (Count < 0) {
      DEBUG ((DEBUG_ERROR, "%a: Could not find compatible NOR flash's SFDP sector parameter table header.\n", __FUNCTION__));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
    }

    SFDPParamSectorTblHeader = &SFDPParamTblHeaders[Count];

    // Use this sector map parameter table header to load the full table
    Offset       = SFDPParamSectorTblHeader->ParamTblOffset;
    AddressShift = 0;
    for (Count = (CmdSize - 1); Count > 0; Count--) {
      Cmd[Count]    = (Offset & (0xFF << AddressShift)) >> AddressShift;
      AddressShift += 8;
    }

    Cmd[0] = NOR_READ_SFDP_CMD;

    SFDPParamSectorTblSize = SFDPParamSectorTblHeader->ParamTblLen * sizeof (UINT32);
    SFDPParamSectorTbl     = AllocateZeroPool (SFDPParamSectorTblSize);
    if (SFDPParamSectorTbl == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto ErrorExit;
    }

    Packet.TxBuf      = Cmd;
    Packet.RxBuf      = SFDPParamSectorTbl;
    Packet.TxLen      = CmdSize;
    Packet.RxLen      = SFDPParamSectorTblSize;
    Packet.WaitCycles = NOR_SFDP_WAIT_CYCLES;
    Packet.ChipSelect = Private->QspiChipSelect;
    Packet.Control    = 0;

    Status = Private->QspiController->PerformTransaction (Private->QspiController, &Packet);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Could not read NOR flash's SFDP sector parameters.\n", __FUNCTION__));
      goto ErrorExit;
    }

    // From sector map parameter table, locate the map descriptor
    Count = 0;
    while (Count < SFDPParamSectorTblHeader->ParamTblLen) {
      if (!SFDPParamSectorTbl[Count].MapDescriptor) {
        // If not map descriptor, it is command descriptor which if followed by
        // data which is same size as descriptor.
        Count += 2;
        continue;
      } else {
        // If map descriptor, find number of regions in the map.
        NumRegions = SFDPParamSectorTbl[Count].RegionCount;
        Count++;
        break;
      }
    }

    if (Count >=  SFDPParamSectorTblHeader->ParamTblLen) {
      DEBUG ((DEBUG_ERROR, "%a: Could not find compatible NOR flash's SFDP sector parameter mapping table.\n", __FUNCTION__));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
    }

    // Out of the regions found in the map, find the region with biggest size.
    SFDPParamSectorTblRegion      = (NOR_SFDP_PARAM_SECTOR_REGION *)&SFDPParamSectorTbl[Count++];
    SFDPParamSectorTblFirstRegion = SFDPParamSectorTblRegion;
    while (NumRegions > 0) {
      if (((NOR_SFDP_PARAM_SECTOR_REGION *)&SFDPParamSectorTbl[Count])->RegionSize >
          SFDPParamSectorTblRegion->RegionSize)
      {
        SFDPParamSectorTblRegion = (NOR_SFDP_PARAM_SECTOR_REGION *)&SFDPParamSectorTbl[Count];
      }

      Count++;
      NumRegions--;
    }

    for (Count = 0; Count < NOR_SFDP_ERASE_COUNT; Count++) {
      if (SFDPParamSectorTblRegion->EraseTypeSupported & (1 << Count)) {
        break;
      }
    }

    if (Count >=  NOR_SFDP_ERASE_COUNT) {
      DEBUG ((DEBUG_ERROR, "%a: Could not find compatible NOR flash's SFDP sector parameter erase table.\n", __FUNCTION__));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
    }

    Private->PrivateFlashAttributes.FlashAttributes.BlockSize = 1 << SFDPParamBasicTbl->EraseType[Count].Size;

    // Out of the regions found in the map, first region is the one used for hybrid.
    for (Count = 0; Count < NOR_SFDP_ERASE_COUNT; Count++) {
      if (SFDPParamSectorTblFirstRegion->EraseTypeSupported & (1 << Count)) {
        break;
      }
    }

    if (Count >=  NOR_SFDP_ERASE_COUNT) {
      DEBUG ((DEBUG_ERROR, "%a: Could not find compatible NOR flash's SFDP first sector parameter erase table.\n", __FUNCTION__));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
    }

    Private->PrivateFlashAttributes.HybridMemoryDensity = (SFDPParamSectorTblFirstRegion->RegionSize + 1) *
                                                          NOR_SFDP_ERASE_REGION_SIZE;
    Private->PrivateFlashAttributes.HybridBlockSize = 1 << SFDPParamBasicTbl->EraseType[Count].Size;
  }

  // Look up uniform erase command based on block size.
  for (Count = 0; Count < NOR_SFDP_ERASE_COUNT; Count++) {
    if (Private->PrivateFlashAttributes.FlashAttributes.BlockSize ==
        (1 << SFDPParamBasicTbl->EraseType[Count].Size))
    {
      break;
    }
  }

  if (Count >=  NOR_SFDP_ERASE_COUNT) {
    DEBUG ((DEBUG_ERROR, "%a: Could not find compatible NOR flash's uniform block size in SFDP sector parameter erase table.\n", __FUNCTION__));
    Status = EFI_UNSUPPORTED;
    goto ErrorExit;
  }

  if (Private->PrivateFlashAttributes.AddrSize == 4) {
    if (!(SFDPParam4ByteInstructionTbl->EraseTypeSupported & (1 << Count))) {
      DEBUG ((DEBUG_ERROR, "%a: Could not find compatible NOR flash's uniform erase table supported in SFDP.\n", __FUNCTION__));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
    }

    Private->PrivateFlashAttributes.UniformEraseCmd = SFDPParam4ByteInstructionTbl->EraseInstruction[Count];
  } else {
    if (SFDPParamBasicTbl->EraseType[Count].Command == 0) {
      DEBUG ((DEBUG_ERROR, "%a: NOR flash 3-byte uniform erase command is zero.\n", __FUNCTION__));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
    }

    Private->PrivateFlashAttributes.UniformEraseCmd = SFDPParamBasicTbl->EraseType[Count].Command;
  }

  // Look up hybrid erase command based on block size if uniform block size is not already 4KB.
  if (Private->PrivateFlashAttributes.FlashAttributes.BlockSize != SIZE_4KB) {
    for (Count = 0; Count < NOR_SFDP_ERASE_COUNT; Count++) {
      if (Private->PrivateFlashAttributes.HybridBlockSize ==
          (1 << SFDPParamBasicTbl->EraseType[Count].Size))
      {
        break;
      }
    }

    if (Count >=  NOR_SFDP_ERASE_COUNT) {
      DEBUG ((DEBUG_ERROR, "%a: Could not find compatible NOR flash's hybrid block size in SFDP sector parameter erase table.\n", __FUNCTION__));
      Status = EFI_UNSUPPORTED;
      goto ErrorExit;
    }

    if (Private->PrivateFlashAttributes.AddrSize == 4) {
      if (!(SFDPParam4ByteInstructionTbl->EraseTypeSupported & (1 << Count))) {
        DEBUG ((DEBUG_ERROR, "%a: Could not find compatible NOR flash's hybrid erase table supported in SFDP.\n", __FUNCTION__));
        Status = EFI_UNSUPPORTED;
        goto ErrorExit;
      }

      Private->PrivateFlashAttributes.HybridEraseCmd = SFDPParam4ByteInstructionTbl->EraseInstruction[Count];
    } else {
      if (SFDPParamBasicTbl->EraseType[Count].Command == 0) {
        DEBUG ((DEBUG_ERROR, "%a: NOR flash 3-byte hybrid erase command is zero.\n", __FUNCTION__));
        Status = EFI_UNSUPPORTED;
        goto ErrorExit;
      }

      Private->PrivateFlashAttributes.HybridEraseCmd = SFDPParamBasicTbl->EraseType[Count].Command;
    }
  }

  // If basic parameter table size is large enough to contain the 11th DWORD,
  // parse it. Otherwise use default values
  if (SFDPParamBasicTblSize > OFFSET_OF (NOR_SFDP_PARAM_BASIC_TBL, Dword11)) {
    Private->PrivateFlashAttributes.PageSize = 1 << SFDPParamBasicTbl->PageSize;
    // Override page size for newer flashes
    if (Private->PrivateFlashAttributes.PageSize > NOR_SFDP_WRITE_DEF_PAGE) {
      // If page size if more then 256, default back to 256
      // to avoid any vendor specific configurations needed
      // to support higher page sizes.
      Private->PrivateFlashAttributes.PageSize = NOR_SFDP_WRITE_DEF_PAGE;
    }

    // Calculate program times based on JEDEC Standard 216F.02
    Private->PrivateFlashAttributes.FlashAttributes.ProgramFirstByteTimeUs =
      (SFDPParamBasicTbl->ByteProgramTypicalTimeFirst + 1) * (SFDPParamBasicTbl->ByteProgramTypicalTimeFirstUnits ? 8 : 1);
    DEBUG ((
      DEBUG_INFO,
      "%a: ProgramFirstByteTimeUs = %u (FirstByte = %u, Units = %u)\n",
      __FUNCTION__,
      Private->PrivateFlashAttributes.FlashAttributes.ProgramFirstByteTimeUs,
      SFDPParamBasicTbl->ByteProgramTypicalTimeFirst,
      SFDPParamBasicTbl->ByteProgramTypicalTimeFirstUnits
      ));
    Private->PrivateFlashAttributes.FlashAttributes.ProgramAdditionalByteTimeUs =
      (SFDPParamBasicTbl->ByteProgramTypicalTimeAdditional + 1) * (SFDPParamBasicTbl->ByteProgramTypicalTimeAdditionalUnits ? 8 : 1);
    DEBUG ((
      DEBUG_INFO,
      "%a: ProgramAdditionalByteTimeUs = %u (AdditionalByte = %u, Units = %u)\n",
      __FUNCTION__,
      Private->PrivateFlashAttributes.FlashAttributes.ProgramAdditionalByteTimeUs,
      SFDPParamBasicTbl->ByteProgramTypicalTimeAdditional,
      SFDPParamBasicTbl->ByteProgramTypicalTimeAdditionalUnits
      ));
    Private->PrivateFlashAttributes.FlashAttributes.ProgramPageTimeUs =
      (SFDPParamBasicTbl->PageProgramTypicalTime + 1) * (SFDPParamBasicTbl->PageProgramTypicalTimeUnits ? 64 : 8);
    DEBUG ((
      DEBUG_INFO,
      "%a: ProgramPageTimeUs = %u (Page = %u, Units = %u)\n",
      __FUNCTION__,
      Private->PrivateFlashAttributes.FlashAttributes.ProgramPageTimeUs,
      SFDPParamBasicTbl->PageProgramTypicalTime,
      SFDPParamBasicTbl->PageProgramTypicalTimeUnits
      ));
    Private->PrivateFlashAttributes.FlashAttributes.ProgramPageSize = 1 << SFDPParamBasicTbl->PageSize;
    DEBUG ((
      DEBUG_INFO,
      "%a: ProgramPageSize = %u (PageSize = %u)\n",
      __FUNCTION__,
      Private->PrivateFlashAttributes.FlashAttributes.ProgramPageSize,
      SFDPParamBasicTbl->PageSize
      ));
    Private->PrivateFlashAttributes.FlashAttributes.ProgramMaxTimeMultiplier = 2 * (SFDPParamBasicTbl->ProgramMaxTimeMultiplier + 1);
    DEBUG ((
      DEBUG_INFO,
      "%a: ProgramMaxTimeMultiplier = %u (Multiplier = %u)\n",
      __FUNCTION__,
      Private->PrivateFlashAttributes.FlashAttributes.ProgramMaxTimeMultiplier,
      SFDPParamBasicTbl->ProgramMaxTimeMultiplier
      ));
  } else {
    Private->PrivateFlashAttributes.PageSize                                    = NOR_SFDP_WRITE_DEF_PAGE;
    Private->PrivateFlashAttributes.FlashAttributes.ProgramFirstByteTimeUs      = NOR_SFDP_PROGRAM_FIRST_BYTE_TIME_DEFAULT;
    Private->PrivateFlashAttributes.FlashAttributes.ProgramAdditionalByteTimeUs = NOR_SFDP_PROGRAM_ADDITIONAL_BYTE_TIME_DEFAULT;
    Private->PrivateFlashAttributes.FlashAttributes.ProgramPageTimeUs           = NOR_SFDP_PROGRAM_PAGE_TIME_DEFAULT;
    Private->PrivateFlashAttributes.FlashAttributes.ProgramPageSize             = NOR_SFDP_WRITE_DEF_PAGE;
    Private->PrivateFlashAttributes.FlashAttributes.ProgramMaxTimeMultiplier    = NOR_SFDP_PROGRAM_MAX_TIME_MULTIPLIER_DEFAULT;
  }

  Private->FlashInstance = NOR_SFDP_SIGNATURE;

ErrorExit:
  if (Cmd != NULL) {
    FreePool (Cmd);
  }

  if (SFDPParamTblHeaders != NULL) {
    FreePool (SFDPParamTblHeaders);
  }

  if (SFDPParamBasicTbl != NULL) {
    FreePool (SFDPParamBasicTbl);
  }

  if (SFDPParam4ByteInstructionTbl != NULL) {
    FreePool (SFDPParam4ByteInstructionTbl);
  }

  if (SFDPParamSectorTbl != NULL) {
    FreePool (SFDPParamSectorTbl);
  }

  return Status;
}

/**
  Get NOR Flash Attributes.

  @param[in]  This                  Instance to protocol
  @param[out] Attributes            Pointer to flash attributes

  @retval EFI_SUCCESS               Operation successful.
  @retval others                    Error occurred

**/
EFI_STATUS
EFIAPI
NorFlashGetAttributes (
  IN  NVIDIA_NOR_FLASH_PROTOCOL  *This,
  OUT NOR_FLASH_ATTRIBUTES       *Attributes
  )
{
  NOR_FLASH_PRIVATE_DATA  *Private;

  if ((This == NULL) ||
      (Attributes == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Private = NOR_FLASH_PRIVATE_DATA_FROM_NOR_FLASH_PROTOCOL (This);

  CopyMem (Attributes, &Private->PrivateFlashAttributes.FlashAttributes, sizeof (NOR_FLASH_ATTRIBUTES));

  return EFI_SUCCESS;
}

/**
  Read data from NOR Flash.

  @param[in] This                  Instance to protocol
  @param[in] Offset                Offset to read from
  @param[in] Size                  Number of bytes to be read
  @param[in] Buffer                Address to read data into

  @retval EFI_SUCCESS              Operation successful.
  @retval others                   Error occurred
**/
EFI_STATUS
EFIAPI
NorFlashRead (
  IN NVIDIA_NOR_FLASH_PROTOCOL  *This,
  IN UINT32                     Offset,
  IN UINT32                     Size,
  IN VOID                       *Buffer
  )
{
  EFI_STATUS               Status;
  UINT32                   CmdSize;
  UINT32                   Count;
  UINT32                   AddressShift;
  QSPI_TRANSACTION_PACKET  Packet;
  NOR_FLASH_PRIVATE_DATA   *Private;
  UINT32                   FlashDensity;

  if ((This == NULL) ||
      (Buffer == NULL) ||
      (Size == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Private = NOR_FLASH_PRIVATE_DATA_FROM_NOR_FLASH_PROTOCOL (This);

  // Validate that read start and end offsets are within range.
  FlashDensity = Private->PrivateFlashAttributes.FlashAttributes.MemoryDensity;
  if ((Offset > (FlashDensity - 1)) ||
      ((Offset + Size) > (FlashDensity)))
  {
    return EFI_INVALID_PARAMETER;
  }

  CmdSize = NOR_CMD_SIZE + Private->PrivateFlashAttributes.AddrSize;
  ZeroMem (Private->CommandBuffer, CmdSize);

  AddressShift = 0;
  for (Count = (CmdSize - 1); Count > 0; Count--) {
    Private->CommandBuffer[Count] = (Offset & (0xFF << AddressShift)) >> AddressShift;
    AddressShift                 += 8;
  }

  if (Private->PrivateFlashAttributes.FastReadSupport) {
    if (Private->PrivateFlashAttributes.AddrSize == 4) {
      Private->CommandBuffer[0] = NOR_FAST_READ_DATA_CMD;
      Packet.WaitCycles         = Private->PrivateFlashAttributes.ReadWaitCycles;
    } else {
      // 3-byte fast read (0x0B) always uses 8 dummy cycles per JEDEC spec,
      // not the Dual IO dummy cycles field which applies to 1-1-2 reads.
      Private->CommandBuffer[0] = NOR_FAST_READ_DATA_CMD_3B;
      Packet.WaitCycles         = NOR_SFDP_FAST_READ_DEF_WAIT;
    }
  } else {
    Private->CommandBuffer[0] = (Private->PrivateFlashAttributes.AddrSize == 4) ?
                                NOR_READ_DATA_CMD : NOR_READ_DATA_CMD_3B;
    Packet.WaitCycles = 0;
  }

  Packet.TxBuf      = Private->CommandBuffer;
  Packet.TxLen      = CmdSize;
  Packet.RxBuf      = Buffer;
  Packet.RxLen      = Size;
  Packet.ChipSelect = Private->QspiChipSelect;
  Packet.Control    = QSPI_CONTROLLER_CONTROL_FAST_MODE;

  DEBUG ((
    DEBUG_INFO,
    "%a: Read Cmd %u Wait Cycles %u\n",
    __FUNCTION__,
    Private->CommandBuffer[0],
    Packet.WaitCycles
    ));

  Status = Private->QspiController->PerformTransaction (Private->QspiController, &Packet);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Could not read data from NOR flash.\n", __FUNCTION__));
    goto ErrorExit;
  }

  DEBUG ((DEBUG_INFO, "%a: Successfully read data from NOR flash.\n", __FUNCTION__));

ErrorExit:

  return Status;
}

/**
  Read data from NOR Flash.

  @param[in] This                  Instance to protocol
  @param[in] MediaId               Media ID for the device
  @param[in] Lba                   Logical block to start reading from
  @param[in] BufferSize            Number of bytes to be read
  @param[in] Buffer                Address to read data into

  @retval EFI_SUCCESS              Operation successful.
  @retval others                   Error occurred
**/
EFI_STATUS
EFIAPI
NorFlashReadBlock (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  EFI_STATUS              Status;
  NOR_FLASH_PRIVATE_DATA  *Private;

  if ((This == NULL) ||
      (Buffer == NULL) ||
      (BufferSize == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Private = NOR_FLASH_PRIVATE_DATA_FROM_BLOCK_IO_PROTOCOL (This);

  if (MediaId != Private->FlashInstance) {
    return EFI_MEDIA_CHANGED;
  }

  Status = NorFlashRead (
             &Private->NorFlashProtocol,
             (Lba * Private->PrivateFlashAttributes.FlashAttributes.BlockSize),
             BufferSize,
             Buffer
             );

  return Status;
}

/**
  Erase data from NOR Flash.

  @param[in] This                  Instance to protocol
  @param[in] Lba                   Logical block to start erasing from
  @param[in] NumLba                Number of block to be erased
  @param[in] Hybrid                Use hybrid region

  @retval EFI_SUCCESS              Operation successful.
  @retval others                   Error occurred
**/
EFI_STATUS
NorFlashErase (
  IN NVIDIA_NOR_FLASH_PROTOCOL  *This,
  IN UINT32                     Lba,
  IN UINT32                     NumLba,
  IN BOOLEAN                    Hybrid
  )
{
  EFI_STATUS               Status;
  UINT32                   CmdSize;
  UINT32                   Count;
  UINT32                   Block;
  UINT32                   AddressShift;
  QSPI_TRANSACTION_PACKET  Packet;
  NOR_FLASH_PRIVATE_DATA   *Private;
  UINT32                   Offset;
  UINT32                   LastBlock;
  UINT64                   MemoryDensity;
  UINT32                   BlockSize;
  UINT8                    EraseCmd;

  if ((This == NULL) ||
      (NumLba == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Private = NOR_FLASH_PRIVATE_DATA_FROM_NOR_FLASH_PROTOCOL (This);

  if (Hybrid) {
    MemoryDensity = Private->PrivateFlashAttributes.HybridMemoryDensity;
    BlockSize     = Private->PrivateFlashAttributes.HybridBlockSize;
    EraseCmd      = Private->PrivateFlashAttributes.HybridEraseCmd;
    if ((MemoryDensity == 0) || (BlockSize == 0) || (EraseCmd == 0)) {
      return EFI_UNSUPPORTED;
    }
  } else {
    MemoryDensity = Private->PrivateFlashAttributes.FlashAttributes.MemoryDensity;
    BlockSize     = Private->PrivateFlashAttributes.FlashAttributes.BlockSize;
    EraseCmd      = Private->PrivateFlashAttributes.UniformEraseCmd;
  }

  LastBlock = (MemoryDensity / BlockSize) - 1;

  if ((Lba > LastBlock) ||
      ((Lba + NumLba - 1) > LastBlock))
  {
    return EFI_INVALID_PARAMETER;
  }

  // To uniform erase first block, must also hybrid erase the hybrid region
  if (!Hybrid &&
      (Lba == 0) &&
      (Private->PrivateFlashAttributes.HybridMemoryDensity > 0))
  {
    Status = NorFlashErase (
               This,
               0,
               Private->PrivateFlashAttributes.HybridMemoryDensity /
               Private->PrivateFlashAttributes.HybridBlockSize,
               TRUE
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: Failed hybrid erase: %r\n",
        __FUNCTION__,
        Status
        ));
      goto ErrorExit;
    }
  }

  CmdSize = NOR_CMD_SIZE + Private->PrivateFlashAttributes.AddrSize;
  ZeroMem (Private->CommandBuffer, CmdSize);

  for (Block = Lba; Block < (Lba + NumLba); Block++) {
    Status = ConfigureNorFlashWriteEnLatch (Private, TRUE);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Could not enable NOR flash WREN.\n", __FUNCTION__));
      goto ErrorExit;
    }

    AddressShift = 0;
    Offset       = Block * BlockSize;
    for (Count = (CmdSize - 1); Count > 0; Count--) {
      Private->CommandBuffer[Count] = (Offset & (0xFF << AddressShift)) >> AddressShift;
      AddressShift                 += 8;
    }

    Private->CommandBuffer[0] = EraseCmd;

    Packet.TxBuf      = Private->CommandBuffer;
    Packet.TxLen      = CmdSize;
    Packet.RxBuf      = NULL;
    Packet.RxLen      = 0;
    Packet.WaitCycles = 0;
    Packet.ChipSelect = Private->QspiChipSelect;
    Packet.Control    = QSPI_CONTROLLER_CONTROL_FAST_MODE;

    Status = Private->QspiController->PerformTransaction (Private->QspiController, &Packet);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Could not erase data from NOR flash.\n", __FUNCTION__));
      goto ErrorExit;
    }

    Status = WaitNorFlashWriteComplete (Private);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Could not complete NOR flash write.\n", __FUNCTION__));
      goto ErrorExit;
    }

    Status = ConfigureNorFlashWriteEnLatch (Private, FALSE);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Could not enable NOR flash WREN.\n", __FUNCTION__));
      goto ErrorExit;
    }
  }

  DEBUG ((DEBUG_INFO, "%a: Successfully erased data from NOR flash.\n", __FUNCTION__));

ErrorExit:

  return Status;
}

/**
  Erase data from NOR Flash using uniform erase.

  @param[in] This                  Instance to protocol
  @param[in] Lba                   Logical block to start erasing from
  @param[in] NumLba                Number of block to be erased

  @retval EFI_SUCCESS              Operation successful.
  @retval others                   Error occurred
**/
EFI_STATUS
EFIAPI
NorFlashUniformErase (
  IN NVIDIA_NOR_FLASH_PROTOCOL  *This,
  IN UINT32                     Lba,
  IN UINT32                     NumLba
  )
{
  return NorFlashErase (This, Lba, NumLba, FALSE);
}

/**
  Write single page data to NOR Flash.

  @param[in] This                  Instance to protocol
  @param[in] Offset                Offset to write to
  @param[in] Size                  Number of bytes to write
  @param[in] Buffer                Address to write data from

  @retval EFI_SUCCESS              Operation successful.
  @retval others                   Error occurred
**/
EFI_STATUS
NorFlashWriteSinglePage (
  IN NVIDIA_NOR_FLASH_PROTOCOL  *This,
  IN UINT32                     Offset,
  IN UINT32                     Size,
  IN VOID                       *Buffer
  )
{
  EFI_STATUS               Status;
  UINT32                   CmdSize;
  UINT32                   Count;
  UINT32                   AddressShift;
  QSPI_TRANSACTION_PACKET  Packet;
  NOR_FLASH_PRIVATE_DATA   *Private;
  UINT32                   FlashDensity;

  if ((This == NULL) ||
      (Buffer == NULL) ||
      (Size == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Private = NOR_FLASH_PRIVATE_DATA_FROM_NOR_FLASH_PROTOCOL (This);

  FlashDensity = Private->PrivateFlashAttributes.FlashAttributes.MemoryDensity;
  if ((Offset > (FlashDensity - 1)) ||
      ((Offset + Size) > (FlashDensity)))
  {
    return EFI_INVALID_PARAMETER;
  }

  CmdSize = NOR_CMD_SIZE + Private->PrivateFlashAttributes.AddrSize;
  ZeroMem (Private->CommandBuffer, CmdSize + Size);
  Status = ConfigureNorFlashWriteEnLatch (Private, TRUE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Could not enable NOR flash WREN.\n", __FUNCTION__));
    goto ErrorExit;
  }

  CopyMem (&Private->CommandBuffer[CmdSize], Buffer, Size);
  AddressShift = 0;
  for (Count = (CmdSize - 1); Count > 0; Count--) {
    Private->CommandBuffer[Count] = (Offset & (0xFF << AddressShift)) >> AddressShift;
    AddressShift                 += 8;
  }

  Private->CommandBuffer[0] = (Private->PrivateFlashAttributes.AddrSize == 4) ?
                              NOR_WRITE_DATA_CMD : NOR_WRITE_DATA_CMD_3B;

  Packet.TxBuf      = Private->CommandBuffer;
  Packet.TxLen      = CmdSize + Size;
  Packet.RxBuf      = NULL;
  Packet.RxLen      = 0;
  Packet.WaitCycles = 0;
  Packet.ChipSelect = Private->QspiChipSelect;
  Packet.Control    = QSPI_CONTROLLER_CONTROL_FAST_MODE;

  Status = Private->QspiController->PerformTransaction (Private->QspiController, &Packet);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Could not write data to NOR flash.\n", __FUNCTION__));
    goto ErrorExit;
  }

  Status = WaitNorFlashWriteComplete (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Could not complete NOR flash write.\n", __FUNCTION__));
    goto ErrorExit;
  }

  Status = ConfigureNorFlashWriteEnLatch (Private, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Could not disable NOR flash WREN.\n", __FUNCTION__));
    goto ErrorExit;
  }

  DEBUG ((DEBUG_INFO, "%a: Successfully wrote data to NOR flash.\n", __FUNCTION__));

ErrorExit:

  return Status;
}

/**
  Write data to NOR Flash.

  @param[in] This                  Instance to protocol
  @param[in] Offset                Offset to write to
  @param[in] Size                  Number of bytes to write
  @param[in] Buffer                Address to write data from

  @retval EFI_SUCCESS              Operation successful.
  @retval others                   Error occurred
**/
EFI_STATUS
EFIAPI
NorFlashWrite (
  IN NVIDIA_NOR_FLASH_PROTOCOL  *This,
  IN UINT32                     Offset,
  IN UINT32                     Size,
  IN VOID                       *Buffer
  )
{
  EFI_STATUS              Status;
  NOR_FLASH_PRIVATE_DATA  *Private;
  UINT32                  FlashDensity;
  UINT32                  PageSize;
  UINT32                  BytesToWrite;

  if ((This == NULL) ||
      (Buffer == NULL) ||
      (Size == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Private = NOR_FLASH_PRIVATE_DATA_FROM_NOR_FLASH_PROTOCOL (This);

  FlashDensity = Private->PrivateFlashAttributes.FlashAttributes.MemoryDensity;
  if ((Offset > (FlashDensity - 1)) ||
      ((Offset + Size) > (FlashDensity)))
  {
    return EFI_INVALID_PARAMETER;
  }

  // Writes need to be confined in a page.
  PageSize = Private->PrivateFlashAttributes.PageSize;
  while (Size > 0) {
    // Calculate offset and size within the page
    BytesToWrite = PageSize - (Offset & (PageSize - 1));
    if (BytesToWrite > Size) {
      BytesToWrite = Size;
    }

    Status = NorFlashWriteSinglePage (This, Offset, BytesToWrite, Buffer);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Could not write data to NOR flash.\n", __FUNCTION__));
      return Status;
    }

    Buffer  = (UINT8 *)Buffer + BytesToWrite;
    Offset += BytesToWrite;
    Size   -= BytesToWrite;
  }

  DEBUG ((DEBUG_INFO, "%a: Successfully wrote data to NOR flash.\n", __FUNCTION__));

  return Status;
}

/**
  Write data to NOR Flash.

  @param[in] This                  Instance to protocol
  @param[in] MediaId               Media ID for the device
  @param[in] Lba                   Logical block to start writing from
  @param[in] BufferSize            Number of bytes to be written
  @param[in] Buffer                Address to write data from

  @retval EFI_SUCCESS              Operation successful.
  @retval others                   Error occurred
**/
EFI_STATUS
EFIAPI
NorFlashWriteBlock (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  EFI_STATUS              Status;
  NOR_FLASH_PRIVATE_DATA  *Private;
  UINT32                  StartPage;
  UINT32                  NumPages;
  UINT32                  PageSize;
  UINT32                  BlockSize;
  UINT8                   *Data;

  if ((This == NULL) ||
      (Buffer == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Private = NOR_FLASH_PRIVATE_DATA_FROM_BLOCK_IO_PROTOCOL (This);

  if (MediaId != Private->FlashInstance) {
    return EFI_MEDIA_CHANGED;
  }

  Status = NorFlashErase (
             &Private->NorFlashProtocol,
             Lba,
             BufferSize / Private->PrivateFlashAttributes.FlashAttributes.BlockSize,
             FALSE
             );

  BlockSize = Private->PrivateFlashAttributes.FlashAttributes.BlockSize;
  PageSize  = Private->PrivateFlashAttributes.PageSize;
  StartPage = (BlockSize / PageSize) * Lba;
  NumPages  = BufferSize / PageSize;

  Data = Buffer;
  while (NumPages > 0) {
    Status = NorFlashWriteSinglePage (
               &Private->NorFlashProtocol,
               StartPage *  BufferSize / Private->PrivateFlashAttributes.PageSize,
               PageSize,
               Data
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    StartPage++;
    NumPages--;
    Data += PageSize;
  }

  return Status;
}
