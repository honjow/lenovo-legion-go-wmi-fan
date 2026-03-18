#!/bin/bash
# uninstall.sh — DKMS uninstall helper for lenovo-legion-wmi-fan
set -euo pipefail

PACKAGE_NAME="lenovo-legion-wmi-fan"
PACKAGE_VERSION="0.1.0"
DKMS_SRC="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root (sudo)." >&2
    exit 1
fi

echo "Uninstalling ${PACKAGE_NAME} ${PACKAGE_VERSION}..."

# Unload the module if it is loaded
if lsmod | grep -q "lenovo_legion_wmi_fan"; then
    modprobe -r lenovo-legion-wmi-fan || true
fi

# Remove via DKMS
dkms remove "${PACKAGE_NAME}/${PACKAGE_VERSION}" --all || true

# Remove source from DKMS tree
rm -rf "${DKMS_SRC}"

echo "Done."
