/** @file
*  FDT Parser Library.
*  SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

/**
  Library to parse the FDT Boot payload and get the platform resource information.
  These library functions should only be called from the PrePi module OR the
  PlatformResourceLib Library.
**/

#include <Uefi.h>
#include "Uefi/UefiBaseType.h"
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DeviceTreeHelperLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TegraPlatformInfoLib.h>

#define MAX_CARVEOUT_STR  (45)

/**
  Get the tegra-boot-params node from the Boot FDT Blob.

  @param[in]  FdtBase      FDT base address.
  @param[out] NodeOffset   Boot Params node offset.

  @return EFI_SUCCESS if the tegra-boot-params is found.
          other       error looking up the node in the FDT.
  **/
STATIC
EFI_STATUS
EFIAPI
GetBootParamsNode (
  IN  UINTN  FdtBase,
  OUT INT32  *NodeOffset
  )
{
  EFI_STATUS  Status;
  UINTN       FdtSize;

  if ((NodeOffset == NULL) || (FdtBase == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid Parameters %p %p\n", __FUNCTION__, NodeOffset, FdtBase));
    return EFI_INVALID_PARAMETER;
  }

  FdtSize = FdtTotalSize ((CONST VOID *)FdtBase);
  SetDeviceTreePointer ((VOID *)FdtBase, FdtSize);

  *NodeOffset = 0;
  Status      = DeviceTreeGetNodeByPath ("/chosen/tegra-boot-params", NodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get tegra-boot-params node %r\n", __FUNCTION__, Status));
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Get the Reserved Memory Node from the Boot FDT Blob.

  @param[in]  FdtBase      FDT base address.
  @param[out] NodeOffset   reserved-memory node offset.

  @return EFI_SUCCESS if the reserved-memory node offset is found.
          other       error looking up the node in the FDT.
**/
STATIC
EFI_STATUS
EFIAPI
GetReservedMemoryNode (
  IN  UINTN  FdtBase,
  OUT INT32  *NodeOffset
  )
{
  EFI_STATUS  Status;
  UINTN       FdtSize;

  if ((NodeOffset == NULL) || (FdtBase == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid Parameters %p %p\n", __FUNCTION__, NodeOffset, FdtBase));
    return EFI_INVALID_PARAMETER;
  }

  FdtSize = FdtTotalSize ((CONST VOID *)FdtBase);
  SetDeviceTreePointer ((VOID *)FdtBase, FdtSize);

  *NodeOffset = 0;
  Status      = DeviceTreeGetNodeByPath ("/reserved-memory", NodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get reserved-memory node %r\n", __FUNCTION__, Status));
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Get a base address and size of a memory region for a socket index from the
  "reg" property of a node. The FDT boot blob organizes the memory regions
  as <base size> pairs in order of the socket index.

  @param[in]  NodeOffset The Device Tree node offset of the memory region.
  @param[in]  SocketIdx  The socket index.
  @param[out] Base       The base address of the memory region.
  @param[out] Size       The size of the memory region.
  @return EFI_SUCCESS if the memory region address and size is found, otherwise return error code.
**/
STATIC
EFI_STATUS
GetSocketAddrSize (
  IN INT32                  NodeOffset,
  IN UINTN                  SocketIdx,
  OUT EFI_PHYSICAL_ADDRESS  *Base,
  OUT UINT64                *Size
  )
{
  EFI_STATUS                        Status;
  NVIDIA_DEVICE_TREE_REGISTER_DATA  *RegisterArray;
  UINT32                            RegisterCount;

  if ((Base == NULL) || (Size == NULL) || (NodeOffset == -1)) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid Parameters %p %p %d\n", __FUNCTION__, Base, Size, NodeOffset));
    return EFI_INVALID_PARAMETER;
  }

  RegisterArray = NULL;
  RegisterCount = 0;

  Status = DeviceTreeGetRegisters (NodeOffset, NULL, &RegisterCount);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get registers %r\n", __FUNCTION__, Status));
    return Status;
  }

  RegisterArray = AllocateZeroPool (RegisterCount * sizeof (NVIDIA_DEVICE_TREE_REGISTER_DATA));
  if (RegisterArray == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to allocate memory\n", __FUNCTION__));
    return EFI_OUT_OF_RESOURCES;
  }

  Status = DeviceTreeGetRegisters (NodeOffset, RegisterArray, &RegisterCount);
  if (Status != EFI_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get registers\n", __FUNCTION__));
    goto ExitGetSocketAddrSize;
  }

  if (SocketIdx >= RegisterCount) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid Socket Idx %u\n", __FUNCTION__, SocketIdx));
    Status = EFI_INVALID_PARAMETER;
    goto ExitGetSocketAddrSize;
  }

  *Base = RegisterArray[SocketIdx].BaseAddress;
  *Size = RegisterArray[SocketIdx].Size;
ExitGetSocketAddrSize:
  FreePool (RegisterArray);
  RegisterArray = NULL;
  RegisterCount = 0;

  return Status;
}

/**
  Get the socket mask from the Boot FDT Blob.

  @param[in]  FdtBase      FDT base address.
  @param[out] SocketMask The socket mask.

  @retval EFI_SUCCESS if the socket mask is found.
          other       FDT lookup failed.
  **/
EFI_STATUS
EFIAPI
DeviceTreeGetSocketMask (
  IN  UINTN   FdtBase,
  OUT UINT32  *SocketMask
  )
{
  EFI_STATUS  Status;
  INT32       NodeOffset;

  if ((SocketMask == NULL) || (FdtBase == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid Parameters %p %p\n", __FUNCTION__, SocketMask, FdtBase));
    return EFI_INVALID_PARAMETER;
  }

  Status = GetBootParamsNode (FdtBase, &NodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get tegra-boot-params node %r\n", __FUNCTION__, Status));
    return Status;
  }

  Status = DeviceTreeGetNodePropertyValue32 (NodeOffset, "socket_mask", SocketMask);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get socket_mask property %r\n", __FUNCTION__, Status));
    return Status;
  }

  return EFI_SUCCESS;
}

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
  )
{
  EFI_STATUS  Status;
  INT32       NodeOffset;

  if ((SmtEnabled == NULL) || (FdtBase == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid Parameters %p %p\n", __FUNCTION__, SmtEnabled, FdtBase));
    return EFI_INVALID_PARAMETER;
  }

  Status = GetBootParamsNode (FdtBase, &NodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get tegra-boot-params node %r\n", __FUNCTION__, Status));
    return Status;
  }

  Status = DeviceTreeGetNodePropertyValue32 (NodeOffset, "is_smt2_enabled", SmtEnabled);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to get tegra-boot-params/is_smt2_enabled property %r\n", Status));
    return Status;
  }

  return EFI_SUCCESS;
}

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
  )
{
  EFI_STATUS  Status;
  INT32       MemoryNodeOffset;
  UINTN       FdtSize;

  // Don't check the SocketIdx as we will validate it after getting the address and size.
  if ((Base == NULL) || (Size == NULL) || (FdtBase == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid Parameters %p %p %p\n", __FUNCTION__, Base, Size, FdtBase));
    return EFI_INVALID_PARAMETER;
  }

  FdtSize = FdtTotalSize ((CONST VOID *)FdtBase);
  SetDeviceTreePointer ((VOID *)FdtBase, FdtSize);

  MemoryNodeOffset = -1;
  // Get the first memory node, there should be only one.
  Status = DeviceTreeGetNextMemoryNode (&MemoryNodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get memory node\n", __FUNCTION__));
    return Status;
  }

  Status = GetSocketAddrSize (MemoryNodeOffset, SocketIdx, Base, Size);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get socket address and size %r\n", __FUNCTION__, Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "Found DRAM Region: 0x%lx 0x%lx\n", *Base, *Size));

  return EFI_SUCCESS;
}

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
  )
{
  EFI_STATUS   Status;
  INT32        NodeOffset;
  INT32        SubNodeOffset;
  CHAR8        MatchNode[MAX_CARVEOUT_STR];
  CONST CHAR8  *MatchNodeList[] = { MatchNode, NULL };

  if ((Base == NULL) || (Size == NULL) || (FdtBase == 0) || (CarveoutName == NULL)) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid Parameters %p %p %p %p\n", __FUNCTION__, Base, Size, FdtBase, CarveoutName));
    return EFI_INVALID_PARAMETER;
  }

  Status = GetReservedMemoryNode (FdtBase, &NodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get reserved-memory node %r\n", __FUNCTION__, Status));
    return Status;
  }

  *Base         = 0;
  *Size         = 0;
  SubNodeOffset = 0;

  AsciiSPrint (MatchNode, sizeof (MatchNode), "nvidia,carveout_%a", CarveoutName);
  Status = DeviceTreeGetNextCompatibleSubnode (MatchNodeList, NodeOffset, &SubNodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get %a node %r\n", __FUNCTION__, MatchNode, Status));
    return Status;
  }

  Status = GetSocketAddrSize (SubNodeOffset, SocketIdx, Base, Size);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get socket address and size %r\n", __FUNCTION__, Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "%a: Found Region: %a Base: 0x%lx Size: 0x%lx\n", __FUNCTION__, MatchNode, *Base, *Size));

  return Status;
}

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
  )
{
  EFI_STATUS  Status;
  INT32       NodeOffset;
  INT32       SubNodeOffset;
  CHAR8       MatchNode[MAX_CARVEOUT_STR];

  // Not checking the CarveoutIdx as its max value is per SOC.
  if ((Base == NULL) || (Size == NULL) || (FdtBase == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid Parameters %p %p %p\n", __FUNCTION__, Base, Size, FdtBase));
    return EFI_INVALID_PARAMETER;
  }

  Status = GetReservedMemoryNode (FdtBase, &NodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get reserved-memory node %r\n", __FUNCTION__, Status));
    return Status;
  }

  *Base         = 0;
  *Size         = 0;
  SubNodeOffset = 0;

  AsciiSPrint (MatchNode, sizeof (MatchNode), "carveout@%u", CarveoutIdx);
  Status = DeviceTreeGetNamedSubnode (MatchNode, NodeOffset, &SubNodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get %a node %r\n", __FUNCTION__, MatchNode, Status));
    return Status;
  }

  Status = GetSocketAddrSize (SubNodeOffset, SocketIdx, Base, Size);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get socket address and size %r\n", __FUNCTION__, Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "%a: Found Region: %a Base: 0x%lx Size: 0x%lx\n", __FUNCTION__, MatchNode, *Base, *Size));

  return Status;
}

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
  )
{
  EFI_STATUS  Status;
  INT32       NodeOffset;
  INT32       SubNodeOffset;

  if ((Base == NULL) || (Size == NULL) || (FdtBase == 0) || (NodeName == NULL)) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid Parameters %p %p %p %p\n", __FUNCTION__, Base, Size, FdtBase, NodeName));
    return EFI_INVALID_PARAMETER;
  }

  Status = GetBootParamsNode (FdtBase, &NodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get tegra-boot-params node %r\n", __FUNCTION__, Status));
    return Status;
  }

  Status = DeviceTreeGetNamedSubnode (NodeName, NodeOffset, &SubNodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get %a node %r\n", __FUNCTION__, NodeName, Status));
    return Status;
  }

  Status = GetSocketAddrSize (SubNodeOffset, SocketIdx, Base, Size);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get socket address and size %r\n", __FUNCTION__, Status));
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Verify the FDT is valid.

  @param[in] Base The base address of the FDT.

  @return EFI_SUCCESS             if the FDT is valid,
          EFI_INVALID_PARAMETER   if the FDT is invalid (fails size/header check).
          other return error code if the FDT can't be parsed.

  **/
EFI_STATUS
EFIAPI
DeviceTreeCheckFdtBoot (
  IN UINTN  Base
  )
{
  INT32  Header;
  UINTN  UplSize;

  if (Base == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Header = FdtCheckHeader ((CONST VOID *)Base);
  if (Header != 0) {
    return EFI_INVALID_PARAMETER;
  }

  UplSize = FdtTotalSize ((CONST VOID *)Base);
  if (UplSize > FDT_BLOB_MAX_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}
