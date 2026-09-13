#!/bin/sh

# SPDX-License-Identifier: MPL-2.0

set -e

# Keep this gate usable both from the regression dispatcher and when invoked
# directly by a QEMU network check.
cd "$(dirname "$0")"

./ipv6_dual_stack
./ipv6_udp
./ipv6_dual_stack_udp
