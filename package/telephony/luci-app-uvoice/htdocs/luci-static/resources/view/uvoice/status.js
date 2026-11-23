'use strict';
'require view';
'require poll';
'require rpc';
'require ui';
'require uci';

const callVoiceStatus = rpc.declare({
	object: 'voice',
	method: 'status',
	expect: { }
});

const callLineTest = rpc.declare({
	object: 'voice',
	method: 'line_test',
	params: [ 'line', 'test' ],
	expect: { }
});

/* measured on the board: 4656 nF with a phone, 19 nF without */
const PHONE_MIN_NF = 300;
const NO_PHONE_MAX_NF = 100;
const GROUND_MAX_NF = 1000;

function lineTestText(r) {
	const c = r?.record;

	if (r?.status != 'complete' || !c)
		return _('The test could not run. Hang up the phone and try again.');

	let phone;
	if (c.c_tr_nf >= PHONE_MIN_NF)
		phone = _('Phone connected.');
	else if (c.c_tr_nf < NO_PHONE_MAX_NF)
		phone = _('No phone detected.');
	else
		phone = _('Unclear whether a phone is connected.');

	const low = Math.min(c.c_tg_nf, c.c_rg_nf), high = Math.max(c.c_tg_nf, c.c_rg_nf);
	const wire = (high < GROUND_MAX_NF && high <= 2 * low + 50)
		? _('No wiring problem found.')
		: _('Possible wiring problem: the two wires are unbalanced to ground.');

	return '%s %s (%d / %d / %d nF)'.format(phone, wire, c.c_tr_nf, c.c_tg_nf, c.c_rg_nf);
}

function handleLineTest(line, ev) {
	const btn = ev.currentTarget;

	btn.disabled = true;
	btn.classList.add('spinning');

	return L.resolveDefault(callLineTest(line, 'capacitance'), null).then((r) => {
		btn.disabled = false;
		btn.classList.remove('spinning');
		ui.addNotification(_('Line %d test').format(line + 1), E('p', {}, [ lineTestText(r) ]), 'info');
	});
}

const regStates = {
	registered: _('Registered'),
	refreshing: _('Registered'),
	trying: _('Registering'),
	resolving: _('Resolving the server'),
	backoff: _('Failed, retrying'),
	unregistered: _('Not registered')
};

const halStates = {
	down: _('Not started'),
	attached: _('Starting'),
	starting: _('Starting'),
	ready: _('Ready'),
	failed: _('Failed')
};

const lineStates = {
	IDLE: _('On hook'),
	OFFHOOK_DIAL: _('Off hook, dial tone'),
	COLLECTING: _('Dialling'),
	SPRE_COLLECT: _('Dialling'),
	XFER_COLLECT: _('Dialling a transfer'),
	HOLD_DIAL: _('Dialling a second call'),
	CALLING: _('Calling'),
	PROCEEDING: _('Calling'),
	PENDING: _('Calling'),
	RINGING: _('Ringing'),
	CONNECTED: _('In a call'),
	CW_OFFERED: _('Call waiting'),
	TWO_CALLS: _('Two calls'),
	HOLDING: _('Call on hold'),
	CONF: _('Conference'),
	CLEARING: _('Call ended, hang up'),
	ROH: _('Left off hook'),
	SILENT: _('Off hook, silent'),
	OOS: _('Out of service')
};

const faultNames = [
	[ 1 << 1, _('Overheated') ],
	[ 1 << 2, _('Ground fault') ],
	[ 1 << 3, _('Power alarm') ],
	[ 1 << 4, _('Battery voltage') ],
	[ 1 << 7, _('Power converter down') ],
	[ 1 << 9, _('PCM clock lost') ],
	[ 1 << 10, _('Chip needs a reset') ]
];

function volts(mv) {
	return '%.1f V'.format(mv / 1000);
}

function amps(ua) {
	return '%.3f A'.format(ua / 1000000);
}

function faults(bits) {
	const names = faultNames.filter((f) => bits & f[0]).map((f) => f[1]);

	return names.length ? names.join(', ') : _('None');
}

function lineName(n) {
	return _('Line %d').format(n + 1);
}

function infoTable(rows) {
	return E('table', { 'class': 'table' }, rows.map((r) =>
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, [ r[0] ]),
			E('td', { 'class': 'td left' }, [ r[1] ])
		])));
}

/* the ports the board is set up for, while the hardware does not report them */
function configuredPorts() {
	return +uci.get('voip', 'base', 'channels') ||
	       uci.sections('voip', 'channel').length || 1;
}

return view.extend({
	load() {
		return Promise.all([
			L.resolveDefault(callVoiceStatus(), null),
			uci.load('voip')
		]).then(([st]) => st);
	},

	renderService(st) {
		const hal = st.hal ?? {};
		const region = st.region ?? {};
		const net = st.network ?? {};

		return infoTable([
			[ _('Service'), st.config?.enabled ? _('Enabled') : _('Disabled') ],
			[ _('Version'), st.version ?? '-' ],
			[ _('Phone port hardware'), hal.slic || '-' ],
			[ _('Hardware state'), halStates[hal.state] ?? hal.state ?? '-' ],
			[ _('Region'), region.describe || region.applied || region.requested || '-' ],
			[ _('IPv4 address'), net.address ?? '-' ],
			[ _('IPv6 address'), net.address6 ?? '-' ],
			[ _('Network device'), net.l3_device ?? '-' ]
		]);
	},

	renderLines(st) {
		const accounts = {};
		const monitor = {};

		for (const a of (st.lines ?? []))
			accounts[a.line] = a;
		for (const r of (st.monitor ?? []))
			monitor[r.line] = r;

		const table = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, [ _('Line') ]),
				E('th', { 'class': 'th' }, [ _('Phone') ]),
				E('th', { 'class': 'th' }, [ _('Account') ]),
				E('th', { 'class': 'th' }, [ _('Registration') ]),
				E('th', { 'class': 'th' }, [ _('Server') ]),
				E('th', { 'class': 'th' }, [ _('Line voltage') ]),
				E('th', { 'class': 'th' }, [ _('Loop current') ]),
				E('th', { 'class': 'th' }, [ _('Faults') ]),
				E('th', { 'class': 'th' }, [ ' ' ])
			])
		]);

		/* the daemon keeps a slot per possible line; show the ports that exist */
		const ports = st.hal?.lines ?? configuredPorts();

		cbi_update_table(table, (st.call ?? []).filter((c) => c.line < ports).map((c) => {
			const a = accounts[c.line];
			const r = monitor[c.line] ?? {};
			const measured = r.sample == 'sampled' || r.sample == 'stale';
			let reg = _('No account configured');

			if (a) {
				reg = regStates[a.state] ?? a.state;
				if (a.last_code && a.state != 'registered' && a.state != 'refreshing')
					reg += ' (SIP %d)'.format(a.last_code);
			}

			return [
				lineName(c.line),
				c.oos ? _('Out of service') : (lineStates[c.state] ?? c.state ?? '-'),
				a?.aor || '-',
				reg,
				a?.registrar ? '%s:%d'.format(a.registrar, a.registrar_port) : '-',
				measured ? volts(r.vtr_mv) : '-',
				measured ? amps(r.itr_ua) : '-',
				faults(r.fault_bits ?? 0),
				E('button', {
					'class': 'btn cbi-button cbi-button-action',
					'title': _('Check whether a phone is connected and the wiring is healthy. Hang up first; takes about two seconds.'),
					'click': L.bind(handleLineTest, this, c.line)
				}, [ _('Test line') ])
			];
		}), E('em', {}, [ _('The phone lines are not up yet.') ]));

		return table;
	},

	renderStatus(st) {
		if (!st)
			return E('div', { 'class': 'alert-message warning' }, [
				_('The voice service is not running.')
			]);

		return E('div', {}, [
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, [ _('Voice service') ]),
				this.renderService(st)
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, [ _('Phone lines') ]),
				this.renderLines(st),
				E('div', { 'class': 'cbi-section-descr' }, [
					_('Line voltage is the DC voltage between the tip and ring wires the port holds: about 40 to 50 V with the phone on hook, a few volts in a call. Loop current flows only while the phone is off hook, typically 0.020 to 0.040 A.')
				])
			])
		]);
	},

	render(st) {
		const node = E('div', {}, [ this.renderStatus(st) ]);

		poll.add(() => L.resolveDefault(callVoiceStatus(), null).then((st) =>
			node.replaceChildren(this.renderStatus(st))), 2);

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, [ _('Telephony') ]),
			E('div', { 'class': 'cbi-map-descr' }, [
				_('State of the analogue phone ports and their SIP accounts.')
			]),
			node
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
