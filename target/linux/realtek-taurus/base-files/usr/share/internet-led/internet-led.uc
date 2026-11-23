// SPDX-License-Identifier: GPL-2.0-only
//
// The Internet LED follows real IPv4 reachability: every two seconds one
// ping leaves through the default route. Lit, blinking with traffic, while a
// host answers; off after two rounds in which none did (about 4 s), or while
// there is no default route.

'use strict';

import * as uloop from 'uloop';
import { popen, writefile } from 'fs';

const ROUND_MS = 2000;
const MAX_MISSED = 2;
const HOSTS = [ '1.1.1.1', '8.8.8.8' ];

const sysfs = `/sys/class/leds/${ARGV[0]}`;
let shown, missed = MAX_MISSED;

function default_device() {
	const p = popen('ip -4 route show default 2>/dev/null');
	const line = p.read('line');

	p.close();
	return match(line ?? '', /\bdev (\S+)/)?.[1];
}

function reachable(dev) {
	for (let host in HOSTS)
		if (system(`ping -4 -c 1 -W 1 -q -I ${dev} ${host} >/dev/null 2>&1`) == 0)
			return true;
	return false;
}

function show(dev) {
	if (dev == shown)
		return;

	shown = dev;
	if (dev) {
		writefile(`${sysfs}/trigger`, 'netdev');
		writefile(`${sysfs}/device_name`, dev);
		for (let attr in [ 'link', 'tx', 'rx' ])
			writefile(`${sysfs}/${attr}`, '1');
	} else {
		writefile(`${sysfs}/trigger`, 'none');
		writefile(`${sysfs}/brightness`, '0');
	}
}

function round() {
	const dev = default_device();

	if (dev && reachable(dev))
		missed = 0;
	else if (missed < MAX_MISSED)
		missed++;
	show(missed < MAX_MISSED ? dev : null);
	uloop.timer(ROUND_MS, round);
}

uloop.init();
round();
uloop.run();
