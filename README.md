# OpenWrt for the VSOL HG325AX30 (Realtek RTL9607DQ)

OpenWrt with Linux 6.18 for the **VSOL HG325AX30**, a GPON router ONU with Wi-Fi 6,
built on the Realtek RTL9607DQ "Taurus" (quad Cortex-A55). It replaces the vendor
firmware and adds GPON with a userspace OMCI daemon, four Ethernet ports (one at
2.5 Gbit/s), dual-band Wi-Fi 6 with open-source drivers, an FXS phone port, and
hardware NAT offload, managed from LuCI. It has been run on GPON with an Airtel Fiber
OLT.

Unlike a bridge-only ONU, this is a **router**: it does PPPoE/IPoE, NAT, DHCP, IPv6
prefix delegation, firewalling and Wi-Fi itself.

**Before you start:** installing replaces the stock image in slot A and creates new
UBI volumes. Stock stays in slot B and is the way back. Take a full flash backup
first, and read [Back to stock](#back-to-stock) and [Recovery](#recovery). The
backup contains your ISP credentials and factory calibration: keep it private.

## Supported hardware

| | VSOL HG325AX30 |
| --- | --- |
| SoC | Realtek RTL9607DQ "Taurus", quad Cortex-A55 |
| Memory | 512 MiB DDR3, 128 MiB SPI NAND (Fudan FM25S01A) |
| PON | GPON, Semtech GN25L95 optical module, calibration from the factory data |
| Ethernet | 3 Gigabit ports (internal PHYs), 1 port at 2.5 Gbit/s (Realtek RTL8221B) |
| Wi-Fi | 2.4 GHz RTL8192XB and 5 GHz RTL8852CE, both on PCIe, Wi-Fi 6 |
| Voice | One FXS port (MaxLinear DXS SLIC over the SoC SSP and SPI) |
| USB | One USB 3.0 port |
| Serial | UART, 115200 8N1 |

## Contents

- [Status](#status) and [Features](#features)
- [How it works](#how-it-works)
- [Stock firmware compared](#stock-firmware-compared) and [Measured performance](#measured-performance)
- [Flash layout](#flash-layout) and [Installation](#installation)
- [Configuration](#configuration), [Updates](#updates), [Back to stock](#back-to-stock) and [Recovery](#recovery)
- [Diagnostics](#diagnostics) and [Building](#building)
- [Credits and license](#credits-and-license)

## Status

### Working

| Feature | Notes |
| --- | --- |
| GPON registration, OMCI, recovery after fibre outages | Tested on Airtel Fiber; `omcid` runs in userspace and is restartable |
| Ethernet lan1 to lan4 | lan4 runs at 2.5G, 1G, 100M and 10M; LAN to LAN switching in hardware |
| Wi-Fi 6, 2.4 GHz and 5 GHz | 5 GHz at 20, 40, 80 and 160 MHz; several SSIDs per radio |
| PPPoE and IPoE routing, IPv6 prefix delegation | Several prefixes can be requested (`reqprefix` list) |
| Hardware NAT offload for IPv4 | Wired clients, both directions, line rate with the CPU idle |
| FXS phone port | uvoice SIP stack, two-way audio, ring and hook detection, line test |
| USB 3.0 storage and tethering drivers | Storage tested; tethering drivers are in the image |
| LEDs, buttons, temperature sensors | PON, LOS, Internet, LAN, phone and USB LEDs follow their state |
| LuCI, `sysupgrade`, statistics | Status pages for the PON and voice, collectd graphs |
| Install to flash and `sysupgrade` | OpenWrt in image A, stock kept in image B |

### Implemented, needs testing

| Feature | What's missing |
| --- | --- |
| IPv6 hardware offload | Upstream flows are installed; the downstream direction and the hash for IPv6 keys are not proven on a wired client |
| Up to 5 APs per radio | Two per radio are tested; three or more are not |
| TR-069 client (`cwmpd`) | Provisioning code is in, not tested against an ACS |
| EPON | The EPON code is carried in `xpon-agent`; not tested on this board |

### Known limits

| Limit | Why |
| --- | --- |
| Wi-Fi clients are not offloaded downstream | The hardware table only knows wired LAN ports, so those flows stay on the CPU |
| Routed Wi-Fi traffic runs on the CPU | About 300 Mbit/s today; see [Measured performance](#measured-performance) |
| GPON identity is configuration, not factory data | Serial and password are seeded from the stock configuration on the first boot |

## Features

- GPON with `omcid` in userspace: MIB, alarms, performance data, OLT
  interoperability profiles, and re-ranging after a fibre outage without a reboot.
- Four LAN ports as DSA switch ports, with LAN to LAN known unicast switched in
  hardware and VLAN-aware bridges.
- Hardware flow offload for routed IPv4 NAT over PPPoE and IPoE, through the
  Cortina packet engine and nftables flowtables.
- Dual-band Wi-Fi 6 with open-source `rtw89` drivers: the RTL8852CE through
  upstream code with board fixes, and the RTL8192XB as our own driver port.
- FXS telephony with `uvoice` and `udsp`, and a LuCI page for registration,
  call quality and line tests.
- Per-board factory data in a raw flash partition (`static_conf`) that survives a
  broken UBI volume.
- LuCI with PON status, OMCI MIB views, voice, statistics and a package manager.

## How it works

The packet engine exposes one conduit, `eth0` (NI port 5, 1 Gbit/s fixed link).
DSA turns it into `lan1` to `lan4`; the GPON MAC appears as `pon0`, whose link is
up while the ONU is registered. Bridges and routing are Linux's; the packet
engine's flow table (`cortina-ppe`) forwards installed NAT flows in hardware.

The GPON MAC, optics and OMCI backend live in the kernel; the OMCI responder
(`omcid`), identity and configuration live in userspace and talk to the kernel
over the xPON generic netlink family. Wi-Fi uses `mac80211` with `rtw89`.
The FXS port uses an ASoC SSP driver, a SLIC SPI driver and the `udsp-slic-dxs`
plug-in.

## Stock firmware compared

The stock firmware is VSOL's V1.0.00 (builds of August 2026) on the same hardware.
"Not measured" means this project hasn't examined that behaviour on stock.

### Platform

| | Stock firmware | This firmware |
| --- | --- | --- |
| Base system | Closed vendor build, vendor kernel and web interface | OpenWrt, Linux 6.18, musl, LuCI |
| Source | Vendor drivers are binary or vendor GPL drops | All drivers and daemons are source |
| Management | Vendor web interface, TR-069 | LuCI, ubus, SSH, `cwmpd` for TR-069 |
| Updates | Vendor images in slot A or B | `sysupgrade` from the shell or LuCI, settings kept |
| Packages | Fixed | `apk` package manager |
| Flash | UBI with two image slots | Slot A for OpenWrt, slot B left for stock |

### PON stack

| | Stock firmware | This firmware |
| --- | --- | --- |
| GPON activation | Closed vendor driver | `cortina-gpon` with a readable event log |
| OMCI | Closed vendor responder | `omcid`, inspected with `omcictl` and LuCI |
| OLT interoperability | Not shown | Auto detect, Nokia/ALCL, Huawei, ZTE, FiberHome, Dasan, Genexis, generic |
| Identity | Serial and password from its configuration | The same, as editable configuration |
| Optics | GN25L95 with per-unit calibration | The same calibration, with power, bias and temperature in the logs |
| Dying gasp | Present | Armed, as stock |

### Datapath

| | Stock firmware | This firmware |
| --- | --- | --- |
| Hardware NAT | IPv4 TCP flows installed after 10 hits; IPv6 stays in software | IPv4 NAT flows (TCP and UDP) installed at once; IPv6 offload under test |
| LAN to LAN | Not measured | Known unicast in hardware, misses go to the CPU |
| Ethernet | 2.5G port supported | 2.5G, 1G, 100M and 10M tested on lan4 |
| Wi-Fi flows | Software | Software |

### Wi-Fi

| | Stock firmware | This firmware |
| --- | --- | --- |
| Drivers | Vendor `rtk_wifi6` | `rtw89` family, open source |
| Transmit power | Louder: power limits are disabled in the firmware | Regulatory tables by default; `disable_txpwr_lmt` is set in the router image |
| 5 GHz bandwidths | Up to 160 MHz | 20, 40, 80 and 160 MHz |
| SSIDs | Vendor interface | Several per radio, assigned to any network in LuCI |

### Voice and diagnostics

| | Stock firmware | This firmware |
| --- | --- | --- |
| SIP | Vendor voice stack | `uvoice` with `udsp`, open source |
| Line test | Vendor test | Capacitance and loop tests, call quality in LuCI |
| Tools | Vendor CLI | `omcictl`, `xpon-report`, `logread`, collectd graphs, debugfs |

## Measured performance

| Test | Result |
| --- | --- |
| GPON, Airtel Fiber | O5 about 32 s after boot, rx -20.8 to -25.5 dBm, downstream FEC on |
| lan3, 1 Gbit/s | 943 Mbit/s in, about 790 Mbit/s out with no retransmits |
| lan4, 2.5 Gbit/s | About 1.05 to 1.1 Gbit/s both ways |
| Jumbo frames, 9000 bytes | 990 Mbit/s in, 952 Mbit/s out |
| Routed IPv4 NAT, hardware path | Plan speed with the CPU about 98% idle |
| 5 GHz, HE160 | About 650 Mbit/s up and 740 Mbit/s down |
| Wi-Fi through the CPU | About 300 Mbit/s, limited by the software path |

Numbers come from iperf3 and ISP speedtests on one unit and depend on the line.

## Flash layout

The SPI NAND holds a boot partition, two U-Boot environments, a small raw
factory partition and one UBI device.

| Area | Purpose |
| --- | --- |
| `boot` (0x0, 2 MiB) | Vendor U-Boot (not replaced) |
| `env`, `env2` | U-Boot environment, including the A/B switches (`sw_commit`, `sw_active`, `sw_tryactive`) |
| `static_conf` (0x300000, 256 KiB) | Factory data: MACs, GPON serial, optics calibration, a copy of stock `ubi_Config` |
| `ubi_device` | UBI: `ubi_DTB0`, `ubi_k0` (OpenWrt FIT, 6 MiB), `rootfs` (squashfs), `rootfs_data` (UBIFS overlay), and the untouched stock volumes |

OpenWrt lives in slot A. The stock volumes of slot B, `ubi_Config`,
`ubi_framework1/2` and `ubi_apps` are never written. U-Boot reads a fixed 6 MiB
from `ubi_k0`, so that volume keeps its size.

## Installation

### 1. Requirements

- A USB serial adapter (3.3 V) on the console, 115200 8N1.
- A PC with a TFTP server on `192.168.1.100/24`, connected to **lan3**. The vendor
  U-Boot only uses lan3 for TFTP.
- The initramfs image and the sysupgrade image from a build (see [Building](#building)):
  `openwrt-realtek-taurus-rtl9607dq-vsol_hg325ax30-initramfs-kernel.bin` and
  `openwrt-realtek-taurus-rtl9607dq-vsol_hg325ax30-squashfs-sysupgrade.bin`.
- Fibre unplugged until the install is done.

### 2. Back up the whole flash

Boot the initramfs from RAM (next step) and copy every MTD device to the PC, for
example with `tftp -p -l /dev/mtdN -r mtdN.bin 192.168.1.100` for each entry in
`/proc/mtd` (the TFTP server must accept uploads). Check the copies, and keep
them private: they contain `ubi_Config` with your ISP credentials.

### 3. Boot from RAM

Nothing is written to flash in this step; a power cycle returns to stock.

1. Power off fully and wait 5 seconds (a power cut is what resets the 2.5G PHY).
2. Power on and press **Ctrl+A** on the serial console to stop in U-Boot.
   `ipaddr` is `192.168.1.1` and `serverip` is `192.168.1.100` in its environment.
3. Load and boot the image. The load address `0x5000000` is required.

```text
tftp 5000000 hg325ax30-initramfs.bin
```

```text
bootm 5000000
```

The initramfs boots to a root shell on the console, and SSH works as
`root@192.168.1.1`. If the boot stops at `smp: Bringing up secondary CPUs`,
power off fully and try again.

### 4. Install to flash

From the RAM image, fetch the sysupgrade file, check its checksum and flash it:

```bash
cd /tmp && tftp -g -r hg325ax30-sysupgrade.bin 192.168.1.100
sha256sum hg325ax30-sysupgrade.bin
sysupgrade -n -v /tmp/hg325ax30-sysupgrade.bin
```

The first flash removes stock's `ubi_r0`, writes `ubi_k0` and creates `rootfs` and
`rootfs_data`, then sets `sw_commit=0`, `sw_active=0` and `sw_tryactive=2`.
The PON LOS LED blinks while the image is written. Do not cut power.

### 5. First boot

The board comes up at `192.168.1.1`. Set a root password, then connect the fibre.
The first boot seeds the GPON serial and password from the stock configuration
kept in `static_conf`.

## Configuration

### Network and IPv6

The default setup has `br-lan` for the LAN ports, `pon0` and the PPPoE or IPoE
interface for the WAN. IPv6 prefix delegation can request more than one prefix:

```bash
uci add_list network.wan6.reqprefix='64'
uci add_list network.wan6.reqprefix='64'
```

Give each downstream interface one of them with `ip6assign` and `ip6class`.

### Wi-Fi

Both radios are off by default, like upstream devices. Enable them and add SSIDs
in LuCI or with `uci`. Up to five APs per radio are allowed; two per radio are
tested. The router image sets `disable_txpwr_lmt=1`: if you need the regulatory
power limits, remove it in `/etc/modules.conf` and set your country.

### Offload

```bash
uci set firewall.@defaults[0].flow_offloading='1'
uci set firewall.@defaults[0].flow_offloading_hw='1'
uci commit firewall && fw4 reload
```

Wired clients are offloaded. Wi-Fi clients' downstream flows are refused by the
hardware and stay in software.

### Voice

The FXS port is provisioned through `uvoice` (LuCI, Telephony). The DXS firmware
and calibration files are copied from the stock image on the first boot and are
not shipped here.

### GPON identity

`omci.main.serial` and `omci.main.password` set the ONU identity; edit them in
LuCI or `uci`.

## Updates

```bash
sysupgrade -v /tmp/hg325ax30-sysupgrade.bin
```

Settings are kept. LuCI's Flash Images page does the same.

## Back to stock

Stock still lives in slot B. In U-Boot:

```text
setenv sw_commit 1; saveenv; reset
```

To return to OpenWrt, set `sw_commit 0` the same way. This path has not been tested separately from the installer.

## Recovery

- **Does not boot:** use the serial console, stop in U-Boot with Ctrl+A, switch the
  slot as above, or load the initramfs image over TFTP on lan3.
- **Lost factory data:** restore `static_conf` from your backup.
- **Wi-Fi or LAN stuck at boot:** power off fully (not a warm reboot) and wait
  5 seconds.
- **A programmer is the last resort.** The NAND is a WSON8 package, so common
  SOIC8 clips do not fit.

## Diagnostics

```bash
dmesg | grep -iE 'gpon|rtw89|cortina'      # PON, Wi-Fi and packet engine
omcictl                                     # OMCI state
logread | grep -i hostapd                   # Wi-Fi
cat /sys/kernel/debug/cortina-ni/ppe_entries   # offloaded flows
ethtool -S eth0 | grep l3fe                # offload counters
```

`xpon-report` collects one file with identifiers masked for bug reports.

## Building

```bash
git clone <this repository> openwrt-hg325ax30 && cd openwrt-hg325ax30
./scripts/feeds update -a && ./scripts/feeds install -a
cp diffconfig/vsol-hg325ax30.diffconfig .config && make defconfig   # the config of this board
make -j$(nproc)
```

The images are in `bin/targets/realtek-taurus/rtl9607dq/`. Build in a clean
environment: do not export `STAGING_DIR` or edit `PATH`.

## Credits and license

Based on OpenWrt, Linux and LuCI.

**Cortina-Access.** The Cortina network engine, GPON and SoC peripheral
drivers trace back to the GPL-2.0 sources released by Cortina-Access Limited
(the `aal_*` datapath code and the CA77xx peripheral drivers).

**BRULE Herman** (https://github.com/alphaonex86/openwrt) ported them to a Linux 6.18
OpenWrt target for the RTL9607F, the sibling of this SoC: the Cortina NI
Ethernet driver, the GPON MAC, BOSA and I2C code, the hardware flow offload,
the GPIO, UART, QSPI and PCIe drivers, the ethtool support and the GPON
protocol core. His commit history describes a bring-up reverse-engineered from
the stock firmware's behaviour and its `ca-ne.ko` module, and records that the
work was done with Claude and published under the GPL.

**AK Sharma** (https://github.com/AKoo7/openwrt/tree/aot5221zy) wrote the Cortina
DSA tag and the NI DSA switch driver for the Zyxel AOT-5221ZY, which this port
uses.

Other sources: `uvoice` and `udsp` come from `cmonroe/uvoice`; the
`xpon-agent` and `luci-app-xpon` packages descend from the Airoha GPON OMCI
packages by Matheus Sampaio Queiroga; Wi-Fi uses the upstream `rtw89` driver
and `linux-firmware`, and the RTL8192XB driver is our own port, written from
the upstream code and Realtek's GPL release.

Original authorship and file-level license notices are retained.

OpenWrt is licensed under GPL-2.0; component licenses remain applicable.
