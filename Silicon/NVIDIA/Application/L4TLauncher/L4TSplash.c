/** @file
  Splash screen support for the L4T Launcher application.

  Clears the display to black and draws the embedded launcher logo so a
  splash is visible while boot processing (rootfs validation, DTB handling,
  etc.) runs before the kernel is started. The pattern follows the firmware
  Logo driver / AvbUiDxe: the BMP is embedded in this image as an HII
  resource section and fetched through the HII Image Ex protocol, which
  returns the bitmap already in display format (BGRX).

  Showing the splash must never block boot: if no GOP or HII support is
  available (e.g. headless/console-only boot) the function simply returns.

  SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiDatabase.h>
#include <Protocol/HiiImageEx.h>
#include <Protocol/HiiPackageList.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/ImageScaleLib.h>

//
// Logo size as a percentage of the screen height, in 10ths of a percent.
// Matches the firmware's default boot logo scaling (40%).
//
#define SPLASH_LOGO_SCREEN_RATIO  400

STATIC EFI_HII_IMAGE_EX_PROTOCOL  *mHiiImageEx;
STATIC EFI_HII_HANDLE             mHiiHandle;

/**
  Fill the entire screen with black.

  @param[in] Gop  Graphics Output Protocol instance.

  @retval EFI_STATUS  Result of the Blt operation.
**/
STATIC
EFI_STATUS
SplashClearScreen (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop
  )
{
  EFI_STATUS                     Status;
  UINTN                          ScreenWidth;
  UINTN                          ScreenHeight;
  UINTN                          BufferSize;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *BlackBuffer;

  ScreenWidth  = Gop->Mode->Info->HorizontalResolution;
  ScreenHeight = Gop->Mode->Info->VerticalResolution;
  BufferSize   = ScreenWidth * ScreenHeight * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);

  //
  // Some GOP implementations do not honor the EfiBltVideoToVideo constant-fill
  // idiom, so build an explicit black frame buffer and push it with
  // EfiBltBufferToVideo, which every compliant driver must implement.
  // All-zero pixels are black in any pixel format.
  //
  BlackBuffer = AllocateZeroPool (BufferSize);
  if (BlackBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = Gop->Blt (
              Gop,
              BlackBuffer,
              EfiBltBufferToVideo,
              0,
              0,
              0,
              0,
              ScreenWidth,
              ScreenHeight,
              ScreenWidth * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
              );

  FreePool (BlackBuffer);

  return Status;
}

/**
  Compare two device paths node by node starting at the given nodes.

  @param[in] Dp1  First device path (or node within it).
  @param[in] Dp2  Second device path (or node within it).

  @retval  TRUE   The remaining paths are identical, including end nodes.
  @retval  FALSE  They differ or have different lengths.
**/
STATIC
BOOLEAN
SameDevicePathFrom (
  IN EFI_DEVICE_PATH_PROTOCOL  *Dp1,
  IN EFI_DEVICE_PATH_PROTOCOL  *Dp2
  )
{
  while (!IsDevicePathEnd (Dp1) && !IsDevicePathEnd (Dp2)) {
    UINTN  Size1;
    UINTN  Size2;

    Size1 = Dp1->Length[0] | ((UINTN)Dp1->Length[1] << 8);
    Size2 = Dp2->Length[0] | ((UINTN)Dp2->Length[1] << 8);
    if ((Size1 != Size2) || (CompareMem (Dp1, Dp2, Size1) != 0)) {
      return FALSE;
    }

    Dp1 = NextDevicePathNode (Dp1);
    Dp2 = NextDevicePathNode (Dp2);
  }

  return IsDevicePathEnd (Dp1) && IsDevicePathEnd (Dp2);
}

/**
  Check whether a text console device path belongs to any Graphics Output
  device (exact match or suffix). The video text console installed by the
  graphics console driver reuses its parent GOP device's device path, so this
  identifies it without relying on private driver data.

  @param[in] TextDp  Device path of a handle that produces Simple Text Out.

  @retval  TRUE   TextDp matches a GOP device path.
  @retval  FALSE  No match (or no comparable GOP paths available).
**/
STATIC
BOOLEAN
IsVideoTextConsole (
  IN EFI_DEVICE_PATH_PROTOCOL  *TextDp
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE             *GopHandles;
  UINTN                  GopCount;
  UINTN                  Index;

  GopHandles = NULL;
  GopCount   = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  &GopCount,
                  &GopHandles
                  );
  if (EFI_ERROR (Status) || (GopCount == 0) || (GopHandles == NULL)) {
    return FALSE;
  }

  for (Index = 0; Index < GopCount; Index++) {
    EFI_DEVICE_PATH_PROTOCOL  *GopDp;
    EFI_DEVICE_PATH_PROTOCOL  *Node;

    Status = gBS->HandleProtocol (GopHandles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&GopDp);
    if (EFI_ERROR (Status) || (GopDp == NULL)) {
      continue;
    }

    //
    // Match on the full path or any suffix of it.
    //
    for (Node = GopDp; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
      if (SameDevicePathFrom (Node, TextDp)) {
        FreePool (GopHandles);
        return TRUE;
      }
    }
  }

  FreePool (GopHandles);
  return FALSE;
}

/**
  Remove video-based text consoles from ConOut so that debug output no longer
  overwrites the splash screen; serial (and other non-GOP) consoles are kept.

  Every handle producing Simple Text Out is located and checked: a console is
  considered video if it has GOP installed on the same handle, or if its device
  path matches a GOP device's path. A video console is dropped by uninstalling
  its Simple Text Out protocol; the console splitter removes it from ConOut
  through its protocol notify, so no console re-installation is needed here.

  If nothing can be identified as a video console the setup is left untouched,
  so output is never lost entirely. Note: this affects the rest of the boot
  session if control ever returns to BDS (e.g. failed boot falling back to
  Setup).
**/
STATIC
EFI_STATUS
KeepOnlySerialConsole (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;

  Handles = NULL;
  Count   = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleTextOutProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (Count == 0) || (Handles == NULL)) {
    // Best effort: leave the console setup alone.
    return EFI_SUCCESS;
  }

  for (Index = 0; Index < Count; Index++) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *TextOut;
    EFI_DEVICE_PATH_PROTOCOL         *DevicePath;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleTextOutProtocolGuid, (VOID **)&TextOut);
    if (EFI_ERROR (Status) || (TextOut == NULL)) {
      continue;
    }

    //
    // A text console installed directly on a GOP device is a video console.
    //
    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiGraphicsOutputProtocolGuid, NULL))) {
      DEBUG ((DEBUG_INFO, "%a: dropping video text console from ConOut\r\n", __FUNCTION__));
      gBS->UninstallProtocolInterface (Handles[Index], &gEfiSimpleTextOutProtocolGuid, TextOut);
      continue;
    }

    //
    // Handles without a device path (e.g. the console splitter's own virtual
    // handle) cannot be identified - keep them.
    //
    Status = gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&DevicePath);
    if (EFI_ERROR (Status) || (DevicePath == NULL)) {
      continue;
    }

    if (!IsVideoTextConsole (DevicePath)) {
      continue;
    }

    DEBUG ((DEBUG_INFO, "%a: dropping video text console from ConOut\r\n", __FUNCTION__));
    gBS->UninstallProtocolInterface (Handles[Index], &gEfiSimpleTextOutProtocolGuid, TextOut);
  }

  FreePool (Handles);
  return EFI_SUCCESS;
}

/**
  Show the splash screen: clear the display to black and draw the embedded
  logo centered on the screen.

  This function is best-effort by design: any failure (no GOP, no HII
  database, missing image, ...) is logged and results in EFI_SUCCESS so the
  boot flow continues unimpeded.

  @param[in] ImageHandle  The firmware allocated handle for this image.
                          Used to open the embedded HII package list.

  @retval EFI_SUCCESS  Always, unless a parameter is invalid.
**/
EFI_STATUS
EFIAPI
ShowL4TSplashScreen (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                     Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop;
  EFI_HII_DATABASE_PROTOCOL      *HiiDatabase;
  EFI_HII_PACKAGE_LIST_HEADER    *PackageList;
  EFI_IMAGE_INPUT                LogoImage;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *ScaledLogo;
  UINTN                          ScreenWidth;
  UINTN                          ScreenHeight;
  UINTN                          TargetHeight;
  UINTN                          TargetWidth;
  UINTN                          DestX;
  UINTN                          DestY;

  if (ImageHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // No display - nothing to show, boot continues normally.
  //
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: no GOP available, skipping splash\r\n", __FUNCTION__));
    return EFI_SUCCESS;
  }

  ScreenWidth  = Gop->Mode->Info->HorizontalResolution;
  ScreenHeight = Gop->Mode->Info->VerticalResolution;

  //
  // Clear the screen to black first so any firmware boot logo or text is gone.
  //
  Status = SplashClearScreen (Gop);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: failed to clear screen: %r\r\n", __FUNCTION__, Status));
    return EFI_SUCCESS;
  }

  //
  // Route further console output to serial only so logs do not trash the splash.
  //
  KeepOnlySerialConsole ();

  //
  // Locate the HII protocols.
  //
  Status = gBS->LocateProtocol (&gEfiHiiDatabaseProtocolGuid, NULL, (VOID **)&HiiDatabase);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: no HII database, skipping logo: %r\r\n", __FUNCTION__, Status));
    return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (&gEfiHiiImageExProtocolGuid, NULL, (VOID **)&mHiiImageEx);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: no HII Image Ex protocol, skipping logo: %r\r\n", __FUNCTION__, Status));
    return EFI_SUCCESS;
  }

  //
  // Retrieve the HII package list embedded in this image's PE/COFF resource
  // section and publish it to the HII database.
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
    DEBUG ((DEBUG_WARN, "%a: HII package list not found in PE/COFF resource section: %r\r\n", __FUNCTION__, Status));
    return EFI_SUCCESS;
  }

  Status = HiiDatabase->NewPackageList (HiiDatabase, PackageList, NULL, &mHiiHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: failed to publish HII package list: %r\r\n", __FUNCTION__, Status));
    return EFI_SUCCESS;
  }

  //
  // Get the logo bitmap in display format.
  //
  ZeroMem (&LogoImage, sizeof (LogoImage));
  Status = mHiiImageEx->GetImageEx (mHiiImageEx, mHiiHandle, IMAGE_TOKEN (IMG_L4T_LOGO), &LogoImage);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: failed to get logo image: %r\r\n", __FUNCTION__, Status));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_INFO, "%a: logo %ux%u, screen %ux%u\r\n", __FUNCTION__, LogoImage.Width, LogoImage.Height, ScreenWidth, ScreenHeight));

  //
  // Scale the logo to SPLASH_LOGO_SCREEN_RATIO percent of the screen height,
  // preserving aspect ratio and clamping to the screen width.
  //
  TargetHeight = (ScreenHeight * SPLASH_LOGO_SCREEN_RATIO) / 1000;
  TargetWidth  = (LogoImage.Width * TargetHeight) / LogoImage.Height;
  if (TargetWidth > ScreenWidth) {
    TargetWidth  = ScreenWidth;
    TargetHeight = (LogoImage.Height * TargetWidth) / LogoImage.Width;
  }

  Status = ImageScale (
             LogoImage.Bitmap,
             (UINTN)LogoImage.Width,
             (UINTN)LogoImage.Height,
             TargetWidth,
             TargetHeight,
             &ScaledLogo
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: failed to scale logo: %r\r\n", __FUNCTION__, Status));
    return EFI_SUCCESS;
  }

  //
  // Center the logo on screen and Blt it.
  //
  DestX = (ScreenWidth - TargetWidth) / 2;
  DestY = (ScreenHeight - TargetHeight) / 2;

  Status = Gop->Blt (
                Gop,
                ScaledLogo,
                EfiBltBufferToVideo,
                0,
                0,
                DestX,
                DestY,
                TargetWidth,
                TargetHeight,
                TargetWidth * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
                );

  FreePool (ScaledLogo);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: failed to draw logo: %r\r\n", __FUNCTION__, Status));
  } else {
    DEBUG ((DEBUG_INFO, "%a: splash shown at %ux%u + offset (%u,%u)\r\n", __FUNCTION__, TargetWidth, TargetHeight, DestX, DestY));
  }

  return EFI_SUCCESS;
}
