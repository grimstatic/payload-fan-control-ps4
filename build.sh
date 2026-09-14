#!/bin/sh
set -eu

: "${PS4_PAYLOAD_SDK:?Set PS4_PAYLOAD_SDK, e.g. /opt/ps4-payload-sdk}"

make clean
make
file fan_control.elf

if command -v readelf >/dev/null 2>&1; then
    readelf -h fan_control.elf
fi
