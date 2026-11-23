'use strict';
'require view';
'require form';
'require rpc';
'require uci';

const callVoiceStatus = rpc.declare({
	object: 'voice',
	method: 'status',
	expect: { }
});

/* TR-104 codec numbers uvoice takes, keyed by the name the hardware reports */
const codecs = [
	[ 'pcma', '2', _('G.711 A-law (PCMA)') ],
	[ 'pcmu', '1', _('G.711 \u00b5-law (PCMU)') ],
	[ 'g729', '3', _('G.729') ],
	[ 'g726_32', '4', _('G.726, 32 kbit/s') ],
	[ 'g722', '5', _('G.722 (wideband)') ],
	[ 'opus', '6', _('Opus (wideband)') ]
];

return view.extend({
	load() {
		return Promise.all([
			L.resolveDefault(callVoiceStatus(), {}),
			uci.load('voip')
		]).then(([st]) => st);
	},

	render(st) {
		const hal = st?.hal ?? {};
		const supported = Array.isArray(hal.codecs) ? hal.codecs : codecs.map((c) => c[0]);
		/* the ports the board is set up for, while the hardware does not report them */
		const ports = hal.lines ?? (+uci.get('voip', 'base', 'channels') ||
					    uci.sections('voip', 'channel').length || 1);
		let m, s, o;

		m = new form.Map('voip', _('Telephony'),
			_('The SIP account and calling features of each analogue phone port. The provider supplies the account details.'));

		s = m.section(form.GridSection, 'channel', _('Lines'));
		s.anonymous = false;
		s.addremove = false;
		s.nodescriptions = true;
		s.sectiontitle = (section_id) =>
			_('Line %d').format(+(m.data.get('voip', section_id, 'id') ?? 0) + 1);

		s.tab('account', _('Account'));
		s.tab('features', _('Features'));
		s.tab('dialing', _('Dialling'));
		s.tab('audio', _('Audio'));
		s.tab('advanced', _('Advanced'));

		o = s.taboption('account', form.Flag, 'enabled', _('Enabled'),
			_('Register the account and accept calls on this port.'));
		o.rmempty = false;
		o.editable = true;

		o = s.taboption('account', form.Value, 'sip_username', _('Number'),
			_('Phone number or user name of the account, as the provider gives it, e.g. <code>+911141234567</code> or <code>01141234567</code>.'));

		o = s.taboption('account', form.Value, 'sip_auth_username', _('Authentication user'),
			_('Login name for the password when it differs from the number, e.g. <code>+911141234567@ims.example.in</code>. Leave empty to use the number.'));
		o.modalonly = true;

		o = s.taboption('account', form.Value, 'sip_secret', _('Password'),
			_('Password of the account.'));
		o.password = true;
		o.modalonly = true;

		o = s.taboption('account', form.Value, 'sip_domain', _('Domain'),
			_('SIP domain of the provider, the part after the <code>@</code> of the account address, e.g. <code>ims.example.in</code>.'));
		o.datatype = 'host';

		o = s.taboption('account', form.Value, 'sip_registrar', _('Registrar'),
			_('Server the account registers with: a host name or IP address. Leave empty to use the domain.'));
		o.datatype = 'host';
		o.modalonly = true;

		o = s.taboption('account', form.Value, 'sip_proxy', _('Proxy'),
			_('Server the calls are sent to: a host name or IP address. Leave empty to use the domain.'));
		o.datatype = 'host';
		o.modalonly = true;

		o = s.taboption('account', form.Value, 'sip_port', _('Proxy port'),
			_('Port of the proxy. Default: 5060.'));
		o.datatype = 'port';
		o.placeholder = '5060';
		o.modalonly = true;

		o = s.taboption('account', form.Value, 'sip_outbound_proxy', _('Outbound proxy'),
			_('Server every request goes through first, only when the provider names one (often called P-CSCF or SBC).'));
		o.datatype = 'host';
		o.modalonly = true;

		o = s.taboption('account', form.Value, 'sip_callerid_name', _('Display name'),
			_('Name shown to the people you call, when the provider passes it on.'));
		o.modalonly = true;

		o = s.taboption('features', form.Flag, 'call_waiting_enabled', _('Call waiting'),
			_('Signal a second incoming call during a call; switch with the flash (recall) button.'));
		o.default = o.enabled;
		o.modalonly = true;

		o = s.taboption('features', form.Flag, 'cid_remote_enable', _('Show caller ID'),
			_('Send the number of the caller to the phone before it rings.'));
		o.default = o.enabled;
		o.modalonly = true;

		o = s.taboption('features', form.Flag, 'cid_outgoing_enable', _('Send own caller ID'),
			_('Let the people you call see your number.'));
		o.default = o.enabled;
		o.modalonly = true;

		o = s.taboption('features', form.Flag, 'do_not_disturb_enabled', _('Do not disturb'),
			_('Reject incoming calls; the phone does not ring.'));
		o.modalonly = true;

		o = s.taboption('features', form.Flag, 'vmwi', _('Voicemail lamp'),
			_('Light the message waiting lamp of phones that have one.'));
		o.default = o.enabled;
		o.modalonly = true;

		o = s.taboption('features', form.Flag, 'sync_phone_time', _('Sync phone time'),
			_('Set the clock on the phone\'s display: the date and time go to the idle phone without ringing it, after the router\'s clock is set by NTP and then at the interval below. It is sent as an FSK caller ID record with no number, so the phone needs FSK caller ID support; some phones list it as an unavailable call.'));
		o.modalonly = true;

		o = s.taboption('features', form.Value, 'sync_phone_time_interval', _('Phone time interval'),
			_('Minutes between updates, 5 to 1440. Default: 60.'));
		o.datatype = 'range(5,1440)';
		o.placeholder = '60';
		o.depends('sync_phone_time', '1');
		o.modalonly = true;

		o = s.taboption('features', form.Flag, 'warmline_enable', _('Hotline'),
			_('Call a fixed number when nothing is dialled after lifting the handset.'));
		o.modalonly = true;

		o = s.taboption('features', form.Value, 'warmline_number', _('Hotline number'),
			_('Number called, dialled as typed on the phone, e.g. <code>112</code>.'));
		o.depends('warmline_enable', '1');
		o.modalonly = true;

		o = s.taboption('features', form.Value, 'warmline_timeout', _('Hotline delay'),
			_('Seconds to wait for a digit before the hotline number is called; 0 calls at once. Default: 8.'));
		o.datatype = 'range(0,60)';
		o.placeholder = '8';
		o.depends('warmline_enable', '1');
		o.modalonly = true;

		o = s.taboption('audio', form.Value, 'ps_tx_gain', _('Microphone level'),
			_('Level from the phone to the network in dB, -20 to 0. Lower it when the other side hears you too loud or distorted. Default: -3.'));
		o.datatype = 'range(-20,0)';
		o.placeholder = '-3';
		o.modalonly = true;

		o = s.taboption('audio', form.Value, 'ps_rx_gain', _('Earpiece level'),
			_('Level from the network to the phone in dB, -20 to 0. Raise it towards 0 when the other side sounds too quiet. Default: -9.'));
		o.datatype = 'range(-20,0)';
		o.placeholder = '-9';
		o.modalonly = true;

		o = s.taboption('audio', form.ListValue, 'sip_dtmf_mode', _('Keypad tones'),
			_('How dialled digits reach the far end during a call, e.g. for voice menus. <em>RTP events</em> suits most providers.'));
		o.value('rfc2833', _('RTP events (RFC 2833)'));
		o.value('inband', _('In the audio'));
		o.value('info', _('SIP INFO messages'));
		o.default = 'inband';
		o.modalonly = true;

		o = s.taboption('advanced', form.ListValue, 'sip_flash_relay', _('Flash button'),
			_('Where the flash (recall) button is handled: on this router for call waiting and transfers, or passed to the provider.'));
		o.value('off', _('On this router'));
		o.value('info', _('To the provider as SIP INFO'));
		o.value('rfc2833', _('To the provider as an RTP event'));
		o.default = 'info';
		o.modalonly = true;

		o = s.taboption('advanced', form.Flag, 'early_media_enabled', _('Early media'),
			_('Play the ringback tone or announcement the provider sends before the call is answered.'));
		o.default = o.enabled;
		o.modalonly = true;

		o = s.taboption('advanced', form.ListValue, 'default_packetization_rate', _('Packet time'),
			_('Audio per network packet. 20 ms unless the provider asks otherwise.'));
		for (const ms of [ 10, 20, 30, 40 ])
			o.value(ms, '%d ms'.format(ms));
		o.default = '20';
		o.modalonly = true;

		o = s.taboption('dialing', form.Value, 'digit_map_base64', _('Digit map'),
			_('When the number is complete, as an MGCP digit map (RFC 3435): <code>x</code> any digit, <code>[2-9]</code> a range, <code>.</code> repeats the one before, <code>T</code> waits for the inter-digit timeout, <code>|</code> separates choices. Empty uses <code>*xx.T|x.T</code>: every number is dialled after the timeout. Example that dials a 10-digit mobile number at once: <code>[6-9]xxxxxxxxx|0xxxxxxxxxx|*xx.T|x.T</code>.'));
		o.placeholder = '*xx.T|x.T';
		o.modalonly = true;
		o.cfgvalue = function(section_id) {
			const v = uci.get('voip', section_id, 'digit_map_base64');
			try { return v ? atob(v) : null; } catch (e) { return null; }
		};
		o.write = function(section_id, value) {
			return uci.set('voip', section_id, 'digit_map_base64', btoa(value.trim()));
		};
		o.remove = function(section_id) {
			return uci.unset('voip', section_id, 'digit_map_base64');
		};
		o.validate = function(section_id, value) {
			if (!value)
				return true;
			return /^[0-9A-D#*xXtT.\[\]\-|]+$/.test(value) && !/\|\||^\||\|$/.test(value)
				? true : _('Only digits, A-D, * # x T . [ ] - and | between choices');
		};

		o = s.taboption('dialing', form.DynamicList, 'dial_rule', _('Number rewrite'),
			_('Each rule is <code>PATTERN STRIP PREFIX</code>: when the dialled number matches PATTERN (digit map syntax, the whole number), STRIP leading digits are removed and PREFIX is put in front; the first match wins. Examples: <code>[6-9]xxxxxxxxx 0 0</code> adds a 0 before a 10-digit number, <code>00x. 2 +</code> turns 00 into +, <code>0xxxxxxxxxx 1</code> removes a leading 0.'));
		o.placeholder = '[6-9]xxxxxxxxx 0 0';
		o.modalonly = true;
		o.validate = function(section_id, value) {
			if (!value)
				return true;
			return /^[0-9A-D#*xX.\[\]\-]+ \d{1,2}( [0-9*#+]{1,23})?$/.test(value.trim())
				? true : _('Expecting PATTERN STRIP PREFIX, e.g. [6-9]xxxxxxxxx 0 0');
		};

		o = s.taboption('dialing', form.Value, 'interdigit_timeout', _('Inter-digit timeout'),
			_('Seconds after the last dialled digit before a number that may still grow is called. Default: 4.'));
		o.datatype = 'range(1,60)';
		o.placeholder = '4';
		o.modalonly = true;

		o = s.taboption('dialing', form.Value, 'interdigit_timeout_long', _('Long inter-digit timeout'),
			_('Seconds to wait for the next digit while the number is not complete yet. Default: 16.'));
		o.datatype = 'range(1,60)';
		o.placeholder = '16';
		o.modalonly = true;

		o = s.taboption('dialing', form.Value, 'start_digit_timer', _('First digit timeout'),
			_('Seconds of dial tone before the first digit, after which the phone hears the off-hook tone. Default: 16.'));
		o.datatype = 'range(1,120)';
		o.placeholder = '16';
		o.modalonly = true;

		o = s.taboption('advanced', form.Flag, 'lec_enable', _('Echo cancellation'),
			_('Remove the echo of the phone line. Keep it on except for fax or modem lines.'));
		o.default = o.enabled;
		o.modalonly = true;

		o = s.taboption('advanced', form.Value, 'jitter_target', _('Jitter buffer'),
			_('Milliseconds of audio held back to smooth out a variable network delay, 10 to 250. Raise it when the audio breaks up. Default: 30.'));
		o.datatype = 'range(10,250)';
		o.placeholder = '30';
		o.modalonly = true;

		o = s.taboption('advanced', form.Flag, 'fax_t38_enable', _('T.38 fax'),
			_('Send fax as T.38 when the provider supports it; otherwise fax travels as audio.'));
		o.modalonly = true;

		o = s.taboption('advanced', form.Value, 'sip_registration_expiry', _('Registration expiry'),
			_('Seconds a registration lasts before it is renewed. Default: 3600.'));
		o.datatype = 'uinteger';
		o.placeholder = '3600';
		o.modalonly = true;

		o = s.taboption('advanced', form.Value, 'sip_local_port', _('Local SIP port'),
			_('Port of this router for SIP messages. Each line needs its own port. Default: 5060.'));
		o.datatype = 'port';
		o.placeholder = '5060';
		o.modalonly = true;

		s = m.section(form.GridSection, 'codec', _('Codecs'),
			_('Audio codecs offered to the provider, the lowest priority number first. Countries outside North America and Japan, India among them, normally put G.711 A-law first. Without any entry the lines offer G.711 \u00b5-law, G.711 A-law and G.729.'));
		s.anonymous = true;
		s.addremove = true;
		s.nodescriptions = true;

		o = s.option(form.ListValue, 'tr104_id', _('Codec'),
			_('Codecs the phone port hardware can encode.'));
		for (const c of codecs)
			if (supported.includes(c[0]))
				o.value(c[1], c[2]);
		o.rmempty = false;

		o = s.option(form.Flag, 'enabled', _('Enabled'),
			_('Offer this codec.'));
		o.default = o.enabled;
		o.editable = true;

		o = s.option(form.Value, 'priority', _('Priority'),
			_('Order of the offer: 1 is offered first. Codecs without a number come last.'));
		o.datatype = 'range(1,99)';
		o.rmempty = false;

		o = s.option(form.ListValue, 'channel', _('Line'),
			_('Limit this entry to one line, or apply it to every line.'));
		o.value('', _('All lines'));
		for (let i = 1; i <= ports; i++)
			o.value(i, _('Line %d').format(i));

		o = s.option(form.Flag, 'silence_suppression', _('Silence suppression'),
			_('Send no audio during pauses to save bandwidth. Some providers mute the call when it is on; leave it off unless the provider asks for it.'));
		o.modalonly = true;

		return m.render();
	}
});
