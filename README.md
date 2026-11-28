# OpenWrt for the VSOL HG325AX30 (Realtek RTL9607DQ)

OpenWrt with Linux 6.18 for the **VSOL HG325AX30**, a GPON router ONU with Wi-Fi 6,
built on the Realtek RTL9607DQ "Taurus" (quad Cortex-A55). It replaces the vendor
firmware and adds GPON with a userspace OMCI daemon, four Ethernet ports (one at
2.5 Gbit/s), dual-band Wi-Fi 6 with open-source drivers, an FXS phone port, and
hardware NAT offload, managed from LuCI. It has been run on GPON with an Airtel Fiber
OLT.

Unlike a bridge-only ONU, this is a **router**: it does PPPoE/IPoE, NAT, DHCP, IPv6
prefix delegation, firewalling and Wi-Fi itself.

The boot chain is open too, apart from the mask ROM (BL1) and BL2, which holds
the closed DDR3 training: BL31 is mainline Trusted Firmware-A v2.14 and BL33 is
mainline U-Boot 2026.10.

**Before you start:** installing removes the stock firmware from the flash. The
only way back is a full flash backup, so take one first, and read
[Back to stock](#back-to-stock) and [Recovery](#recovery). The backup contains
your ISP credentials and factory calibration: keep it private.

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
- [Flash layout](#flash-layout) and [Installation](#installation), with the
  [open bootloaders](#6-install-the-open-bootloaders)
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
| Install to flash and `sysupgrade` | One OpenWrt image; the stock volumes are removed and their space goes to `rootfs_data` |
| TF-A v2.14 BL31 and U-Boot 2026.10 | All four cores, `reboot`, TFTP on lan1 to lan4, boot counter after a sysupgrade |
| Bootloader fallback | A broken `fip0` boots the vendor bootloaders in `fip1` within 23 s |

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
| BL2 is the vendor's | It holds the DDR3 training, for which there is no source |
| The FXS firmware is not in the repository | The vendor files cannot be redistributed; the build takes them from your own unit |

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
- Open BL31 and BL33: TF-A v2.14 with PSCI for the four cores, and U-Boot 2026.10
  with its environment in UBI, TFTP on every LAN port, and a boot counter that
  stops at the prompt when a new image fails three times.
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

At power-on the mask ROM (BL1) starts the vendor BL2, which trains the DRAM and
loads BL31 from the active FIP, `fip0`. BL2 loads nothing else, so our BL31 reads
BL33 (U-Boot) from the same FIP itself. U-Boot boots the OpenWrt FIT from
`ubi_k0`. The FIP's certificate chain is checked, but the SoC has no root key
burnt in, so a chain signed with your own keys is accepted. BL2 arms a 23 s
watchdog that U-Boot stops: if BL31 or U-Boot hangs, BL2 boots the standby FIP,
`fip1`, which keeps the vendor BL31 and U-Boot.

## Stock firmware compared

The stock firmware is VSOL's V1.0.00 (builds of August 2026) on the same hardware.
"Not measured" means this project hasn't examined that behaviour on stock.

### Platform

| | Stock firmware | This firmware |
| --- | --- | --- |
| Base system | Closed vendor build, vendor kernel and web interface | OpenWrt, Linux 6.18, musl, LuCI |
| Source | Vendor drivers are binary or vendor GPL drops | All drivers and daemons are source |
| Management | Vendor web interface, TR-069 | LuCI, ubus, SSH, `cwmpd` for TR-069 |
| Bootloaders | Vendor BL2, BL31 (TF-A 1.5) and U-Boot 2022.10 | Vendor BL2, TF-A v2.14 BL31, U-Boot 2026.10 |
| Updates | Vendor images in slot A or B | `sysupgrade` from the shell or LuCI, settings kept |
| Packages | Fixed | `apk` package manager |
| Flash | UBI with two image slots | One image; `rootfs_data` has 82 MiB |

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
| `boot` (0x0, 2 MiB) | GPT, BL1 data, `fip0` (0x40000, 768 KiB: BL2, our BL31 and U-Boot) and `fip1` (0x100000: the vendor FIP, never written) |
| `env`, `env2` | The vendor U-Boot's environment, used only when `fip1` boots |
| `static_conf` (0x300000, 256 KiB) | Factory data: MACs, GPON serial, optics calibration, a copy of stock `ubi_Config` |
| `ubi_device` | UBI: `ubi_DTB0`, `ubi_k0` (OpenWrt FIT, 6 MiB), `rootfs` (squashfs), `rootfs_data` (UBIFS overlay), `ubootenv` and `ubootenv2` (our U-Boot's environment) |

Linux shows `boot` as one partition; U-Boot splits it so that only `fip0` can be
written. The vendor U-Boot in `fip1` still reads `ubi_k0` and `ubi_DTB0`, so both
stay, and `ubi_k0` keeps its 6 MiB.

## Installation

### 1. Requirements

- A USB serial adapter (3.3 V) on the console, 115200 8N1.
- A PC with a TFTP server on `192.168.1.100/24`, connected to **lan3**. The vendor
  U-Boot only uses lan3 for TFTP; our U-Boot uses any LAN port.
- The initramfs image and the sysupgrade image from a build (see [Building](#building)):
  `openwrt-realtek-taurus-rtl9607dq-vsol_hg325ax30-initramfs-kernel.bin` and
  `openwrt-realtek-taurus-rtl9607dq-vsol_hg325ax30-squashfs-sysupgrade.bin`.
- For the open bootloaders, the FIP built from your own unit's BL2
  (see [Building the FIP](#building-the-fip)).
- Fibre unplugged until the install is done.

### 2. Back up the whole flash

Boot the initramfs from RAM (next step) and copy every MTD device to the PC, for
example with `tftp -p -l /dev/mtdN -r mtdN.bin 192.168.1.100` for each entry in
`/proc/mtd` (the TFTP server must accept uploads). Check the copies, and keep
them private: they contain `ubi_Config` with your ISP credentials.

A dump with a programmer is the better backup, because it can be written back
whatever state the board is in: a WSON8 8x6 mm clip on a CH341A works with
SNANDer (flashrom does not support SPI NAND). Read the chip twice and compare.

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

The first flash removes the stock volumes (`ubi_Config`, `ubi_DTB1`, `ubi_k1`,
`ubi_r0`, `ubi_r1`, `ubi_framework1/2`, `ubi_apps`), creates `ubootenv` and
`ubootenv2`, writes `ubi_k0` and creates `rootfs` and `rootfs_data`. The PON LOS
LED blinks while the image is written. Do not cut power. The vendor U-Boot boots
the new image from `ubi_k0`.

### 5. First boot

The board comes up at `192.168.1.1`. Set a root password, then connect the fibre.
The first boot seeds the GPON serial and password from the stock configuration
kept in `static_conf`.

### 6. Install the open bootloaders

Stop the vendor U-Boot with **Ctrl+A** and write the FIP to `fip0` only:

```text
tftpboot 0x05000000 hg325ax30-fip0.bin
nand erase 0x40000 0xc0000
nand write 0x05000000 0x40000 ${filesize}
reset
```

The board now prints `BL31: v2.14.0` and `U-Boot 2026.10 ... hg325ax30`, then
boots OpenWrt. `fip1` is not touched: if the new FIP does not come up, BL2 boots
the vendor bootloaders from `fip1` after 23 s and you can write `fip0` again.

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
and line coefficients (`fw_dxs.bin`, `bbd.bin`) are installed by the
`dxs-firmware` package from files you take from your own stock firmware (see
[Building](#building)).

### GPON identity

`omci.main.serial` and `omci.main.password` set the ONU identity; edit them in
LuCI or `uci`.

## Updates

```bash
sysupgrade -v /tmp/hg325ax30-sysupgrade.bin
```

Settings are kept. LuCI's Flash Images page does the same.

sysupgrade sets `upgrade_available` in the U-Boot environment, and U-Boot counts
the boots of the new image until OpenWrt clears it after a good boot. After three
failed boots in a row it stays at its prompt with the network up. Other boots
only read the environment.

The bootloaders are updated from the U-Boot prompt. Put the new FIP on the TFTP
server as `hg325ax30-fip0.bin`; the script checks its size (0xc0000) and magic,
writes only `fip0` and reads it back:

```text
run upgrade_uboot
reset
```

## Back to stock

The stock volumes are removed by the first install, so the way back is the full
flash backup: write the programmer dump back with SNANDer, or restore the MTD
copies from an initramfs. `fip1` and the vendor environment are never written,
so the vendor bootloaders are still on the board, but they have no stock image
left to boot.

## Recovery

- **New image fails to boot:** after three tries U-Boot stops at its prompt.
  Load the initramfs over TFTP on any LAN port and flash a working image:
  `tftpboot ${loadaddr} hg325ax30-initramfs.bin` then `bootm ${loadaddr}`.
  `setenv bootcount 0; saveenv; reset` retries the normal boot.
- **Our U-Boot or BL31 does not start:** BL2 boots the vendor bootloaders from
  `fip1` after 23 s. Stop that U-Boot with Ctrl+A and write `fip0` again as in
  [Install the open bootloaders](#6-install-the-open-bootloaders).
- **Lost factory data:** restore `static_conf` from your backup.
- **Wi-Fi or LAN stuck at boot:** power off fully (not a warm reboot) and wait
  5 seconds.
- **A programmer is the last resort.** The NAND is a WSON8 package: use a WSON8
  8x6 mm clip, not a SOIC8 one.

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

The `dxs-firmware` package needs `fw_dxs.bin` and `bbd.bin` in
`vsol-local/firmware/dxs/`, and checks their sha256. They are
`/image/fw_dxs-2.1.3.bin` and `/image/IBB12_R600.BIN` in the stock root file
system (`ubi_r0` or `ubi_r1`, squashfs); copy them before the first install.

### Building the FIP

The FIP holds BL2, BL31, U-Boot and six certificates. BL2 cannot be
redistributed, so take it from your own unit: cut `fip0` out of the backup of
the `boot` partition and unpack it with `fiptool`. BL31 comes from the
`arm-trusted-firmware-taurus` package, and U-Boot from the
`u-boot-vsol_hg325ax30_bl33` package (select it under Boot Loaders), which is
linked at the address BL31 starts it from.

```bash
make package/boot/arm-trusted-firmware-taurus/compile
TFA=build_dir/target-aarch64_generic_musl/trusted-firmware-a-vsol_hg325ax30/trusted-firmware-a-2.14
env PATH="$PWD/staging_dir/toolchain-aarch64_generic_gcc-14.4.0_musl/bin:$PATH" \
	make -C $TFA CROSS_COMPILE=aarch64-openwrt-linux-musl- PLAT=rtl9607dq \
	OPENSSL_DIR=/usr certtool fiptool   # again after every compile above
FIPTOOL=$TFA/build/rtl9607dq/release/tools/fiptool/fiptool
CERT=$TFA/build/rtl9607dq/release/tools/cert_create/cert_create
IMG=staging_dir/target-aarch64_generic_musl/image

dd if=mtd0.bin of=fip0-stock.bin bs=4096 skip=64 count=192   # boot, 0x40000 + 0xc0000
$FIPTOOL unpack --tb-fw bl2.bin fip0-stock.bin

FW="--tb-fw bl2.bin --soc-fw $IMG/vsol_hg325ax30-bl31.bin --nt-fw $IMG/vsol_hg325ax30_bl33-u-boot.bin"
CRT="--trusted-key-cert tk.crt --soc-fw-key-cert sk.crt --nt-fw-key-cert nk.crt \
     --tb-fw-cert tb.crt --soc-fw-cert soc.crt --nt-fw-cert nt.crt"
$CERT -n --tfw-nvctr 0 --ntfw-nvctr 0 $FW $CRT    # -n: new keys each time
$FIPTOOL create $FW $CRT fip.bin
cp fip.bin hg325ax30-fip0.bin && truncate -s 786432 hg325ax30-fip0.bin
```

`upgrade_uboot` wants the file at the full slot size, 0xc0000 bytes.

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

The boot chain uses mainline Trusted Firmware-A (BSD-3-Clause) and mainline
U-Boot; the RTL9607DQ platform code, U-Boot board and drivers are ours, with the
Cortina QSPI and serial code credited in their headers.

Other sources: `uvoice` and `udsp` come from `cmonroe/uvoice`; the
`xpon-agent` and `luci-app-xpon` packages descend from the Airoha GPON OMCI
packages by Matheus Sampaio Queiroga; Wi-Fi uses the upstream `rtw89` driver
and `linux-firmware`, and the RTL8192XB driver is our own port, written from
the upstream code and Realtek's GPL release.

Original authorship and file-level license notices are retained.

OpenWrt is licensed under GPL-2.0; component licenses remain applicable.
