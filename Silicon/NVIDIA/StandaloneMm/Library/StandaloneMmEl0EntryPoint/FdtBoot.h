/** @file
  Internal header for FdtBoot.

  SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef FDT_BOOT_H_
#define FDT_BOOT_H_

#include <PiMm.h>

#include <PiPei.h>

/**
  Use the boot information passed by privileged firmware to populate a HOB list
  suitable for consumption by the MM Core and drivers.

  @param  [in, out] CpuDriverEntryPoint   Address of MM CPU driver entrypoint
  @param  [in]      PayloadBootInfo       Boot information passed by privileged
                                          firmware

**/
VOID *
CreateHobListFromBootInfo (
  IN UINT64  TotalSPMemorySize,
  IN VOID    *DTBAddress
  );

/*
 * From the manifest load-address and entrypoint-offset, find the base address of the SP code.
 *
 * @param  [in] DtbAddress           Address of the partition manifest.
 */
UINT64
GetSpImageBase (
  IN VOID  *DtbAddress
  );

#endif
