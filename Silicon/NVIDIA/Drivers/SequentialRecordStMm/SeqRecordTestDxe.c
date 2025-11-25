/** @file

  SPDX-FileCopyrightText: Copyright (c) 2021-2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/MmCommunication2.h>
#include <Library/BaseLib.h>
#include <Guid/NVIDIAMmMb1Record.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>

STATIC  EFI_MM_COMMUNICATION2_PROTOCOL  *mMmCommProtocol = NULL;
#define  TEST_COMMAND_SIZE  (sizeof (EFI_MM_COMMUNICATE_HEADER) + sizeof (NVIDIA_MM_MB1_RECORD_PAYLOAD) - 1)
STATIC  NVIDIA_MM_MB1_RECORD_PAYLOAD  TestPayload;

STATIC
EFI_STATUS
SendSeqRecordTestCommand (
  IN UINT8  *TestCommandBuffer,
  IN UINT8  Command
  )
{
  EFI_STATUS                    Status;
  EFI_MM_COMMUNICATE_HEADER     *Header;
  NVIDIA_MM_MB1_RECORD_PAYLOAD  *Payload;
  UINTN                         MmBufferSize;

  ZeroMem (&TestPayload, sizeof (NVIDIA_MM_MB1_RECORD_PAYLOAD));

  MmBufferSize = TEST_COMMAND_SIZE;
  Header       = (EFI_MM_COMMUNICATE_HEADER *)TestCommandBuffer;
  CopyGuid (&Header->HeaderGuid, &gNVIDIAMmMb1RecordGuid);
  Header->MessageLength = sizeof (NVIDIA_MM_MB1_RECORD_PAYLOAD);
  Payload               = (NVIDIA_MM_MB1_RECORD_PAYLOAD *)&Header->Data;
  Payload->Command      = Command;

  Status = mMmCommProtocol->Communicate (
                              mMmCommProtocol,
                              TestCommandBuffer,
                              TestCommandBuffer,
                              &MmBufferSize
                              );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to send test command %r\r\n", __FUNCTION__, Status));
    return Status;
  }

  DEBUG ((DEBUG_ERROR, "%a: Test command sent successfully\r\n", __FUNCTION__));
  DEBUG ((DEBUG_ERROR, "%a: Test command status: %r\r\n", __FUNCTION__, Payload->Status));
  if (!EFI_ERROR (Payload->Status) && (Command == NVIDIA_MM_MB1_RECORD_READ_CMD)) {
    CopyMem (&TestPayload, Payload, sizeof (NVIDIA_MM_MB1_RECORD_PAYLOAD));
    DEBUG ((DEBUG_ERROR, "\r\n"));
    DEBUG ((
      DEBUG_ERROR,
      "%a: Test command command: %u Status: %r\r\n",
      __FUNCTION__,
      TestPayload.Command,
      TestPayload.Status
      ));
    for (int i = 0; i < sizeof (TestPayload.Data); i++) {
      DEBUG ((DEBUG_ERROR, "%02X ", TestPayload.Data[i]));
    }

    DEBUG ((DEBUG_ERROR, "\r\n"));
  }

  return Status;
}

/**
 * Entry point of the driver.
 *
 * @param ImageHandle     The image handle.
 * @param SystemTable     The system table.
**/
EFI_STATUS
EFIAPI
SeqRecordTestDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT8       *TestCommandBuffer;

  TestCommandBuffer = AllocateZeroPool (TEST_COMMAND_SIZE);
  if (TestCommandBuffer == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to allocate test command buffer\r\n", __FUNCTION__));
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->LocateProtocol (&gEfiMmCommunication2ProtocolGuid, NULL, (VOID **)&mMmCommProtocol);
  ASSERT_EFI_ERROR (Status);

  Status = SendSeqRecordTestCommand (TestCommandBuffer, NVIDIA_MM_MB1_RECORD_READ_CMD);
  ASSERT_EFI_ERROR (Status);

  SetMem (TestCommandBuffer, TEST_COMMAND_SIZE, 0xAB);
  Status = SendSeqRecordTestCommand (TestCommandBuffer, NVIDIA_MM_MB1_RECORD_WRITE_CMD);
  ASSERT_EFI_ERROR (Status);

  ZeroMem (TestCommandBuffer, TEST_COMMAND_SIZE);
  Status = SendSeqRecordTestCommand (TestCommandBuffer, NVIDIA_MM_MB1_RECORD_READ_CMD);
  ASSERT_EFI_ERROR (Status);

  Status = SendSeqRecordTestCommand (TestCommandBuffer, NVIDIA_MM_MB1_ERASE_PARTITION);
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
