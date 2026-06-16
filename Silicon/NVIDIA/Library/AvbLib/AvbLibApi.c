/** @file
  EDK2 API for LibAvb

  SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi/UefiBaseType.h>

#include <Library/BaseLib.h>
#include <Library/NVIDIADebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/NVIDIADebugLib.h>
#include <Library/HandleParsingLib.h>
#include <Library/BootChainInfoLib.h>
#include <Library/SiblingPartitionLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DeviceTreeHelperLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/BootConfigProtocolLib.h>
#include <Library/OpteeNvLib.h>
#include <Library/AndroidBcbLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/NctLib.h>

#include <Protocol/PartitionInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>

#include "libavb/libavb/libavb.h"
#include "Library/AvbLib.h"

#define UPPER_32_BITS(n)  ((UINT32)((n) >> 32))
#define LOWER_32_BITS(n)  ((UINT32)(n))

#define LIBAVB_MODULUS_OFFSET  8
#define LIBAVB_KEY2MOD(x)  ((x) + LIBAVB_MODULUS_OFFSET)
#define LIBAVB_KEY_LEN  (LIBAVB_MODULUS_OFFSET + (VERITY_KEY_SIZE * 2))

#define MAX_SN_LEN              32
#define PatchLevelStrFormatLen  10

//
// Build-time switch for the factory-flash AVB unlock-state reset override.
// Driven by gNVIDIATokenSpaceGuid.PcdAvbEnableFactoryUnlockReset (default
// FALSE in NVIDIA.dec, overridden to TRUE in NVIDIA.common.dsc.inc under
// CONFIG_BUILD_ANDROID && CONFIG_SOC_T23X).
//   TRUE  = enable: read the platform scratch register at the start of
//           AvbVerifyBoot and, when the reset-request bit is set, sync the
//           AVB TA lock state to match the register.
//   FALSE = disable: AvbVerifyBoot ignores the scratch register entirely
//           and AvbApplyFactoryUnlockReset / its helpers are not compiled.
//
// Build-time switch for the NCT integrity SHA-256 seed/verify path.
// Driven by gNVIDIATokenSpaceGuid.PcdAvbEnableNctIntegrityHash (default
// FALSE in NVIDIA.dec, overridden to TRUE in NVIDIA.common.dsc.inc under
// CONFIG_BUILD_ANDROID && CONFIG_SOC_T23X).
//   TRUE  = enable: on first boot the SHA-256 of NCT is written to
//           RPMB; on later boots it is compared against the stored
//           digest and any mismatch unconditionally forces BootState
//           to RED (NCT integrity is enforced regardless of lock
//           state / pre-existing AVB result).
//   FALSE = disable: AvbSeedNctIntegrityHash and its call site are
//           not compiled.
//
#if FixedPcdGetBool (PcdAvbEnableFactoryUnlockReset)
//
// Factory-flash seed for the AVB device unlock state (T234 only).
//
// SCRATCH_SECURE_RSV103_SCRATCH_0 is programmed during factory flash to
// convey the initial unlock state to the bootloader:
//   bit 1 (BIT1): 1 = locked, 0 = unlocked.
//
// The bit is consulted only on first boot, when the AVB TA has no persisted
// lock state yet (AvbReadDeviceLockedState returns EFI_NOT_FOUND). Once the
// AVB TA holds a value -- whether seeded from here or written by fastboot --
// it is treated as authoritative and this register is ignored.
//
#define T234_SCRATCH_BASE                            0x0C390000
#define SCRATCH_SECURE_RSV103_SCRATCH_0_OFFSET       0x39C
#define SCRATCH_SECURE_RSV103_SCRATCH_0_ADDR         (T234_SCRATCH_BASE + SCRATCH_SECURE_RSV103_SCRATCH_0_OFFSET)
#define SCRATCH_SECURE_RSV103_UNLOCK_LOCK_STATE_BIT  BIT1
#endif

STATIC EFI_HANDLE      mControllerHandle;
STATIC AVB_BOOT_STATE  mAvbBootState = VERIFIED_BOOT_UNKNOWN_STATE;

EFI_STATUS
AvbShowUi (
  IN AVB_BOOT_STATE  BootState
  );

/*
 * @brief Holds information about the public key used to validate
 *        the ROT binary.
 * @param PubKey      Pointer to the public key stored within |slot_data|
 * @param Len         Length of the public key in bytes
 * @param IsTrusted   Indicates if platform key == public key used
 *                    to validate the ROT binary.
 */
STATIC struct AvbKeyData {
  CONST UINT8    *PubKey;
  UINT32         Len;
  BOOLEAN        IsTrusted;
} gAvbKeyData;

EFI_STATUS
AvbReadDeviceLockedState (
  OUT BOOLEAN  *IsLocked
  )
{
  EFI_STATUS                 Status            = EFI_SUCCESS;
  OPTEE_INVOKE_FUNCTION_ARG  InvokeFunctionArg = { 0 };

  InvokeFunctionArg.Function            = TA_AVB_CMD_READ_LOCK_STATE;
  InvokeFunctionArg.Params[0].Attribute = OPTEE_MESSAGE_ATTRIBUTE_TYPE_VALUE_OUTPUT;

  Status = AvbOpteeInvoke (&InvokeFunctionArg);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r trying to read unlocked state from AVB TA\n", __FUNCTION__, Status));
    return Status;
  }

  *IsLocked = (InvokeFunctionArg.Params[0].Union.Value.A == 0) ? FALSE : TRUE;

  return EFI_SUCCESS;
}

EFI_STATUS
AvbWriteDeviceLockedState (
  BOOLEAN  IsLocked
  )
{
  EFI_STATUS                 Status            = EFI_SUCCESS;
  OPTEE_INVOKE_FUNCTION_ARG  InvokeFunctionArg = { 0 };

  InvokeFunctionArg.Function                = TA_AVB_CMD_WRITE_LOCK_STATE;
  InvokeFunctionArg.Params[0].Attribute     = OPTEE_MESSAGE_ATTRIBUTE_TYPE_VALUE_INPUT;
  InvokeFunctionArg.Params[0].Union.Value.A = IsLocked ? 1 : 0;

  Status = AvbOpteeInvoke (&InvokeFunctionArg);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r trying to set unlocked state from AVB TA\n", __FUNCTION__, Status));
    return Status;
  }

  return EFI_SUCCESS;
}

#if FixedPcdGetBool (PcdAvbEnableFactoryUnlockReset)

/**
  Read the factory-flash-encoded target lock state from the platform scratch
  register.

  SCRATCH_SECURE_RSV103_SCRATCH_0 bit 1 holds the target lock state
  programmed by the factory flash tool (1 = locked, 0 = unlocked). The read
  is non-destructive; the register is only consulted when the AVB TA has no
  persisted state yet, so the bit acts as a one-time seed.

  @return  TRUE if the factory wants the device locked, FALSE if unlocked.
**/
STATIC
BOOLEAN
AvbReadFactoryLockStateFromScratch (
  VOID
  )
{
  UINT32   Value;
  BOOLEAN  IsLocked;

  Value    = MmioRead32 (SCRATCH_SECURE_RSV103_SCRATCH_0_ADDR);
  IsLocked = ((Value & SCRATCH_SECURE_RSV103_UNLOCK_LOCK_STATE_BIT) != 0);

  DEBUG ((
    DEBUG_ERROR,
    "%a: SCRATCH_SECURE_RSV103_SCRATCH_0 (0x%lx) = 0x%08x, factory target=%a\n",
    __FUNCTION__,
    (UINT64)SCRATCH_SECURE_RSV103_SCRATCH_0_ADDR,
    Value,
    IsLocked ? "locked" : "unlocked"
    ));

  return IsLocked;
}

/**
  Seed the AVB TA lock state from the factory-flash scratch register on
  first boot.

  Strategy:
    1. Probe the AVB TA via AvbReadDeviceLockedState.
    2. If it returns success, the TA already holds an authoritative value
       (either a previous factory seed or a fastboot lock/unlock); do not
       overwrite it.
    3. If it returns EFI_NOT_FOUND, the TA has no state yet (fresh factory
       flash or RPMB wipe). Read SCRATCH_SECURE_RSV103_SCRATCH_0 bit 1 and
       write it back so subsequent AvbReadDeviceLockedState calls return
       the factory-provisioned value.
    4. On any other error, surface it; we do not want to mask TA failures
       with a scratch-derived guess.

  @retval EFI_SUCCESS  TA already had a state, or the factory state was
                       written successfully.
  @retval Others       TA read failed for a reason other than NOT_FOUND,
                       or the factory state could not be written.
**/
STATIC
EFI_STATUS
AvbApplyFactoryUnlockReset (
  VOID
  )
{
  BOOLEAN     CurrentLocked    = FALSE;
  BOOLEAN     FactoryLockState = FALSE;
  EFI_STATUS  Status;

  Status = AvbReadDeviceLockedState (&CurrentLocked);
  if (!EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: AVB TA already holds lock state (%a); skipping factory seed\n",
      __FUNCTION__,
      CurrentLocked ? "locked" : "unlocked"
      ));
    return EFI_SUCCESS;
  }

  if (Status != EFI_NOT_FOUND) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Got %r reading AVB TA lock state; not seeding from scratch\n",
      __FUNCTION__,
      Status
      ));
    return Status;
  }

  FactoryLockState = AvbReadFactoryLockStateFromScratch ();

  Status = AvbWriteDeviceLockedState (FactoryLockState);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Got %r writing factory lock state (%a) to AVB TA\n",
      __FUNCTION__,
      Status,
      FactoryLockState ? "locked" : "unlocked"
      ));
    return Status;
  }

  DEBUG ((
    DEBUG_ERROR,
    "%a: seeded AVB TA lock state from factory scratch (target=%a)\n",
    __FUNCTION__,
    FactoryLockState ? "locked" : "unlocked"
    ));
  return EFI_SUCCESS;
}

#endif // FixedPcdGetBool (PcdAvbEnableFactoryUnlockReset)

/**
  Read tamper-evident storage, parse device unlocked state.

  Pure read path: defers seeding a missing AVB TA state to
  AvbApplyFactoryUnlockReset (which runs once during AvbVerifyBoot, before
  this callback is invoked by libavb), so EFI_NOT_FOUND is surfaced to the
  caller as AVB_IO_RESULT_ERROR_NO_SUCH_VALUE rather than silently coerced
  to "locked". This keeps the TA the single source of truth and avoids a
  double-write race with the factory seed path.

  @param[in]  Ops         A pointer to the AvbOps struct.
  @param[out] IsUnlocked  True if device is unlocked.

  @retval AVB_IO_RESULT_OK                  Successfully read the state.
  @retval AVB_IO_RESULT_ERROR_NO_SUCH_VALUE TA has no persisted state.
  @retval AVB_IO_RESULT_ERROR_IO            Other TA read failure.

**/
STATIC
AvbIOResult
ReadIsDeviceUnlocked (
  IN  AvbOps  *Ops,
  OUT bool    *IsUnlocked
  )
{
  EFI_STATUS  Status;
  BOOLEAN     DeviceLocked = FALSE;

  Status = AvbReadDeviceLockedState (&DeviceLocked);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r trying to read locked state from AVB TA\n", __FUNCTION__, Status));
    return (Status == EFI_NOT_FOUND) ? AVB_IO_RESULT_ERROR_NO_SUCH_VALUE : AVB_IO_RESULT_ERROR_IO;
  }

  *IsUnlocked = (DeviceLocked == FALSE) ? TRUE : FALSE;
  return AVB_IO_RESULT_OK;
}

/**
  Get size of a given partition.

  @param[in]  Ops             A pointer to the AvbOps struct.
  @param[in]  Partition       Partition name string.
  @param[out] OutSizeNumBytes Output buffer to store partition size

  @retval AVB_IO_RESULT_OK  The operation completed successfully.

**/
STATIC
AvbIOResult
GetSizeOfPartition (
  IN  AvbOps      *Ops,
  IN  const char  *Partition,
  OUT uint64_t    *OutSizeNumBytes
  )
{
  EFI_STATUS             Status = EFI_SUCCESS;
  EFI_HANDLE             PartitionHandle;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINTN                  PartitionSize;
  CHAR16                 PartitionName[MAX_PARTITION_NAME_LEN];
  CHAR16                 ActivePartitionName[MAX_PARTITION_NAME_LEN];
  AvbIOResult            AvbResult = AVB_IO_RESULT_OK;

  if (AsciiStrCmp (Partition, "recovery") == 0) {
    UnicodeSPrintAsciiFormat (ActivePartitionName, sizeof (ActivePartitionName), "%a", Partition);
  } else {
    UnicodeSPrintAsciiFormat (PartitionName, sizeof (PartitionName), "%a", Partition);

    Status = GetActivePartitionName (PartitionName, ActivePartitionName);
    if (EFI_ERROR (Status)) {
      AvbResult = AVB_IO_RESULT_ERROR_NO_SUCH_PARTITION;
      goto Exit;
    }
  }

  PartitionHandle = GetSiblingPartitionHandle (
                      mControllerHandle,
                      ActivePartitionName
                      );

  if (PartitionHandle == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Unable to found sibling partition handle for %s\r\n", __FUNCTION__, ActivePartitionName));
    AvbResult = AVB_IO_RESULT_ERROR_NO_SUCH_PARTITION;
    goto Exit;
  }

  Status = gBS->HandleProtocol (
                  PartitionHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&BlockIo
                  );
  if (EFI_ERROR (Status) || (BlockIo == NULL)) {
    DEBUG ((DEBUG_ERROR, "%a: Unable to locate block io protocol on partition\r\n", __FUNCTION__));
    AvbResult = AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

  PartitionSize    = (UINTN)(BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize;
  *OutSizeNumBytes = (uint64_t)PartitionSize;

Exit:
  return AvbResult;
}

/**
  Read parition data from given offset.

  @param[in]  Ops         A pointer to the AvbOps struct.
  @param[in]  Partition   Partition name string.
  @param[in]  Offset      Read from this offset (negative means read from bottom).
  @param[in]  NumBytes    Num of bytes to read.
  @param[out] Buffer      Buffer address to read into.
  @param[out] NumRead     Actual bytes read from storage.

  @retval AVB_IO_RESULT_OK  The operation completed successfully.

**/
STATIC
AvbIOResult
ReadFromPartition (
  IN  AvbOps      *Ops,
  IN  const char  *Partition,
  IN  int64_t     Offset,
  IN  size_t      NumBytes,
  OUT VOID        *Buffer,
  OUT size_t      *NumRead
  )
{
  EFI_STATUS             Status = EFI_SUCCESS;
  EFI_HANDLE             PartitionHandle;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  EFI_DISK_IO_PROTOCOL   *DiskIo;
  CHAR16                 PartitionName[MAX_PARTITION_NAME_LEN];
  CHAR16                 ActivePartitionName[MAX_PARTITION_NAME_LEN];
  AvbIOResult            AvbResult = AVB_IO_RESULT_OK;

  if (AsciiStrCmp (Partition, "recovery") == 0) {
    UnicodeSPrintAsciiFormat (ActivePartitionName, sizeof (ActivePartitionName), "%a", Partition);
  } else {
    UnicodeSPrintAsciiFormat (PartitionName, sizeof (PartitionName), "%a", Partition);

    Status = GetActivePartitionName (PartitionName, ActivePartitionName);
    if (EFI_ERROR (Status)) {
      AvbResult = AVB_IO_RESULT_ERROR_NO_SUCH_PARTITION;
      goto Exit;
    }
  }

  PartitionHandle = GetSiblingPartitionHandle (
                      mControllerHandle,
                      ActivePartitionName
                      );
  if (PartitionHandle == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Unable to get sibling partition handle: %s\n", __FUNCTION__, ActivePartitionName));
    AvbResult = AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

  Status = gBS->HandleProtocol (
                  PartitionHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&BlockIo
                  );
  if (EFI_ERROR (Status) || (BlockIo == NULL)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r to locate block io protocol on partition\r\n", __FUNCTION__, Status));
    AvbResult = AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

  Status = gBS->HandleProtocol (
                  PartitionHandle,
                  &gEfiDiskIoProtocolGuid,
                  (VOID **)&DiskIo
                  );
  if (EFI_ERROR (Status) || (DiskIo == NULL)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r to locate disk io protocol on partition\r\n", __FUNCTION__, Status));
    AvbResult = AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

  if (Offset < 0) {
    Offset += (UINTN)(BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize;
  }

  // Make sure offset < PartitionSize
  if (Offset >= (UINTN)(BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid offset=%d, larger than partition size\n", __FUNCTION__, (INT32)Offset));
    AvbResult = AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

  Status = DiskIo->ReadDisk (
                     DiskIo,
                     BlockIo->Media->MediaId,
                     Offset,
                     NumBytes,
                     (VOID *)Buffer
                     );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to read disk with %r\r\n", __FUNCTION__, Status));
    AvbResult = AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

  *NumRead = NumBytes;

Exit:
  return AvbResult;
}

/**
  Read parition data from given offset.

  @param[in]  Ops         A pointer to the AvbOps struct.
  @param[in]  Partition   Partition name string.
  @param[out] GuidBuf     Output buffer for partition guid.
  @param[in]  GuidBufSize Size of partition guid buffer.

  @retval AVB_IO_RESULT_OK  The operation completed successfully.

**/
STATIC
AvbIOResult
GetUniqueGuidForPartition (
  IN  AvbOps      *Ops,
  IN  const char  *Partition,
  OUT char        *GuidBuf,
  IN  size_t      GuidBufSize
  )
{
  EFI_STATUS                   Status = EFI_SUCCESS;
  EFI_PARTITION_INFO_PROTOCOL  *PartitionInfo;
  EFI_HANDLE                   PartitionHandle;
  CHAR16                       PartitionName[MAX_PARTITION_NAME_LEN];
  CHAR16                       ActivePartitionName[MAX_PARTITION_NAME_LEN];
  AvbIOResult                  AvbResult = AVB_IO_RESULT_OK;
  EFI_GUID                     *PartitionGuid;

  UnicodeSPrintAsciiFormat (PartitionName, sizeof (PartitionName), "%a", Partition);

  Status = GetActivePartitionName (PartitionName, ActivePartitionName);
  if (EFI_ERROR (Status)) {
    AvbResult = AVB_IO_RESULT_ERROR_NO_SUCH_PARTITION;
    goto Exit;
  }

  PartitionHandle = GetSiblingPartitionHandle (
                      mControllerHandle,
                      ActivePartitionName
                      );
  if (PartitionHandle == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Unable to get sibling partition handle: %s\n", __FUNCTION__, ActivePartitionName));
    AvbResult = AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

  Status = gBS->HandleProtocol (
                  PartitionHandle,
                  &gEfiPartitionInfoProtocolGuid,
                  (VOID **)&PartitionInfo
                  );
  if (EFI_ERROR (Status) || (PartitionInfo == NULL)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r to get partition info, or PartitionInfo == NULL\n", __FUNCTION__, Status));
    AvbResult = AVB_IO_RESULT_ERROR_NO_SUCH_PARTITION;
    goto Exit;
  }

  PartitionGuid = &PartitionInfo->Info.Gpt.UniquePartitionGUID;
  AsciiSPrintUnicodeFormat (
    GuidBuf,
    GuidBufSize,
    L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    PartitionGuid->Data1,
    PartitionGuid->Data2,
    PartitionGuid->Data3,
    PartitionGuid->Data4[0],
    PartitionGuid->Data4[1],
    PartitionGuid->Data4[2],
    PartitionGuid->Data4[3],
    PartitionGuid->Data4[4],
    PartitionGuid->Data4[5],
    PartitionGuid->Data4[6],
    PartitionGuid->Data4[7]
    );

Exit:
  return AvbResult;
}

/**
  Validate if vbmeta key0 is trusted key.

  @param[in]  Ops               A pointer to the AvbOps struct.
  @param[in]  PubKey            Key0 public key buffer.
  @param[in]  PubKeyLen         Key0 public key length.
  @param[in]  PubKeyMetadata    Public key metadata buffer.
  @param[in]  PubKeyMetadataLen Public key metadata length.
  @param[out] OutIsTrusted      Output buffer to store trusted state.

  @retval AVB_IO_RESULT_OK  The operation completed successfully.

**/
STATIC
AvbIOResult
ValidateVbmetaPublicKey (
  IN  AvbOps         *Ops,
  IN  const uint8_t  *PubKey,
  IN  size_t         PubKeyLen,
  IN  const uint8_t  *PubKeyMetadata,
  IN  size_t         PubKeyMetadataLen,
  OUT bool           *OutIsTrusted
  )
{
  BOOLEAN      Response;
  INT32        NodeOffset;
  EFI_STATUS   Status;
  UINT8        StoredHashValue[SHA1_DIGEST_SIZE];
  UINT8        *HashValueInDtb = NULL;
  UINT32       KeyLenInDtb;
  UINT32       KeyHashLenInDtb;
  AvbIOResult  AvbResult = AVB_IO_RESULT_OK;

  *OutIsTrusted = FALSE;

  Response = Sha1HashAll (PubKey, PubKeyLen, StoredHashValue);
  if (Response == FALSE) {
    AvbResult = AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

  // Read Pubkey Hash from UEFI-DTB
  NodeOffset = -1;
  Status     = DeviceTreeGetNodeByPath ("/chosen", &NodeOffset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r getting /chosen\n", __FUNCTION__, Status));
    AvbResult = AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

  Status = DeviceTreeGetNodePropertyValue32 (NodeOffset, "avb_key0_size", &KeyLenInDtb);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r geting avb_key0_size \n", __FUNCTION__, Status));
    // no key0 makes AVB boot to yellow state
    AvbResult = AVB_IO_RESULT_OK;
    goto Exit;
  }

  Status = DeviceTreeGetNodeProperty (NodeOffset, "avb_key0_sha1", (CONST VOID **)&HashValueInDtb, &KeyHashLenInDtb);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r getting avb_key0_sha1 \n", __FUNCTION__, Status));
    // no key0 makes AVB boot to yellow state
    AvbResult = AVB_IO_RESULT_OK;
    goto Exit;
  }

  // Compare pubkey0 SHA1
  if ((KeyLenInDtb == PubKeyLen) &&
      (KeyHashLenInDtb == SHA1_DIGEST_SIZE) &&
      (0 == CompareMem ((VOID *)StoredHashValue, (VOID *)HashValueInDtb, SHA1_DIGEST_SIZE)))
  {
    *OutIsTrusted = TRUE;
  }

  gAvbKeyData.PubKey    = PubKey;
  gAvbKeyData.Len       = PubKeyLen;
  gAvbKeyData.IsTrusted = *OutIsTrusted;

Exit:
  return AvbResult;
}

/**
  Write rollback index to location in tamper-evident storage.

  @param[in]  Ops                    A pointer to the AvbOps struct.
  @param[in]  RollbackIndexLocation  Location for rollback index in tamper-evident storage.
  @param[in]  RollbackIndex          Rollback index to set.

  @retval AVB_IO_RESULT_OK  The operation completed successfully.

**/
STATIC
AvbIOResult
WriteRollbackIndex (
  IN AvbOps    *Ops,
  IN size_t    RollbackIndexLocation,
  IN uint64_t  RollbackIndex
  )
{
  EFI_STATUS                 Status            = EFI_SUCCESS;
  AvbIOResult                AvbResult         = AVB_IO_RESULT_OK;
  OPTEE_INVOKE_FUNCTION_ARG  InvokeFunctionArg = { 0 };

  InvokeFunctionArg.Function                = TA_AVB_CMD_WRITE_ROLLBACK_INDEX;
  InvokeFunctionArg.Params[0].Attribute     = OPTEE_MESSAGE_ATTRIBUTE_TYPE_VALUE_INPUT;
  InvokeFunctionArg.Params[0].Union.Value.A = (UINT64)RollbackIndexLocation;
  InvokeFunctionArg.Params[1].Attribute     = OPTEE_MESSAGE_ATTRIBUTE_TYPE_VALUE_INPUT;
  InvokeFunctionArg.Params[1].Union.Value.A = UPPER_32_BITS (RollbackIndex);
  InvokeFunctionArg.Params[1].Union.Value.B = LOWER_32_BITS (RollbackIndex);

  Status = AvbOpteeInvoke (&InvokeFunctionArg);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r trying to write rollback index 0x%lx to 0x%lx\r\n", __FUNCTION__, Status, RollbackIndex, RollbackIndexLocation));
    AvbResult = AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

Exit:
  return AvbResult;
}

/**
  Read rollback index from location in tamper-evident storage.

  @param[in]  Ops                    A pointer to the AvbOps struct.
  @param[in]  RollbackIndexLocation  Location for rollback index in tamper-evident storage.
  @param[out] RollbackIndex          Buffer for rollback index to read.

  @retval AVB_IO_RESULT_OK  The operation completed successfully.

**/
STATIC
AvbIOResult
ReadRollbackIndex (
  IN  AvbOps    *Ops,
  IN  size_t    RollbackIndexLocation,
  OUT uint64_t  *OutRollbackIndex
  )
{
  EFI_STATUS                 Status            = EFI_SUCCESS;
  AvbIOResult                AvbResult         = AVB_IO_RESULT_OK;
  OPTEE_INVOKE_FUNCTION_ARG  InvokeFunctionArg = { 0 };

  if (OutRollbackIndex == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: OutRollbackIndex == NULL\n", __FUNCTION__));
    AvbResult = AVB_IO_RESULT_ERROR_NO_SUCH_VALUE;
    goto Exit;
  }

  InvokeFunctionArg.Function                = TA_AVB_CMD_READ_ROLLBACK_INDEX;
  InvokeFunctionArg.Params[0].Attribute     = OPTEE_MESSAGE_ATTRIBUTE_TYPE_VALUE_INPUT;
  InvokeFunctionArg.Params[0].Union.Value.A = (UINT64)RollbackIndexLocation;
  InvokeFunctionArg.Params[1].Attribute     = OPTEE_MESSAGE_ATTRIBUTE_TYPE_VALUE_OUTPUT;

  Status = AvbOpteeInvoke (&InvokeFunctionArg);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r trying to read rolback index from 0x%lx\r\n", __FUNCTION__, Status, RollbackIndexLocation));
    AvbResult = (Status == EFI_NOT_FOUND) ? AVB_IO_RESULT_ERROR_NO_SUCH_VALUE : AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

  *OutRollbackIndex = (InvokeFunctionArg.Params[1].Union.Value.A << 32)
                      + InvokeFunctionArg.Params[1].Union.Value.B;

Exit:
  return AvbResult;
}

/**
  Commit (advance) rollback indexes after a confirmed-successful prior boot.

  libavb's avb_slot_verify() deliberately does NOT bump the stored rollback
  index; that responsibility is left to the bootloader so it can implement a
  safe commit policy (see external/avb/libavb/avb_slot_verify.h and AOSP's
  reference avb_ab_flow_do_io). NVIDIA's UEFI never performed this commit, so
  the RPMB-backed counter stays at its factory value forever and the
  anti-rollback property is not actually enforced across reboots.

  Policy implemented here (deferred commit):

    1. The new payload's vbmeta must have passed AVB verification, i.e.
       SlotData is non-NULL and BootState == VERIFIED_BOOT_GREEN_STATE
       (fully verified with the platform/embedded key, device locked).
       YELLOW (user key) and ORANGE (unlocked) intentionally do not commit.

    2. The active slot in BCB must already be marked SuccessfulBoot=1.
       That bit is set by Android (bootctl markBootSuccessful /
       IBootControl) only after the system is fully up. By gating on it we
       only advance the stored rollback index on the boot AFTER the new
       slot proved itself good, which is the standard A/B anti-brick rule:
       if the new slot bricks before Android can mark it successful, the
       stored rollback index has not advanced yet and any rollback path
       (BCB retry exhaustion, FW chain switch) is still permitted.

    3. Per location, only WriteRollbackIndex when payload value is strictly
       greater than the currently stored value. Monotonic increase only;
       never decrement, never re-write the same value (avoids unnecessary
       RPMB writes / wear).

  Failures here are logged but never propagated: AVB verify already
  succeeded, so we must not turn a successful boot into a failure just
  because the commit thunk to OP-TEE / RPMB had a transient error.

  @param[in]  SlotData    AvbSlotVerifyData from the just-completed verify.
                          Source of payload rollback_indexes[] values.
  @param[in]  BootState   Resolved AVB boot state for this boot.

  Return policy: only truly anomalous conditions surface as an error.
  Policy-driven skips (not GREEN, prior boot not yet marked successful) and
  best-effort per-location read/write failures already produce DEBUG logs, so
  they all return EFI_SUCCESS to keep the caller's control flow simple. Real
  I/O errors that prevent the policy decision itself from being made (e.g. the
  MSC partition can't be read) are propagated, since we can't safely conclude
  whether a commit is warranted.

  @retval EFI_SUCCESS              Commit step finished. May mean all
                                   locations were committed, partially
                                   committed, or fully skipped per policy
                                   (see DEBUG log for the detailed reason).
  @retval EFI_INVALID_PARAMETER    SlotData is NULL (broken contract;
                                   logged then returned).
  @retval other EFI_STATUS         Underlying error from
                                   AndroidBcbGetActiveSlotSuccessful() when
                                   the MSC partition could not be read.
**/
STATIC
EFI_STATUS
AvbCommitRollbackIndexes (
  IN AvbSlotVerifyData  *SlotData,
  IN AVB_BOOT_STATE     BootState
  )
{
  EFI_STATUS   Status;
  AvbIOResult  AvbResult;
  BOOLEAN      PriorBootSuccessful;
  UINT32       Idx;
  uint64_t     StoredIndex;
  uint64_t     PayloadIndex;
  UINT32       CommittedCount = 0;

  if (SlotData == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: SlotData is NULL\n", __FUNCTION__));
    return EFI_INVALID_PARAMETER;
  }

  // Gate 1: only commit on a fully-trusted GREEN boot. YELLOW/ORANGE/RED
  // deliberately do not advance the stored counter.
  if (BootState != VERIFIED_BOOT_GREEN_STATE) {
    DEBUG ((
      DEBUG_INFO,
      "%a: BootState=%d != GREEN, skipping rollback-index commit\n",
      __FUNCTION__,
      BootState
      ));
    return EFI_SUCCESS;
  }

  // Gate 2: only commit if the PRIOR boot was marked successful by Android.
  // This is the "deferred commit" the user asked for: we never advance the
  // stored index on the very first boot of a new slot; we wait until Android
  // has had a chance to call markBootSuccessful, then commit on the next
  // boot (which will re-verify the same slot and read SuccessfulBoot=1).
  PriorBootSuccessful = FALSE;
  Status              = AndroidBcbGetActiveSlotSuccessful (NULL, &PriorBootSuccessful);
  if (EFI_ERROR (Status)) {
    // Real I/O failure reading the MSC partition (the BCB-uninitialized /
    // CRC-mismatch case is absorbed by AndroidBcbGetActiveSlotSuccessful as
    // SUCCESS with Successful=FALSE, so anything that reaches this branch is
    // an unexpected hardware/protocol error and must be reported.
    DEBUG ((
      DEBUG_ERROR,
      "%a: AndroidBcbGetActiveSlotSuccessful returned %r, skipping commit\n",
      __FUNCTION__,
      Status
      ));
    return Status;
  }

  if (!PriorBootSuccessful) {
    DEBUG ((
      DEBUG_INFO,
      "%a: Active slot not yet marked successful by Android, "
      "deferring rollback-index commit to next boot\n",
      __FUNCTION__
      ));
    return EFI_SUCCESS;
  }

  // Gate 3 (per location): only bump where payload > stored.
  for (Idx = 0; Idx < AVB_MAX_NUMBER_OF_ROLLBACK_INDEX_LOCATIONS; Idx++) {
    PayloadIndex = SlotData->rollback_indexes[Idx];
    if (PayloadIndex == 0) {
      // libavb leaves locations unused by this vbmeta tree at 0. Nothing
      // to commit; skip without an RPMB read.
      continue;
    }

    StoredIndex = 0;
    AvbResult   = ReadRollbackIndex (NULL, (size_t)Idx, &StoredIndex);
    if (AvbResult != AVB_IO_RESULT_OK) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: ReadRollbackIndex(loc=%u) failed (%d), skipping this location\n",
        __FUNCTION__,
        Idx,
        AvbResult
        ));
      continue;
    }

    if (PayloadIndex <= StoredIndex) {
      // Same value (no-op) or stored is already higher (should never
      // happen for a GREEN verify, but be defensive and never decrement).
      continue;
    }

    AvbResult = WriteRollbackIndex (NULL, (size_t)Idx, PayloadIndex);
    if (AvbResult != AVB_IO_RESULT_OK) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: WriteRollbackIndex(loc=%u, %lu->%lu) failed (%d)\n",
        __FUNCTION__,
        Idx,
        StoredIndex,
        PayloadIndex,
        AvbResult
        ));
      continue;
    }

    DEBUG ((
      DEBUG_ERROR,
      "%a: Committed rollback index loc=%u: %lu -> %lu\n",
      __FUNCTION__,
      Idx,
      StoredIndex,
      PayloadIndex
      ));
    CommittedCount++;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: rollback-index commit done, %u location(s) advanced\n",
    __FUNCTION__,
    CommittedCount
    ));

  return EFI_SUCCESS;
}

/**
  Validate if vbmeta partition key is trusted key.

  @param[in]  Ops                       A pointer to the AvbOps struct.
  @param[in]  Partition                 Partition name string for the key.
  @param[in]  PubKeyData                Key0 public key buffer.
  @param[in]  PubKeyLength              Key0 public key length.
  @param[in]  PubKeyMetadata            Public key metadata buffer.
  @param[in]  PubKeyMetadataLen         Public key metadata length.
  @param[out] OutIsTrusted              Output buffer to store trusted state.
  @param[out] *OutRollbackIndexLocation Output buffer to store rollback location.

  @retval AVB_IO_RESULT_OK  The operation completed successfully.

**/
STATIC
AvbIOResult
ValidatePublicKeyForPartition (
  IN  AvbOps         *Ops,
  IN  const char     *Partition,
  IN  const uint8_t  *PubKeyData,
  IN  size_t         PubKeyLength,
  IN  const uint8_t  *PubKeyMetadata,
  IN  size_t         PubKeyMetadataLen,
  OUT bool           *OutIsTrusted,
  OUT uint32_t       *OutRollbackIndexLocation
  )
{
  if ((OutIsTrusted == NULL) || (OutRollbackIndexLocation == NULL)) {
    DEBUG ((DEBUG_ERROR, "%a: OutIsTrusted or OutRollbackIndexLocation == NULL\n", __FUNCTION__));
    return AVB_IO_RESULT_ERROR_NO_SUCH_VALUE;
  }

  *OutRollbackIndexLocation = 1;
  *OutIsTrusted             = TRUE;

  return AVB_IO_RESULT_OK;
}

/**
  Read persistent value in tamper-evident storage.

  @param[in]  Ops                    A pointer to the AvbOps struct.
  @param[in]  Name                   Persistent value name to read.
  @param[in]  BufferSize             Buffer size provided.
  @param[out] OutBuffer              Output buffer for persistent value.
  @param[out] OutNumBytesRead        Num of bytes read.

  @retval AVB_IO_RESULT_OK  The operation completed successfully.

**/
STATIC
AvbIOResult
ReadPersistentValue (
  IN  AvbOps              *Ops,
  IN  const         char  *Name,
  IN  size_t              BufferSize,
  OUT uint8_t             *OutBuffer,
  OUT size_t              *OutNumBytesRead
  )
{
  EFI_STATUS                 Status            = EFI_SUCCESS;
  AvbIOResult                AvbResult         = AVB_IO_RESULT_OK;
  OPTEE_INVOKE_FUNCTION_ARG  InvokeFunctionArg = { 0 };
  VOID                       *NameBuffer       = NULL;
  UINT32                     NameLength;

  if ((OutBuffer == NULL) || (OutNumBytesRead == NULL)) {
    DEBUG ((DEBUG_ERROR, "%a: OutBuffer or OutNumBytesRead == NULL\n", __FUNCTION__));
    return AVB_IO_RESULT_ERROR_NO_SUCH_VALUE;
  }

  NameLength = AsciiStrLen (Name);
  NameBuffer = AllocateCopyPool (NameLength, Name);

  InvokeFunctionArg.Function                             = TA_AVB_CMD_READ_PERSIST_VALUE;
  InvokeFunctionArg.Params[0].Attribute                  = OPTEE_MESSAGE_ATTRIBUTE_TYPE_MEMORY_INPUT;
  InvokeFunctionArg.Params[0].Union.Memory.BufferAddress = (UINT64)NameBuffer;
  InvokeFunctionArg.Params[0].Union.Memory.Size          = NameLength;
  InvokeFunctionArg.Params[1].Attribute                  = OPTEE_MESSAGE_ATTRIBUTE_TYPE_MEMORY_INOUT;
  InvokeFunctionArg.Params[1].Union.Memory.BufferAddress = (UINT64)OutBuffer;
  InvokeFunctionArg.Params[1].Union.Memory.Size          = BufferSize;

  Status = AvbOpteeInvoke (&InvokeFunctionArg);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r trying to read persist value - %a\n", __FUNCTION__, Status, Name));
    AvbResult = (Status == EFI_NOT_FOUND) ? AVB_IO_RESULT_ERROR_NO_SUCH_VALUE : AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

  *OutNumBytesRead = InvokeFunctionArg.Params[1].Union.Memory.Size;

Exit:
  if (NameBuffer != NULL) {
    FreePool (NameBuffer);
  }

  return AvbResult;
}

/**
  Write persistent value in tamper-evident storage.

  @param[in]  Ops                    A pointer to the AvbOps struct.
  @param[in]  Name                   Persistent value name to read.
  @param[in]  BufferSize             Buffer size provided.
  @param[in]  Value                  Value to write to persistent value.

  @retval AVB_IO_RESULT_OK  The operation completed successfully.

**/
STATIC
AvbIOResult
WritePersistentValue (
  IN AvbOps         *Ops,
  IN const char     *Name,
  IN size_t         BufferSize,
  IN const uint8_t  *Value
  )
{
  EFI_STATUS                 Status            = EFI_SUCCESS;
  AvbIOResult                AvbResult         = AVB_IO_RESULT_OK;
  OPTEE_INVOKE_FUNCTION_ARG  InvokeFunctionArg = { 0 };
  VOID                       *NameBuffer       = NULL;
  UINT32                     NameLength;

  NameLength = AsciiStrLen (Name);
  NameBuffer = AllocateCopyPool (NameLength, Name);

  InvokeFunctionArg.Function                             = TA_AVB_CMD_WRITE_PERSIST_VALUE;
  InvokeFunctionArg.Params[0].Attribute                  = OPTEE_MESSAGE_ATTRIBUTE_TYPE_MEMORY_INPUT;
  InvokeFunctionArg.Params[0].Union.Memory.BufferAddress = (UINT64)NameBuffer;
  InvokeFunctionArg.Params[0].Union.Memory.Size          = NameLength;
  InvokeFunctionArg.Params[1].Attribute                  = OPTEE_MESSAGE_ATTRIBUTE_TYPE_MEMORY_INPUT;
  InvokeFunctionArg.Params[1].Union.Memory.BufferAddress = (UINT64)Value;
  InvokeFunctionArg.Params[1].Union.Memory.Size          = BufferSize;

  Status = AvbOpteeInvoke (&InvokeFunctionArg);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r trying to write persist value - %a\n", __FUNCTION__, Status, Name));
    AvbResult = AVB_IO_RESULT_ERROR_IO;
    goto Exit;
  }

Exit:
  if (NameBuffer != NULL) {
    FreePool (NameBuffer);
  }

  return AvbResult;
}

#if FixedPcdGetBool (PcdAvbEnableNctIntegrityHash)

/**
  Seed or verify the NCT SHA-256 in RPMB under NCT_INTEGRITY_HASH_NAME.

  Pure "hash NCT + persist/compare in RPMB" helper; boot-state policy
  on mismatch (e.g. demoting to RED) is the caller's job. The stored
  seed is factory ground truth: written once on first boot, never
  overwritten on mismatch. Best-effort I/O failures (RPMB transient
  error, hash compute error) are non-fatal and never escalated -- an
  inconclusive read must not be turned into a tamper signal.

  @retval EFI_SUCCESS              Match, first-time seed, or
                                   best-effort I/O failure (see log).
  @retval EFI_SECURITY_VIOLATION   Stored hash differs from the
                                   freshly computed one -- confirmed
                                   NCT tampering.
  @retval Others                   NCT could not be loaded or hashed;
                                   nothing written.
**/
STATIC
EFI_STATUS
AvbSeedNctIntegrityHash (
  VOID
  )
{
  EFI_STATUS   Status;
  AvbIOResult  AvbResult;
  UINT8        ComputedHash[SHA256_DIGEST_SIZE];
  UINT8        StoredHash[SHA256_DIGEST_SIZE];
  size_t       StoredBytes = 0;

  Status = NctGetSha256Hash (ComputedHash);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Got %r computing NCT SHA-256, skipping integrity seed\n",
      __FUNCTION__,
      Status
      ));
    return Status;
  }

  AvbResult = ReadPersistentValue (
                NULL,
                NCT_INTEGRITY_HASH_NAME,
                sizeof (StoredHash),
                StoredHash,
                &StoredBytes
                );

  if (AvbResult == AVB_IO_RESULT_OK) {
    if ((StoredBytes == SHA256_DIGEST_SIZE) &&
        (CompareMem (StoredHash, ComputedHash, SHA256_DIGEST_SIZE) == 0))
    {
      DEBUG ((DEBUG_INFO, "%a: NCT integrity hash matches RPMB seed\n", __FUNCTION__));
      return EFI_SUCCESS;
    }

    // Confirmed tamper evidence; caller decides policy.
    DEBUG ((
      DEBUG_ERROR,
      "%a: NCT integrity MISMATCH (stored=%lu bytes, expected=%u bytes)\n",
      __FUNCTION__,
      (UINT64)StoredBytes,
      SHA256_DIGEST_SIZE
      ));

    return EFI_SECURITY_VIOLATION;
  }

  if (AvbResult != AVB_IO_RESULT_ERROR_NO_SUCH_VALUE) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: ReadPersistentValue(%a) returned %d, skipping integrity seed\n",
      __FUNCTION__,
      NCT_INTEGRITY_HASH_NAME,
      AvbResult
      ));
    return EFI_SUCCESS;
  }

  // First boot: no seed in RPMB yet. Persist the freshly-computed hash so
  // subsequent boots have a baseline to compare against.
  AvbResult = WritePersistentValue (
                NULL,
                NCT_INTEGRITY_HASH_NAME,
                SHA256_DIGEST_SIZE,
                ComputedHash
                );
  if (AvbResult != AVB_IO_RESULT_OK) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: WritePersistentValue(%a) failed (%d); will retry next boot\n",
      __FUNCTION__,
      NCT_INTEGRITY_HASH_NAME,
      AvbResult
      ));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "%a: Seeded NCT integrity hash to RPMB\n", __FUNCTION__));

  return EFI_SUCCESS;
}

#endif // FixedPcdGetBool (PcdAvbEnableNctIntegrityHash)

/**
  Verify avb_slot_verify and get boot state based on result.

  @param[in]  IsRecovery  If boot is recovery boot.
  @param[out] BootState   Output buffer to store boot state.
  @param[out] SlotData    Output buffer to store AvbSlotVerifyData.

  @retval AVB_IO_RESULT_OK  The operation completed successfully.

**/
STATIC
EFI_STATUS
VerifiedBootGetBootState (
  IN  BOOLEAN            IsRecovery,
  OUT AVB_BOOT_STATE     *BootState,
  OUT AvbSlotVerifyData  **SlotData
  )
{
  // Use libavb API to verify boot chain
  AvbOps               Ops = {
    .read_from_partition               = ReadFromPartition,
    .read_is_device_unlocked           = ReadIsDeviceUnlocked,
    .validate_vbmeta_public_key        = ValidateVbmetaPublicKey,
    .validate_public_key_for_partition = ValidatePublicKeyForPartition,
    .get_unique_guid_for_partition     = GetUniqueGuidForPartition,
    .get_size_of_partition             = GetSizeOfPartition,
    .read_persistent_value             = ReadPersistentValue,
    .write_persistent_value            = WritePersistentValue,
    .read_rollback_index               = ReadRollbackIndex,
    .write_rollback_index              = WriteRollbackIndex,
  };
  EFI_STATUS           Status;
  AvbSlotVerifyResult  AvbRes                         = AVB_SLOT_VERIFY_RESULT_ERROR_VERIFICATION;
  BOOLEAN              DeviceUnlocked                 = FALSE;
  AvbSlotVerifyFlags   Flags                          = 0;
  BOOLEAN              VerityCorrupted                = FALSE;
  const char           *NormalRequestedPartitions[]   = { "boot", "vendor_boot", NULL };
  const char           *RecoveryRequestedPartitions[] = { NULL };
  const char *const    *RequestedPartitions;

  if (ReadIsDeviceUnlocked (&Ops, &DeviceUnlocked) != AVB_IO_RESULT_OK) {
    return EFI_UNSUPPORTED;
  }

  RequestedPartitions = IsRecovery ? RecoveryRequestedPartitions : NormalRequestedPartitions;
  Flags              |= (DeviceUnlocked ? AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR : 0);

  // Check for dm-verity corruption flag in slot metadata
  Status = AndroidBcbGetVerityCorrupted (NULL, &VerityCorrupted);
  if (!EFI_ERROR (Status) && VerityCorrupted) {
    DEBUG ((DEBUG_INFO, "%a: Verity corruption detected, setting RESTART_CAUSED_BY_HASHTREE_CORRUPTION flag\n", __FUNCTION__));
    Flags |= AVB_SLOT_VERIFY_FLAGS_RESTART_CAUSED_BY_HASHTREE_CORRUPTION;
  }

  AvbRes = avb_slot_verify (
             &Ops,
             RequestedPartitions,
             "",
             Flags,
             AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO,
             SlotData
             );

  /**
    * Orange state:
    * Device is unlocked
    * Red state:
    * Any fatal failure during verification
    * Red EIO state:
    * I/O error during verification, or dm-verity corruption detected
    * Yellow state:
    * Verification passed, but public key for vbmeta.img does not match the
    * PKC public key for the platform
    * Green state:
    * Verification passed with the platform public key in vbmeta.img
    **/

  if (DeviceUnlocked == TRUE) {
    *BootState = VERIFIED_BOOT_ORANGE_STATE;
  } else if (AvbRes == AVB_SLOT_VERIFY_RESULT_ERROR_PUBLIC_KEY_REJECTED) {
    *BootState = VERIFIED_BOOT_YELLOW_STATE;
  } else if (AvbRes == AVB_SLOT_VERIFY_RESULT_ERROR_IO) {
    *BootState = VERIFIED_BOOT_RED_STATE_EIO;
  } else if (AvbRes != AVB_SLOT_VERIFY_RESULT_OK) {
    *BootState = VERIFIED_BOOT_RED_STATE;
  } else {
    *BootState = VERIFIED_BOOT_GREEN_STATE;
  }

  return ((*BootState != VERIFIED_BOOT_RED_STATE) && (*BootState != VERIFIED_BOOT_RED_STATE_EIO))
         ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
}

/**
  Convert a bootpatch level string to UINT32.

  @param[in] bootpatch level string as YYYY-MM-DD.

  @retval bootpatch level
**/
STATIC
UINT32
BootPatchLevelStrToL (
  IN CONST CHAR8  *Str
  )
{
  UINT32  Year, Month, Day = 0;

  if ((Str == NULL) || (AsciiStrLen (Str) != PatchLevelStrFormatLen)) {
    return 0;
  }

  // Str == YYYY-MM-DD
  Year  = 1000 * (Str[0] - '0') + 100 * (Str[1] - '0') + 10 * (Str[2] - '0') + (Str[3] - '0');
  Month = 10 * (Str[5] - '0') + (Str[6] - '0');
  Day   = 10 * (Str[8] - '0') + (Str[9] - '0');

  return 10000 * Year + 100 * Month + Day;
}

/**
  Pass ROT bootinfo to optee AVB TA.

  @param[in]  Ops                    A pointer to the AvbOps struct.
  @param[in]  SlotData               AvbSlotVerifyData output ptr from avb_slot_verify call.
  @param[in]  BootState              Avb BootState color value.

  @retval bootpatch level
**/
STATIC
EFI_STATUS
AvbPassOpteeBootInfo (
  IN AvbOps             *Ops,
  IN AvbSlotVerifyData  *SlotData,
  IN AVB_BOOT_STATE     BootState
  )
{
  EFI_STATUS   Status                      = EFI_SUCCESS;
  UINT8        AvbHash[SHA256_DIGEST_SIZE] = { 0 };
  BOOLEAN      Response;
  CHAR8        Sn[MAX_SN_LEN] = { 0 };
  INT32        NodeOffset;
  CONST CHAR8  *BootConfigStr     = NULL;
  CHAR8        *SnStartPtr        = NULL;
  CHAR8        *SnEndPtr          = NULL;
  CONST CHAR8  *BootPatchLevelStr = NULL;
  UINT32       BootPatchLevel     = 20241201;
  UINT8        DeviceBootLocked;
  UINT8        DeviceBootUnlocked;
  AvbIOResult  AvbResult = AVB_IO_RESULT_OK;
  UINT32       Idx       = 0;

  // avb.managed_verity_mode.verified_boot_key
  if (BootState != VERIFIED_BOOT_ORANGE_STATE) {
    Response = Sha256HashAll (gAvbKeyData.PubKey, gAvbKeyData.Len, AvbHash);
    if (Response == FALSE) {
      Status = EFI_NOT_READY;
      goto Exit;
    }
  }

  AvbResult = WritePersistentValue (Ops, ROT_VERIFIEDBOOT_KEY_NAME, SHA256_DIGEST_SIZE, AvbHash);
  if (AvbResult != AVB_IO_RESULT_OK) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to set ROT - %a\n", __FUNCTION__, ROT_VERIFIEDBOOT_KEY_NAME));
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }

  // avb.managed_verity_mode.serial
  // Check bootconfig first, if not found, call BootConfigAddSerialNumber()
  NodeOffset = -1;
  Status     = DeviceTreeGetNodeByPath ("/chosen", &NodeOffset);
  if (!EFI_ERROR (Status)) {
    Status = DeviceTreeGetNodeProperty (NodeOffset, "bootconfig", (CONST VOID **)&BootConfigStr, NULL);
    if (!EFI_ERROR (Status)) {
      SnStartPtr = AsciiStrStr (BootConfigStr, "serialno=");
    }
  }

  if (SnStartPtr != NULL) {
    SnStartPtr += AsciiStrLen ("serialno=");
    SnEndPtr    = AsciiStrStr (SnStartPtr, "\n");
    AsciiStrnCpyS (Sn, MAX_SN_LEN, SnStartPtr, SnEndPtr - SnStartPtr);
  } else {
    Status = BootConfigAddSerialNumber (NULL, Sn, MAX_SN_LEN);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Got %r trying to add serial number to BootConfigProtocol\n", __FUNCTION__, Status));
      goto Exit;
    }
  }

  AvbResult = WritePersistentValue (Ops, ROT_SERIALNO_NAME, AsciiStrLen (Sn), (UINT8 *)Sn);
  if (AvbResult != AVB_IO_RESULT_OK) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to set ROT - %a\n", __FUNCTION__, ROT_SERIALNO_NAME));
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }

  // avb.managed_verity_mode.device_boot_locked
  ReadIsDeviceUnlocked (Ops, (BOOLEAN *)&DeviceBootUnlocked);
  DeviceBootLocked = (DeviceBootUnlocked != 0) ? 0 : 1;
  AvbResult        = WritePersistentValue (Ops, ROT_DEVICE_BOOT_LOCKED_NAME, sizeof (DeviceBootLocked), &DeviceBootLocked);
  if (AvbResult != AVB_IO_RESULT_OK) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to set ROT - %a\n", __FUNCTION__, ROT_DEVICE_BOOT_LOCKED_NAME));
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }

  // avb.managed_verity_mode.verified_boot_state
  AvbResult = WritePersistentValue (Ops, ROT_VERIFIEDBOOT_STATE_NAME, sizeof (BootState), (UINT8 *)&BootState);
  if (AvbResult != AVB_IO_RESULT_OK) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to set ROT - %a\n", __FUNCTION__, ROT_VERIFIEDBOOT_STATE_NAME));
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }

  // avb.managed_verity_mode.boot_patchlevel
  for (Idx = 0; Idx < SlotData->num_vbmeta_images; Idx++) {
    AvbVBMetaData  *VbmetaImg = &(SlotData->vbmeta_images[Idx]);
    BootPatchLevelStr = avb_property_lookup (
                          VbmetaImg->vbmeta_data,
                          VbmetaImg->vbmeta_size,
                          PROP_BOOT_PATCHLEVEL_NAME,
                          0,
                          NULL
                          );
    if (BootPatchLevelStr != NULL) {
      BootPatchLevel = BootPatchLevelStrToL (BootPatchLevelStr);
      DEBUG ((DEBUG_INFO, "%a: BootPatchLevel found = %u\n", __FUNCTION__, BootPatchLevel));
      break;
    }
  }

  AvbResult = WritePersistentValue (Ops, ROT_BOOT_PATCHLEVEL_NAME, sizeof (BootPatchLevel), (UINT8 *)&BootPatchLevel);
  if (AvbResult != AVB_IO_RESULT_OK) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to set ROT - %a\n", __FUNCTION__, ROT_BOOT_PATCHLEVEL_NAME));
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }

  // avb.managed_verity_mode.vbmeta_digest
  avb_slot_verify_data_calculate_vbmeta_digest (
    SlotData,
    AVB_DIGEST_TYPE_SHA256,
    AvbHash
    );
  AvbResult = WritePersistentValue (Ops, ROT_VBMETA_DIGEST_NAME, SHA256_DIGEST_SIZE, AvbHash);
  if (AvbResult != AVB_IO_RESULT_OK) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to set ROT - %a\n", __FUNCTION__, ROT_VBMETA_DIGEST_NAME));
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }

Exit:
  return Status;
}

/**
  Add RPMB AVB info to DTB under /chosen/rpmb_dump node.

  Dumps avb_state, rollback_index, and vbmeta_digest into the
  device tree for diagnostic purposes. All values are read from
  RPMB tamper-resistant storage via OP-TEE.

  @retval EFI_SUCCESS           Info added to DTB successfully.
  @retval Others                Error occurred.

**/
EFI_STATUS
AddRpmbInfoToDtb (
  VOID
  )
{
  AVB_BOOT_STATE  BootState = mAvbBootState;
  EFI_STATUS      Status;
  VOID            *DeviceTree;
  INT32           ChosenNode;
  INT32           NodeOffset;
  BOOLEAN         DeviceUnlocked;
  CHAR8           StrBuf[512];
  CONST CHAR8     *LockState;
  CONST CHAR8     *EioState;
  UINT32          Idx;
  UINTN           Len;
  UINT8           VbmetaDigest[SHA256_DIGEST_SIZE];
  size_t          DigestBytesRead;
  INT32           FdtErr;
  uint64_t        RollbackIdx;
  AvbIOResult     AvbResult;

  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &DeviceTree);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Got %r trying to get dtb ptr\n", __FUNCTION__, Status));
    return Status;
  }

  ChosenNode = FdtPathOffset (DeviceTree, "/chosen");
  if (ChosenNode < 0) {
    DEBUG ((DEBUG_ERROR, "%a: /chosen node not found\n", __FUNCTION__));
    return EFI_NOT_FOUND;
  }

  NodeOffset = FdtPathOffset (DeviceTree, "/chosen/rpmb_dump");
  if (NodeOffset < 0) {
    NodeOffset = FdtAddSubnode (DeviceTree, ChosenNode, "rpmb_dump");
    if (NodeOffset < 0) {
      DEBUG ((DEBUG_ERROR, "%a: Failed to create /chosen/rpmb_dump subnode: %d\n", __FUNCTION__, NodeOffset));
      return EFI_DEVICE_ERROR;
    }
  }

  AvbResult = ReadIsDeviceUnlocked (NULL, &DeviceUnlocked);
  if (AvbResult != AVB_IO_RESULT_OK) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to read lock state: %d\n", __FUNCTION__, AvbResult));
    return EFI_DEVICE_ERROR;
  }

  LockState = DeviceUnlocked ? "unlocked" : "locked";
  EioState  = (BootState == VERIFIED_BOOT_RED_STATE_EIO) ? "eio_mode" : "normal_mode";
  AsciiSPrint (StrBuf, sizeof (StrBuf), "%a, %a", LockState, EioState);
  FdtErr = FdtSetPropString (DeviceTree, NodeOffset, "avb_state", StrBuf);
  if (FdtErr != 0) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to set avb_state: %d\n", __FUNCTION__, FdtErr));
    return EFI_DEVICE_ERROR;
  }

  StrBuf[0] = '\0';
  Len       = 0;
  for (Idx = 0; Idx < AVB_MAX_NUMBER_OF_ROLLBACK_INDEX_LOCATIONS; Idx++) {
    RollbackIdx = 0;
    AvbResult   = ReadRollbackIndex (NULL, (size_t)Idx, &RollbackIdx);
    if (AvbResult != AVB_IO_RESULT_OK) {
      RollbackIdx = 0;
    }

    Len += AsciiSPrint (
             StrBuf + Len,
             sizeof (StrBuf) - Len,
             "%lu ",
             RollbackIdx
             );
  }

  FdtErr = FdtSetPropString (DeviceTree, NodeOffset, "rollback_index", StrBuf);
  if (FdtErr != 0) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to set rollback_index: %d\n", __FUNCTION__, FdtErr));
    return EFI_DEVICE_ERROR;
  }

  DigestBytesRead = 0;
  AvbResult       = ReadPersistentValue (
                      NULL,
                      ROT_VBMETA_DIGEST_NAME,
                      sizeof (VbmetaDigest),
                      VbmetaDigest,
                      &DigestBytesRead
                      );
  if ((AvbResult == AVB_IO_RESULT_OK) && (DigestBytesRead > 0)) {
    StrBuf[0] = '\0';
    Len       = 0;
    for (Idx = 0; Idx < DigestBytesRead; Idx++) {
      Len += AsciiSPrint (
               StrBuf + Len,
               sizeof (StrBuf) - Len,
               "%02x",
               VbmetaDigest[Idx]
               );
    }

    FdtErr = FdtSetPropString (DeviceTree, NodeOffset, "vbmeta_digest", StrBuf);
    if (FdtErr != 0) {
      DEBUG ((DEBUG_ERROR, "%a: Failed to set vbmeta_digest: %d\n", __FUNCTION__, FdtErr));
      return EFI_DEVICE_ERROR;
    }
  } else {
    DEBUG ((DEBUG_ERROR, "%a: Failed to read vbmeta_digest from RPMB\n", __FUNCTION__));
  }

  DEBUG ((DEBUG_INFO, "%a: Updated rpmb_dump info to DTB\n", __FUNCTION__));

  return EFI_SUCCESS;
}

EFI_STATUS
AvbVerifyBoot (
  IN BOOLEAN     IsRecovery,
  IN EFI_HANDLE  ControllerHandle,
  OUT CHAR8      **AvbCmdline
  )
{
  NVIDIA_BOOTCONFIG_UPDATE_PROTOCOL  *BootConfigUpdate = NULL;
  AVB_BOOT_STATE                     BootState         = VERIFIED_BOOT_UNKNOWN_STATE;
  EFI_STATUS                         Status;
  AvbSlotVerifyData                  *SlotData     = NULL;
  CHAR8                              *BootStateStr = NULL;

  mControllerHandle = ControllerHandle;

  Status = AvbOpteeInterfaceInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a:Avb OP-TEE initialization failed with %r\n", __func__, Status));
    goto Exit;
  }

 #if FixedPcdGetBool (PcdAvbEnableFactoryUnlockReset)
  // Seed the AVB TA lock state from the factory scratch register if the TA
  // has none yet. Must run before AVB verify consults the TA via
  // ReadIsDeviceUnlocked; otherwise libavb would observe NO_SUCH_VALUE and
  // fall back to its conservative default.
  Status = AvbApplyFactoryUnlockReset ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Factory unlock-state seed failed with %r\n", __func__, Status));
    goto Exit;
  }

 #endif

  Status = VerifiedBootGetBootState (IsRecovery, &BootState, &SlotData);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a:Avb Verify Boot failed with %r\n", __func__, Status));
  }

  if ((SlotData != NULL) && (AvbCmdline != NULL)) {
    DEBUG ((DEBUG_ERROR, "Avb cmdline: %a\n", SlotData->cmdline));
    *AvbCmdline = SlotData->cmdline;
  }

 #if FixedPcdGetBool (PcdAvbEnableNctIntegrityHash)
  // Must run before AvbPassOpteeBootInfo / mAvbBootState /
  // AvbCommitRollbackIndexes / verifiedbootstate / AvbShowUi so a
  // RED demotion propagates to all of them. NCT integrity is
  // enforced regardless of lock state and regardless of the current
  // AVB result (ORANGE and RED_EIO get flattened to RED on mismatch);
  // any other error is best-effort and never escalated.
  Status = AvbSeedNctIntegrityHash ();
  if (Status == EFI_SECURITY_VIOLATION) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: NCT mismatch; forcing boot state %d -> RED\n",
      __FUNCTION__,
      BootState
      ));
    BootState = VERIFIED_BOOT_RED_STATE;
  } else if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: NCT integrity seed/verify returned %r (ignored)\n",
      __FUNCTION__,
      Status
      ));
  }

 #endif

  Status = AvbPassOpteeBootInfo (NULL, SlotData, BootState);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: %r to set ROT bootinfo to Optee\n", __FUNCTION__, Status));
    goto Exit;
  }

  mAvbBootState = BootState;

  // Deferred rollback-index commit: bump RPMB-stored rollback indexes only
  // after AVB verify passed AND the prior boot of this slot was marked
  // successful by Android (BCB SuccessfulBoot=1). See AvbCommitRollbackIndexes
  // for the full policy. The function self-logs every meaningful outcome and
  // only returns an error for true contract violations (e.g. NULL SlotData);
  // we log such errors here and continue, since a verified boot must not be
  // turned into a failure by the commit step. Status is intentionally
  // reassigned below by GetBootConfigUpdateProtocol().
  Status = AvbCommitRollbackIndexes (SlotData, BootState);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: rollback-index commit returned %r (ignored)\n",
      __FUNCTION__,
      Status
      ));
  }

  BootStateStr = (BootState == VERIFIED_BOOT_RED_STATE) ? "red" :
                 (BootState == VERIFIED_BOOT_RED_STATE_EIO) ? "red_eio" :
                 (BootState == VERIFIED_BOOT_YELLOW_STATE) ? "yellow" :
                 (BootState == VERIFIED_BOOT_GREEN_STATE) ? "green" :
                 (BootState == VERIFIED_BOOT_ORANGE_STATE) ? "orange" : "unknown";

  DEBUG ((DEBUG_ERROR, "%a: Android verifiedbootstate = %a\n", __FUNCTION__, BootStateStr));

  Status = GetBootConfigUpdateProtocol (&BootConfigUpdate);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: %r to get BootConfigUpdateProtocol\n", __FUNCTION__, Status));
    goto Exit;
  }

  Status = BootConfigUpdate->UpdateBootConfigs (BootConfigUpdate, "verifiedbootstate", BootStateStr);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: %r to update BootConfigUpdateProtocol\n", __FUNCTION__, Status));
  }

  AvbShowUi (BootState);

  // RED / RED_EIO must not hand off to the payload we just marked as
  // failed. Report the failure up to the LoadFile caller (boot
  // manager) via EFI_SECURITY_VIOLATION; A/B fallback is already
  // primed because AndroidBcbCheckAndUpdateRetryCount decremented
  // TriesRemaining before we ran.
  if ((BootState == VERIFIED_BOOT_RED_STATE) ||
      (BootState == VERIFIED_BOOT_RED_STATE_EIO))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: RED boot state (%d) - refusing to load kernel\n",
      __FUNCTION__,
      BootState
      ));
    Status = EFI_SECURITY_VIOLATION;
  }

Exit:
  return Status;
}
