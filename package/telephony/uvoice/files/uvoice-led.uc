// SPDX-License-Identifier: GPL-2.0-only
//
// Drive the phone LED from the uvoiced status:
//   off         service off, hardware not ready, or no port with an account
//   slow blink  an account registering or waiting to retry, or the voice
//               interface without carrier (1 per second)
//   on          every account registered, phones idle
//   fast blink  a phone ringing, off hook or in a call

'use strict';

import { connect } from 'ubus';
import * as uloop from 'uloop';
import { readfile, writefile } from 'fs';
import { cursor } from 'uci';

const POLL_MS = 250;
/* how often the voice interface's device is looked up again, in polls */
const DEVICE_POLLS = 20;
const blink = { registering: [ 500, 500 ], busy: [ 100, 100 ] };

const sysfs = `/sys/class/leds/${ARGV[0]}`;
let shown, device, device_age = DEVICE_POLLS;

function show(pattern) {
	if (pattern == shown)
		return;

	shown = pattern;
	if (blink[pattern]) {
		writefile(`${sysfs}/trigger`, 'timer');
		writefile(`${sysfs}/delay_on`, `${blink[pattern][0]}`);
		writefile(`${sysfs}/delay_off`, `${blink[pattern][1]}`);
	} else {
		writefile(`${sysfs}/trigger`, 'none');
		writefile(`${sysfs}/brightness`, pattern == 'on' ? '1' : '0');
	}
}

/*
 * A registration outlives the network under it: uvoiced only learns of an
 * outage at the next refresh, up to the expiry later. So the carrier of the
 * voice interface's device counts too.
 */
function voice_link_up(bus) {
	if (++device_age >= DEVICE_POLLS) {
		const iface = cursor().get('voip', 'base', 'local_interface');
		const st = iface && bus ? bus.call(`network.interface.${iface}`, 'status') : null;

		device = st?.l3_device ?? st?.device;
		device_age = 0;
	}
	if (!device)
		return true;
	return trim(readfile(`/sys/class/net/${device}/carrier`) ?? '0') == '1';
}

function pattern(st, link_up) {
	if (!st?.config?.enabled || st?.hal?.state != 'ready')
		return 'off';

	const ports = st.hal.lines ?? length(st.call);
	const accounts = {};
	let account = false, busy = false, registering = false;

	for (let a in st.lines ?? [])
		accounts[a.line] = a;

	for (let c in st.call ?? []) {
		const a = accounts[c.line];

		if (c.line >= ports || c.oos || !a)
			continue;

		account = true;
		if (c.ringing || (c.state && c.state != 'IDLE'))
			busy = true;

		switch (a.state) {
		case 'registered':
		case 'refreshing':
			break;
		default:
			registering = true;
			break;
		}
	}

	if (!account)
		return 'off';
	if (busy)
		return 'busy';
	if (registering || !link_up)
		return 'registering';
	return 'on';
}

const bus = connect();

uloop.init();

function poll() {
	show(pattern(bus?.call('voice', 'status'), voice_link_up(bus)));
	uloop.timer(POLL_MS, poll);
}

poll();
uloop.run();
