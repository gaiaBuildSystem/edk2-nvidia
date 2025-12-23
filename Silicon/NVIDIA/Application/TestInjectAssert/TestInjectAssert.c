/** @file
  TestInjectAssert

  SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/ShellLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/MmCommunication2.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PlatformResourceLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/UefiHiiServicesLib.h>
#include <Library/HiiLib.h>

//
// Used for ShellCommandLineParseEx only
// and to ensure user inputs are in valid format
//
SHELL_PARAM_ITEM  TestInjectAssertParamList[] = {
  { L"--swassert",    TypeFlag },
  { L"--exception",   TypeFlag },
  { L"--swassert_mm", TypeFlag },
  { L"-?",            TypeFlag },
  { NULL,             TypeMax  },
};

#define MM_TEST_PAYLOAD_SIZE  (64 + sizeof (EFI_MM_COMMUNICATE_HEADER))

STATIC CHAR16          AppName[] = L"TestInjectAssert";
STATIC EFI_HII_HANDLE  HiiHandle;
STATIC UINT8           MmTestPayload[MM_TEST_PAYLOAD_SIZE];

/**
  This function is used to inject an assert in MM by sending a message
  to an unregistered MMI.

  @param[in] ImageHandle    The image handle of this application.
  @param[in] SystemTable    The pointer to the EFI System Table.

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
InjectAssertInMM (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                      Status;
  EFI_MM_COMMUNICATION2_PROTOCOL  *MmCommProtocol;
  EFI_MM_COMMUNICATE_HEADER       *Header;
  UINTN                           DataSize;

  Status = gBS->LocateProtocol (&gEfiMmCommunication2ProtocolGuid, NULL, (VOID **)&MmCommProtocol);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"%a: locate Mm communication 2 protocol failed: %r\n", __FUNCTION__, Status);
    return Status;
  }

  Header = (EFI_MM_COMMUNICATE_HEADER *)MmTestPayload;
  CopyGuid (&Header->HeaderGuid, &gNVIDIATestAssertMM);
  Header->MessageLength = 64;
  DataSize              = MM_TEST_PAYLOAD_SIZE;

  Status = MmCommProtocol->Communicate (
                             MmCommProtocol,
                             MmTestPayload,
                             MmTestPayload,
                             &DataSize
                             );
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"%a: MM communicate failed: %r\n", __FUNCTION__, Status);
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  This is the declaration of an EFI image entry point. This entry point is
  the same for UEFI Applications, UEFI OS Loaders, and UEFI Drivers, including
  both device drivers and bus drivers.

  The entry point for StackCheck application that should casue an abort due to stack overwrite.

  @param[in] ImageHandle    The image handle of this application.
  @param[in] SystemTable    The pointer to the EFI System Table.

  @retval EFI_SUCCESS    The operation completed successfully.

**/
EFI_STATUS
EFIAPI
TestInjectAssert (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  LIST_ENTRY                   *ParamPackage;
  EFI_HII_PACKAGE_LIST_HEADER  *PackageList;
  CHAR16                       *ProblemParam;
  BOOLEAN                      SwAssertInject;
  BOOLEAN                      ExceptionInject;
  BOOLEAN                      SwAssertInjectMM;
  EFI_STATUS                   Status;
  UINT8                        *TestPtr;

  SwAssertInject  = FALSE;
  ExceptionInject = FALSE;
  TestPtr         = NULL;

  //
  // Retrieve HII package list from ImageHandle
  //
  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiHiiPackageListProtocolGuid,
                  (VOID **)&PackageList,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Publish HII package list to HII Database.
  //
  Status = gHiiDatabase->NewPackageList (
                           gHiiDatabase,
                           PackageList,
                           NULL,
                           &HiiHandle
                           );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ASSERT (HiiHandle != NULL);

  Status = ShellCommandLineParseEx (TestInjectAssertParamList, &ParamPackage, &ProblemParam, TRUE, FALSE);
  if (EFI_ERROR (Status)) {
    ShellPrintHiiEx (-1, -1, NULL, STRING_TOKEN (STR_TEST_INJECT_ASSERT_UNKNOWN), HiiHandle, ProblemParam);
    goto TestInjectAssertDone;
  }

  if (ShellCommandLineGetFlag (ParamPackage, L"-?")) {
    ShellPrintHiiEx (-1, -1, NULL, STRING_TOKEN (STR_TEST_INJECT_ASSERT_HELP), HiiHandle, AppName);
    goto TestInjectAssertDone;
  }

  if (ShellCommandLineGetFlag (ParamPackage, L"--swassert")) {
    SwAssertInject = TRUE;
  }

  if (ShellCommandLineGetFlag (ParamPackage, L"--exception")) {
    ExceptionInject = TRUE;
  }

  if (ShellCommandLineGetFlag (ParamPackage, L"--swassert_mm")) {
    SwAssertInjectMM = TRUE;
  }

  if (SwAssertInject && ExceptionInject && SwAssertInjectMM) {
    ShellPrintHiiEx (-1, -1, NULL, STRING_TOKEN (STR_TEST_INJECT_ASSERT_EXCEPTION), HiiHandle, AppName);
    goto TestInjectAssertDone;
  }

  if (SwAssertInject == TRUE) {
    ErrorPrint (L"%a: INJECTING AN ASSERT \r\n", __FUNCTION__);
    InValidateActiveBootChain ();
    ASSERT (FALSE);
  } else if (ExceptionInject == TRUE) {
    InValidateActiveBootChain ();
    *TestPtr = 8;
  } else if (SwAssertInjectMM == TRUE) {
    ErrorPrint (L"%a: INJECTING AN ASSERT IN MM \r\n", __FUNCTION__);
    InjectAssertInMM (ImageHandle, SystemTable);
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"%a: Inject Assert in MM failed: %r\n", __FUNCTION__, Status);
      goto TestInjectAssertDone;
    }

    InValidateActiveBootChain ();
    ASSERT (FALSE);
  } else {
    ErrorPrint (L"%a: No assert to inject\r\n", __FUNCTION__);
    goto TestInjectAssertDone;
  }

TestInjectAssertDone:
  ShellCommandLineFreeVarList (ParamPackage);
  HiiRemovePackages (HiiHandle);

  return EFI_SUCCESS;
}
