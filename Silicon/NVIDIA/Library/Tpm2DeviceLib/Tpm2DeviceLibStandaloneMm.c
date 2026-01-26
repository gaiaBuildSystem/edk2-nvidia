/** @file
  TPM2 Device Library for Standalone MM

  SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Protocol/Tcg2Protocol.h>
#include <IndustryStandard/Tpm20.h>
#include <Library/MmServicesTableLib.h>

#include "Tpm2DeviceLibInternal.h"

STATIC NVIDIA_TPM2_PROTOCOL  *mTpm2 = NULL;

/**
  This service enables the sending of commands to the TPM2.

  @param[in]      InputParameterBlockSize  Size of the TPM2 input parameter block.
  @param[in]      InputParameterBlock      Pointer to the TPM2 input parameter block.
  @param[in,out]  OutputParameterBlockSize Size of the TPM2 output parameter block.
  @param[in]      OutputParameterBlock     Pointer to the TPM2 output parameter block.

  @retval EFI_SUCCESS            The command byte stream was successfully sent to the device and a response was successfully received.
  @retval EFI_DEVICE_ERROR       The command was not successfully sent to the device or a response was not successfully received from the device.
  @retval EFI_BUFFER_TOO_SMALL   The output parameter block is too small.
**/
EFI_STATUS
EFIAPI
Tpm2SubmitCommand (
  IN UINT32      InputParameterBlockSize,
  IN UINT8       *InputParameterBlock,
  IN OUT UINT32  *OutputParameterBlockSize,
  IN UINT8       *OutputParameterBlock
  )
{
  if (mTpm2 == NULL) {
    return EFI_DEVICE_ERROR;
  }

  if ((InputParameterBlock == NULL) || (OutputParameterBlock == NULL) || (OutputParameterBlockSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  return TisTpmCommand (
           mTpm2,
           InputParameterBlock,
           InputParameterBlockSize,
           OutputParameterBlock,
           OutputParameterBlockSize
           );
}

/**
  This service requests use TPM2.

  @retval EFI_SUCCESS      Get the control of TPM2 chip.
  @retval EFI_NOT_FOUND    TPM2 not found.
  @retval EFI_DEVICE_ERROR Unexpected device behavior.
**/
EFI_STATUS
EFIAPI
Tpm2RequestUseTpm (
  VOID
  )
{
  if (mTpm2 == NULL) {
    return EFI_DEVICE_ERROR;
  }

  return TisRequestUseTpm (mTpm2);
}

/**
  This service register TPM2 device.

  @param Tpm2Device  TPM2 device

  @retval EFI_SUCCESS          This TPM2 device is registered successfully.
  @retval EFI_UNSUPPORTED      System does not support register this TPM2 device.
  @retval EFI_ALREADY_STARTED  System already register this TPM2 device.
**/
EFI_STATUS
EFIAPI
Tpm2RegisterTpm2DeviceLib (
  IN TPM2_DEVICE_INTERFACE  *Tpm2Device
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Constructor for TPM2 device library in Standalone MM.

  @param[in]  ImageHandle   Image Handle
  @param[in]  MmSystemTable MM System Table

  @retval EFI_SUCCESS           The operation completed successfully.
**/
EFI_STATUS
EFIAPI
Tpm2DeviceLibStandaloneMmConstructor (
  IN EFI_HANDLE           ImageHandle,
  IN EFI_MM_SYSTEM_TABLE  *MmSystemTable
  )
{
  EFI_STATUS            Status;
  NVIDIA_TPM2_PROTOCOL  *Tpm2;

  //
  // Locate TPM2 Protocol
  //
  Status = gMmst->MmLocateProtocol (
                    &gNVIDIATpm2ProtocolGuid,
                    NULL,
                    (VOID **)&Tpm2
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: TPM2 protocol not found: %r\n", __FUNCTION__, Status));
  } else {
    mTpm2 = Tpm2;
  }

  return EFI_SUCCESS;
}
