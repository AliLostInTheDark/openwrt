#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The backend checks and the public and uAPI headers are shared with the
# kernel. internal.h is not: the kernel keeps only the configuration store.
set -eu
cd "$(dirname "$0")"
# Compare past a leading copyright block, which some targets add.
body() {
    awk 'NR == 2 && $0 == "/*" { skip = 1 } !skip { print } skip && $0 == "" { skip = 0 }' "$1"
}

same() {
    body "$1" | cmp -s - "$2" || body "$2" | cmp - "$1"
}

for target in realtek-luna/files-6.18 realtek-taurus/files-6.18; do
    kernel=../../../../../target/linux/$target
    [ -d "$kernel/net/xpon/omci" ] || continue
    same ../src/backend.c "$kernel/net/xpon/omci/backend.c"
    same ../src/device.h "$kernel/include/net/xpon/omci.h"
    same ../../libomci-protocol/src/include/linux/omci-backend.h "$kernel/include/uapi/linux/omci-backend.h"
    same ../../libomci-protocol/src/include/linux/omci.h "$kernel/include/uapi/linux/omci.h"
done
kernel=../../../../../target/linux/realtek-luna/files-6.18
for copy in libomci-protocol omci-agent omci-monitor; do
    cmp "../../$copy/src/include/linux/omci.h" "$kernel/include/uapi/linux/omci.h"
done
