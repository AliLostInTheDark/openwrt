'use strict';
'require view';
'require poll';
'require rpc';

const callVoiceCdr = rpc.declare({
	object: 'voice',
	method: 'cdr',
	expect: { cdr: [] }
});

function duration(ms) {
	return ms ? '%t'.format(Math.round(ms / 1000)) : '-';
}

/* small pill: coloured dot, tinted background; readable on light and dark themes */
function badge(rgb, text, title) {
	return E('span', {
		'title': title ?? '',
		'style': 'display:inline-flex;align-items:center;gap:.4em;padding:.1em .6em;' +
			 'border-radius:999px;white-space:nowrap;font-size:90%%;line-height:1.6;' +
			 'background:rgba(%s,.14);border:1px solid rgba(%s,.35)'.format(rgb, rgb)
	}, [
		E('span', { 'style': 'width:.5em;height:.5em;border-radius:50%%;background:rgb(%s)'.format(rgb) }),
		text
	]);
}

/* ITU-T G.107 bands of the listening MOS */
function quality(m) {
	const mos = +(m?.mos_lq ?? 0);

	if (!mos)
		return '-';
	const band = mos >= 4.0 ? [ '34,160,90', _('Good') ] :
		     mos >= 3.6 ? [ '40,130,200', _('Fair') ] :
		     mos >= 3.1 ? [ '220,140,20', _('Poor') ] : [ '210,60,50', _('Bad') ];
	return badge(band[0], '%s %s'.format(band[1], m.mos_lq),
		_('MOS listening %s, conversational %s, R-factor %d').format(
			m.mos_lq, m.mos_cq ?? '-', m.rfactor ?? 0));
}

/* RTCP loss fraction (RFC 3550 A.3), in units of 1/256 */
function percent(v) {
	return v != null ? '%.1f %%'.format(+v * 100 / 256) : '-';
}

function media(c) {
	const m = c.media;

	if (!m)
		return '-';
	return E('span', { 'style': 'white-space:nowrap' }, [
		_('loss %s / far %s').format(percent(m.loss_rate), percent(m.loss_rate_far)), E('br'),
		_('jitter %d / far %d ms').format(m.jitter_avg ?? 0, m.jitter_far ?? 0), E('br'),
		_('RTT %d ms').format(m.rtt_avg ?? 0), E('br'),
		_('packets %d sent, %d received, %d lost').format(m.packets_sent ?? 0,
			m.packets_recv ?? 0, m.packets_lost ?? 0)
	]);
}

function buffer(c) {
	const j = c.jb;

	if (!j)
		return '-';
	return E('span', { 'style': 'white-space:nowrap' }, [
		_('%s, %d ms').format(j.mode ?? '-', j.depth_ms ?? 0), E('br'),
		_('concealed %d, late %d').format(j.plc ?? 0, j.late_drop ?? 0), E('br'),
		_('underruns %d').format(j.underruns ?? 0)
	]);
}

return view.extend({
	load() {
		return L.resolveDefault(callVoiceCdr(), []);
	},

	update(table, cdr) {
		cbi_update_table(table, cdr.slice().reverse().map((c) => [
			new Date(c.start * 1000).toLocaleString(),
			_('Line %d').format(c.line + 1),
			c.inbound ? _('Incoming') : _('Outgoing'),
			c.number ?? c.remote ?? '-',
			c.name ?? '',
			c.answered ? duration(c.duration_ms)
				   : badge('220,140,20', c.fail ?? _('Missed')),
			c.codec ? '%s, %d ms'.format(c.codec, c.ptime ?? 0) : '-',
			quality(c.media),
			media(c),
			buffer(c)
		]), E('em', {}, [ _('No calls yet.') ]));
	},

	render(cdr) {
		const table = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, [ _('Time') ]),
				E('th', { 'class': 'th' }, [ _('Line') ]),
				E('th', { 'class': 'th' }, [ _('Direction') ]),
				E('th', { 'class': 'th' }, [ _('Number') ]),
				E('th', { 'class': 'th' }, [ _('Name') ]),
				E('th', { 'class': 'th' }, [ _('Duration') ]),
				E('th', { 'class': 'th' }, [ _('Codec') ]),
				E('th', { 'class': 'th' }, [ _('Quality') ]),
				E('th', { 'class': 'th' }, [ _('Network') ]),
				E('th', { 'class': 'th' }, [ _('Jitter buffer') ])
			])
		]);

		this.update(table, cdr);
		poll.add(() => L.resolveDefault(callVoiceCdr(), []).then((cdr) =>
			this.update(table, cdr)), 10);

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, [ _('Telephony') ]),
			E('div', { 'class': 'cbi-map-descr' }, [
				_('The most recent calls of all lines, newest first, with the codec and the call quality measured from RTP and RTCP. Hover the quality for the MOS and R-factor; "far" values are what the other side reported.')
			]),
			E('div', { 'class': 'cbi-section' }, [ table ])
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
