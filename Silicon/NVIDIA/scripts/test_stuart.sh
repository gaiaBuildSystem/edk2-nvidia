#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2021-2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# SPDX-License-Identifier: BSD-2-Clause-Patent



###############################################################################
# Run an NVIDIA host-based tests in the current workspace using the stuart
# build system.  Assumes the workspace has been created using submodules and
# prepared using prepare_stuart.sh

# This script, along with others in this directory, serves as a working example
# of building NVIDIA UEFI images.


PACKAGE=$1
PLATFORM_BUILD=$2
[ -z "$PLATFORM_BUILD" ] && { echo "$0 <package> <platform_build.py>"; exit 1; }


# Set common environment variables.
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
. ${SCRIPT_DIR}/setenv_stuart.sh


_msg "Activating Python virtual environment."
. venv/bin/activate

STUART_TEST_OPTIONS=${STUART_TEST_OPTIONS:---verbose}

# Host-based tests build native executables, so the edk2 architecture must
# match the host.  Honor HOSTAPP_ARCH if set, otherwise derive it from the
# host machine type.
if [ -z "${HOSTAPP_ARCH}" ]; then
  case "$(uname -m)" in
    aarch64 | arm64) HOSTAPP_ARCH=AARCH64 ;;
    x86_64 | amd64)  HOSTAPP_ARCH=X64 ;;
    *) _die 4 "Unsupported host machine type for host-based tests: $(uname -m)" ;;
  esac
fi
_msg "Building host-based tests for ${HOSTAPP_ARCH}."

_msg "Testing ($PLATFORM_BUILD)."
stuart_ci_build -t NOOPT -a ${HOSTAPP_ARCH} -p ${PACKAGE} -c ${PLATFORM_BUILD} ${STUART_TEST_OPTIONS}
