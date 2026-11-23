#!/bin/sh
#
# Every interrupt of the SoC lands on CPU0 by default, and the generic
# steering puts both NI NAPI threads (LAN and WAN receive) on one CPU. Run
# the generic steering, then give the Ethernet receive interrupt, each NI
# NAPI thread and each Wi-Fi card a CPU of its own.

/usr/libexec/network/packet-steering.uc "$@"

[ "$(uci -q get network.@globals[0].packet_steering)" = 0 ] && exit 0

. /lib/functions.sh

irq_cpu() {
	local irq

	irq=$(awk -v n="$2" '$NF == n { sub(":", "", $1); print $1 }' /proc/interrupts)
	[ -n "$irq" ] && echo "$1" > "/proc/irq/$irq/smp_affinity"
}

case "$(board_name)" in
vsol,hg325ax30)
	irq_cpu 2 f4325000.pcie		# RTL8852CE, 5 GHz; rtw89 NAPI runs here
	irq_cpu 4 4f4300000.ethernet-rx0
	irq_cpu 8 f4326000.pcie		# RTL8192XB, 2.4 GHz

	cpu=2
	for pid in $(pgrep '^napi/eth0-'); do
		taskset -p "$(printf %x $((1 << cpu)))" "$pid" >/dev/null
		cpu=$((cpu + 1))
	done
	;;
esac
