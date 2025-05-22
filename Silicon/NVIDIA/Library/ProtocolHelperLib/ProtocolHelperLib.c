/** @file

  This driver implements supporting functions for working with UEFI protocols.

  SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Uefi/UefiBaseType.h"
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/DevicePath.h>

EFI_STATUS
EFIAPI
ProtocolHelperGetParentProtocolFromDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN EFI_GUID                  *ProtocolGuid,
  OUT VOID                     **Protocol
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  ParentHandle;

  // Get parent protocol from device path
  Status = gBS->LocateDevicePath (
                  ProtocolGuid,
                  &DevicePath,
                  &ParentHandle
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: Failed to locate parent protocol - %r\n", __FUNCTION__, Status));
    return Status;
  }

  Status = gBS->HandleProtocol (
                  ParentHandle,
                  ProtocolGuid,
                  (VOID **)Protocol
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: Failed to get parent protocol - %r\n", __FUNCTION__, Status));
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ProtocolHelperGetParentProtocolFromHandle (
  IN EFI_HANDLE  Handle,
  IN EFI_GUID    *ProtocolGuid,
  OUT VOID       **Protocol
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;

  // Get device path protocol
  Status = gBS->HandleProtocol (
                  Handle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&DevicePath
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: Failed to get Device Path Protocol - %r\n", __FUNCTION__, Status));
    return Status;
  }

  return ProtocolHelperGetParentProtocolFromDevicePath (DevicePath, ProtocolGuid, Protocol);
}
