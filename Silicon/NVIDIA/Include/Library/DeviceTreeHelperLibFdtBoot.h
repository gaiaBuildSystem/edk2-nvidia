/** @file
*
*  SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

#ifndef __DEVICE_TREE_HELPER_LIB_FDT_BOOT_H__
#define __DEVICE_TREE_HELPER_LIB_FDT_BOOT_H__

#include <Uefi/UefiBaseType.h>

/**
  Get the socket mask from the Boot FDT Blob.

  @param[in]  FdtBase    Boot FDT base address.
  @param[out] SocketMask The socket mask.

  @retval EFI_SUCCESS if the socket mask is found.
          other       FDT lookup failed.
  **/
EFI_STATUS
EFIAPI
DeviceTreeGetSocketMask (
  IN  UINTN   FdtBase,
  OUT UINT32  *SocketMask
  );

/**
  Get the SMT enabled property from the Boot FDT Blob.

  @param[in]  FdtBase      FDT base address.
  @param[out] SmtEnabled   SmtEnabled.

  @return EFI_SUCCESS if the SMT enabled property is found.
          other       FDT lookup failed.
  **/
EFI_STATUS
EFIAPI
DeviceTreeGetSmtEnabled (
  IN  UINTN   FdtBase,
  OUT UINT32  *SmtEnabled
  );

/**
  Get the carveout region for the given socket given the carveout name.

  @param[in]  FdtBase      FDT base address.
  @param[in]  CarveoutName The carveout name to match.
  @param[in]  SocketIdx    The socket index.
  @param[out] Base         The base address of the carveout region.
  @param[out] Size         The size of the carveout region.

  @return EFI_SUCCESS if the carveout region is found.
          other       FDT lookup failed.
  **/
EFI_STATUS
EFIAPI
DeviceTreeGetSocketCarveoutRegionByName (
  IN  UINTN                 FdtBase,
  IN  CONST CHAR8           *CarveoutName,
  IN  UINTN                 SocketIdx,
  OUT EFI_PHYSICAL_ADDRESS  *Base,
  OUT UINT64                *Size
  );

/**
  Get the carveout region for the given socket given the carveout index.

  @param[in]  FdtBase      FDT base address.
  @param[in]  CarveoutIdx  The carveout index to match.
  @param[in]  SocketIdx    The socket index.
  @param[out] Base         The base address of the carveout region.
  @param[out] Size         The size of the carveout region.

  @return EFI_SUCCESS if the carveout region is found.
          other       FDT lookup failed.
  **/
EFI_STATUS
EFIAPI
DeviceTreeGetSocketCarveoutRegionByIndex (
  IN  UINTN                 FdtBase,
  IN  UINTN                 CarveoutIdx,
  IN  UINTN                 SocketIdx,
  OUT EFI_PHYSICAL_ADDRESS  *Base,
  OUT UINT64                *Size
  );

/**
  Get the DRAM region for the given socket index.

  @param[in]  FdtBase      FDT base address.
  @param[in]  SocketIdx    The socket index.
  @param[out] Base         The base address of the DRAM region.
  @param[out] Size         The size of the DRAM region.

  @return EFI_SUCCESS if the DRAM region is found.
          other       FDT lookup failed.
  **/
EFI_STATUS
EFIAPI
DeviceTreeGetSocketDramRegion (
  IN  UINTN                 FdtBase,
  IN  UINTN                 SocketIdx,
  OUT EFI_PHYSICAL_ADDRESS  *Base,
  OUT UINT64                *Size
  );

/**
  This function is used to get the memory base and size from the reg property
  of a subnode of the boot params node.

  @param[in]  FdtBase      FDT base address.
  @param[in]  SocketIdx    The socket index.
  @param[in]  NodeName     The name of the subnode to get the reg property from.
  @param[out] Base         The base address of the memory region.
  @param[out] Size         The size of the memory region.

  @return EFI_SUCCESS if the memory region is found.
          other       FDT lookup failed.
  **/
EFI_STATUS
EFIAPI
DeviceTreeGetBootParamsSubnodePhysAddrSize (
  IN  UINTN                 FdtBase,
  IN  UINTN                 SocketIdx,
  IN  CONST CHAR8           *NodeName,
  OUT EFI_PHYSICAL_ADDRESS  *Base,
  OUT UINT64                *Size
  );

/**
  Verify the FDT is valid.

  @param[in] Base The base address of the FDT.

  @return EFI_SUCCESS             if the FDT is valid.
          EFI_INVALID_PARAMETER   if the FDT is invalid (fails size/header check).
          other return error code if the FDT can't be parsed.
**/
EFI_STATUS
EFIAPI
DeviceTreeCheckFdtBoot (
  IN UINTN  Base
  );

#endif // __DEVICE_TREE_HELPER_LIB_FDT_BOOT_H__
