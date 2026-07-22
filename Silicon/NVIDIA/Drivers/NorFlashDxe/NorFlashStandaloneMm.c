/** @file

  NOR Flash Standalone MM Driver

  SPDX-FileCopyrightText: Copyright (c) 2018-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiMm.h>
#include <Library/MmServicesTableLib.h>
#include <NorFlashPrivate.h>
#include <Library/IoLib.h>
#include <Library/StandaloneMmOpteeDeviceMem.h>

EFI_BLOCK_IO_MEDIA  Media = {
  0,         // Media ID gets updated during Start
  FALSE,     // Non removable media
  TRUE,      // Media currently present
  0,         // First logical block
  FALSE,     // Not read only
  FALSE,     // Does not cache write data
  SIZE_64KB, // Block size gets updated during start
  4,         // Alignment required
  0          // Last logical block gets updated during start
};

VENDOR_DEVICE_PATH  VendorDevicePath = {
  {
    HARDWARE_DEVICE_PATH,
    HW_VENDOR_DP,
    {
      (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
      (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
    }
  },
  { 0x8332de7f, 0x50c3, 0x47ca, { 0x82, 0x4e, 0x83, 0x3a, 0xac, 0x7c, 0xf1, 0x6d }
  }
};

/**
  Starts a device controller or a bus controller.

  @param[in]  ImageHandle          The firmware allocated handle for the EFI image.
  @param[in]  MmSystemTable        A pointer to the EFI MM System Table.

  @retval EFI_SUCCESS              The device was started.
  @retval EFI_OUT_OF_RESOURCES     The request could not be completed due to a lack of resources.
  @retval Others                   The driver failed to start the device.
**/
EFI_STATUS
EFIAPI
NorFlashInitialise (
  IN EFI_HANDLE           ImageHandle,
  IN EFI_MM_SYSTEM_TABLE  *MmSystemTable
  )
{
  EFI_STATUS                       Status;
  NOR_FLASH_PRIVATE_DATA           *Private = NULL;
  NVIDIA_QSPI_CONTROLLER_PROTOCOL  *QspiProtocol;
  UINTN                            Index;
  UINTN                            NumHandles;
  EFI_HANDLE                       *HandleBuffer = NULL;
  UINT32                           *QspiSocket;
  UINT32                           *Socket;

  Status = GetProtocolHandleBuffer (
             &gNVIDIAQspiControllerProtocolGuid,
             &NumHandles,
             &HandleBuffer
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Failed to get QSPI protocol handles (%r)\r\n",
      __FUNCTION__,
      Status
      ));
    return EFI_SUCCESS;
  }

  for (Index = 0; Index < NumHandles; Index++) {
    // Allocate Private Data
    QspiProtocol = NULL;
    Status       = gMmst->MmHandleProtocol (
                            HandleBuffer[Index],
                            &gNVIDIAQspiControllerProtocolGuid,
                            (VOID **)&QspiProtocol
                            );
    if (EFI_ERROR (Status)) {
      Status = EFI_NOT_FOUND;
      break;
    }

    Private = AllocateRuntimeZeroPool (sizeof (NOR_FLASH_PRIVATE_DATA));
    if (Private == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto ErrorExit;
    }

    Private->Signature      = NOR_FLASH_SIGNATURE;
    Private->QspiController = QspiProtocol;

    Status = GetVarStoreCs (&Private->QspiChipSelect);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Unknown chip select: %r\n", __FUNCTION__, Status));
      goto ErrorExit;
    }

    DEBUG ((
      DEBUG_ERROR,
      "%a: Using ChipSelect %u\n",
      __FUNCTION__,
      Private->QspiChipSelect
      ));

    if (PcdGetBool (PcdSecureQspiUseFastRead) == TRUE) {
      Private->PrivateFlashAttributes.FastReadSupport = TRUE;
    } else {
      Private->PrivateFlashAttributes.FastReadSupport = FALSE;
    }

    // Read NOR flash's SFDP
    Status = ReadNorFlashSFDP (Private);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: SFDP Read Failed\n", __FUNCTION__));
      FreePool (Private);
      // Continue to the next socket.
      Status  = EFI_SUCCESS;
      Private = NULL;
      continue;
    }

    DEBUG ((
      DEBUG_INFO,
      "%a: NOR Flash Uniform Memory Density: 0x%lx\n",
      __FUNCTION__,
      Private->PrivateFlashAttributes.FlashAttributes.MemoryDensity
      ));
    DEBUG ((
      DEBUG_INFO,
      "%a: NOR Flash Uniform Block Size: 0x%lx\n",
      __FUNCTION__,
      Private->PrivateFlashAttributes.FlashAttributes.BlockSize
      ));
    DEBUG ((
      DEBUG_INFO,
      "%a: NOR Flash Hybrid Memory Density: 0x%lx\n",
      __FUNCTION__,
      Private->PrivateFlashAttributes.HybridMemoryDensity
      ));
    DEBUG ((
      DEBUG_INFO,
      "%a: NOR Flash Hybrid Block Size: 0x%lx\n",
      __FUNCTION__,
      Private->PrivateFlashAttributes.HybridBlockSize
      ));
    DEBUG ((
      DEBUG_INFO,
      "%a: NOR Flash Write Page Size: 0x%lx\n",
      __FUNCTION__,
      Private->PrivateFlashAttributes.PageSize
      ));

    // Allocate Command Buffer
    Private->CommandBuffer = AllocateRuntimeZeroPool (
                               NOR_CMD_SIZE + NOR_ADDR_SIZE +
                               Private->PrivateFlashAttributes.PageSize
                               );
    if (Private->CommandBuffer == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto ErrorExit;
    }

    // Install Protocols
    Private->NorFlashProtocol.FvbAttributes = EFI_FVB2_READ_ENABLED_CAP |
                                              EFI_FVB2_READ_STATUS |
                                              EFI_FVB2_STICKY_WRITE |
                                              EFI_FVB2_ERASE_POLARITY |
                                              EFI_FVB2_WRITE_STATUS |
                                              EFI_FVB2_WRITE_ENABLED_CAP;
    Private->NorFlashProtocol.GetAttributes = NorFlashGetAttributes;
    Private->NorFlashProtocol.Read          = NorFlashRead;
    Private->NorFlashProtocol.Write         = NorFlashWrite;
    Private->NorFlashProtocol.Erase         = NorFlashUniformErase;

    Status = gMmst->MmInstallProtocolInterface (
                      &Private->NorFlashHandle,
                      &gNVIDIANorFlashProtocolGuid,
                      EFI_NATIVE_INTERFACE,
                      &Private->NorFlashProtocol
                      );

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Failed to install NOR flash protocols\n", __FUNCTION__));
      goto ErrorExit;
    }

    Private->ProtocolsInstalled = TRUE;

    QspiSocket = NULL;
    Status     = gMmst->MmHandleProtocol (
                          HandleBuffer[Index],
                          &gNVIDIASocketIdProtocolGuid,
                          (VOID **)&QspiSocket
                          );
    if (EFI_ERROR (Status)) {
      Status = EFI_NOT_FOUND;
      break;
    }

    Socket = AllocateRuntimeZeroPool (sizeof (UINT32));
    if (Socket == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    *Socket = *QspiSocket;
    Status  = gMmst->MmInstallProtocolInterface (
                       &Private->NorFlashHandle,
                       &gNVIDIASocketIdProtocolGuid,
                       EFI_NATIVE_INTERFACE,
                       Socket
                       );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Failed to install SocketIdProtocol %r \n", __FUNCTION__, Status));
      goto ErrorExit;
    }
  }

ErrorExit:
  if (EFI_ERROR (Status)) {
    if (Private != NULL) {
      if (Private->NorFlashDevicePath != NULL) {
        FreePool (Private->NorFlashDevicePath);
      }

      if (Private->CommandBuffer != NULL) {
        FreePool (Private->CommandBuffer);
      }

      FreePool (Private);
    }
  }

  if (HandleBuffer !=  NULL) {
    FreePool (HandleBuffer);
  }

  return Status;
}
