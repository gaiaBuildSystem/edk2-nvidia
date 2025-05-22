/** @file

  SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef __PROTOCOL_HELPER_LIB_H__
#define __PROTOCOL_HELPER_LIB_H__

#include <Uefi.h>
#include <Protocol/DevicePath.h>

/**
  Search backwards through the device path for a protocol.

  @param[in]  DevicePath    The device path to search back through.
  @param[in]  ProtocolGuid  The protocol guid to search for.
  @param[out] Protocol      The protocol found.

  @retval EFI_SUCCESS       The protocol was found.
  @retval EFI_NOT_FOUND     The protocol was not found.
  @retval Others            Other errors.

**/
EFI_STATUS
EFIAPI
ProtocolHelperGetParentProtocolFromDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN EFI_GUID                  *ProtocolGuid,
  OUT VOID                     **Protocol
  );

/**
  Search backwards through the device path of the handle for a protocol.

  @param[in]  Handle        The handle whose device path to search back through.
  @param[in]  ProtocolGuid  The protocol guid to search for.
  @param[out] Protocol      The protocol found.

  @retval EFI_SUCCESS       The protocol was found.
  @retval EFI_NOT_FOUND     The protocol was not found or handle does not have a device path.
  @retval Others            Other errors.

**/
EFI_STATUS
EFIAPI
ProtocolHelperGetParentProtocolFromHandle (
  IN EFI_HANDLE  Handle,
  IN EFI_GUID    *ProtocolGuid,
  OUT VOID       **Protocol
  );

#endif //__PROTOCOL_HELPER_LIB_H__
