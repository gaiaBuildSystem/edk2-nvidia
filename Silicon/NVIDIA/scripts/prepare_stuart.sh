#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# SPDX-License-Identifier: BSD-2-Clause-Patent


###############################################################################
# Prepares the current workspace to build NVIDIA UEFI images using the stuart
# build system.

# This script, along with others in this directory, serves as a working example
# of building NVIDIA UEFI images.


PLATFORM_BUILD=$1
[ -z "${PLATFORM_BUILD}" ] && { echo "$0 <platform_build.py>"; exit 1; }


# Set common environment variables.
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
. ${SCRIPT_DIR}/setenv_stuart.sh

if [[ -z "${UEFI_SKIP_VENV}" ]]; then
  # Create a python virtual env, if we haven't already.
  find_venv_activate
  if [ ! -e ${VENV_ACTIVATE} ]; then
    _msg "Creating Python virtual environment in `pwd`/venv..."
    # Use python3 -m venv instead of virtualenv for better compatibility
    python3 -m venv venv
    find_venv_activate
    . ${VENV_ACTIVATE}
    _msg "Installing required Python packages..."
    # Upgrade pip first to ensure we have latest version
    pip install --upgrade pip
    # Force installation in the virtual environment to avoid system conflicts
    pip install --force-reinstall ${UEFI_PIP_INSTALL_ARGS} --upgrade -r edk2/pip-requirements.txt
    pip install --force-reinstall ${UEFI_PIP_INSTALL_ARGS} --upgrade kconfiglib
  else
    _msg "Activating Python virtual environment."
    find_venv_activate
    . ${VENV_ACTIVATE}
  fi
fi

if [[ -z "${UEFI_SKIP_UPDATE}" ]]; then
  _msg "Updating build environment (${PLATFORM_BUILD})."
  # Requires mono to be installed.
  stuart_update ${STUART_UPDATE_OPTIONS} -c ${PLATFORM_BUILD}
fi

_msg "Building basetools."
python edk2/BaseTools/Edk2ToolsBuild.py -t GCC
