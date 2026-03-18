#!/bin/bash
# install.sh — DKMS install helper for lenovo-legion-wmi-fan
set -euo pipefail

PACKAGE_NAME="lenovo-legion-wmi-fan"
PACKAGE_VERSION="0.1.0"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
DKMS_SRC="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root (sudo)." >&2
    exit 1
fi

echo "Installing ${PACKAGE_NAME} ${PACKAGE_VERSION} via DKMS..."

# Copy source to the DKMS tree
install -d "${DKMS_SRC}"
install -m 644 "${SRC_DIR}/lenovo-legion-wmi-fan.c" "${DKMS_SRC}/"
install -m 644 "${SRC_DIR}/Makefile"                "${DKMS_SRC}/"
install -m 644 "${SRC_DIR}/dkms.conf"               "${DKMS_SRC}/"

# Add, build, and install
dkms add     "${PACKAGE_NAME}/${PACKAGE_VERSION}"
dkms build   "${PACKAGE_NAME}/${PACKAGE_VERSION}"
dkms install "${PACKAGE_NAME}/${PACKAGE_VERSION}"

echo "Done.  Load the module with:  sudo modprobe lenovo-legion-wmi-fan"
