// SPDX-License-Identifier: GPL-2.0-or-later
//
// Blink the USB LED while a USB disk reads or writes, then hand it back to
// its usbport trigger. The kernel timer trigger does the blinking; this only
// watches the disk counters. The LED is left alone while another trigger,
// such as netdev for USB tethering, owns it. Block hotplug restarts the
// service when a disk comes or goes, so the disk list is read once.

'use strict';

import { readfile, writefile, lsdir, readlink } from 'fs';
import { cursor } from 'uci';
import * as uloop from 'uloop';

const POLL_IDLE_MS = 500;
const POLL_BUSY_MS = 200;
const BLINK_MS = 60;
const IDLE_POLLS = 3;

const uci = cursor();
const led = uci.get('system', 'led_usb', 'sysfs');
const ports = uci.get('system', 'led_usb', 'port') ?? [];
const sysfs = `/sys/class/leds/${led}`;

const stats = map(filter(lsdir('/sys/block') ?? [],
	(d) => match(readlink(`/sys/block/${d}`) ?? '', /\/usb[0-9]/)),
	(d) => `/sys/block/${d}/stat`);

let last = '', blinking = false, idle = 0;

function trigger() {
	return match(readfile(`${sysfs}/trigger`) ?? '', /\[([^\]]+)\]/)?.[1];
}

function blink() {
	writefile(`${sysfs}/trigger`, 'timer');
	writefile(`${sysfs}/delay_on`, `${BLINK_MS}`);
	writefile(`${sysfs}/delay_off`, `${BLINK_MS}`);
}

function usbport() {
	writefile(`${sysfs}/trigger`, 'usbport');
	for (let p in (type(ports) == 'array' ? ports : [ ports ]))
		writefile(`${sysfs}/ports/${p}`, '1');
}

/* sectors read and written: fields 3 and 7 of each stat file */
function counters() {
	let s = '';

	for (let f in stats) {
		const v = split(trim(readfile(f) ?? ''), /\s+/);

		s += `${v[2]}/${v[6]} `;
	}
	return s;
}

function poll() {
	const now = counters();
	const busy = last != '' && now != last;

	last = now;

	if (busy) {
		idle = 0;
		/* take the LED only from usbport, never from another owner */
		if (!blinking && trigger() == 'usbport') {
			blink();
			blinking = true;
		}
	} else if (blinking && ++idle >= IDLE_POLLS) {
		/* someone else may have taken the LED meanwhile */
		if (trigger() == 'timer')
			usbport();
		blinking = false;
	}

	uloop.timer(blinking ? POLL_BUSY_MS : POLL_IDLE_MS, poll);
}

if (!led || !length(stats) || !readfile(`${sysfs}/trigger`))
	exit(0);

uloop.init();
poll();
uloop.run();
