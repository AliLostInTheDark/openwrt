'use strict';
'require view';
'require form';
'require fs';
'require tools.widgets as widgets';

/* region files are named by ISO 3166 alpha-3; the option takes alpha-2 too */
const regionNames = {
	AUS: [ 'AU', _('Australia') ],
	AUT: [ 'AT', _('Austria') ],
	ETS: [ 'EU', _('Europe (ETSI)') ],
	GBR: [ 'GB', _('United Kingdom') ],
	IND: [ 'IN', _('India') ],
	NZL: [ 'NZ', _('New Zealand') ],
	SRB: [ 'RS', _('Serbia') ],
	USA: [ 'US', _('United States') ]
};

return view.extend({
	load() {
		return L.resolveDefault(fs.list('/usr/share/udsp/regions'), []);
	},

	render(regions) {
		let m, s, o;

		m = new form.Map('voip', _('Telephony'),
			_('Settings of the voice service that drives the analogue phone ports. The SIP accounts are set per line on the Lines page.'));

		s = m.section(form.NamedSection, 'base', 'base', _('Service'));
		s.tab('general', _('General Settings'));
		s.tab('network', _('Network'));
		s.tab('sip', _('SIP'));

		o = s.taboption('general', form.Flag, 'enabled', _('Enable'),
			_('Power the phone ports and handle calls. When disabled the ports stay without line voltage.'));
		o.rmempty = false;

		o = s.taboption('general', form.Value, 'region', _('Region'),
			_('Country of the call progress tones (dial, busy, ringback), the ring cadence and the caller ID format. Must match the country of the phone and the provider.'));
		for (const f of regions) {
			const iso3 = f.name.replace(/\.conf$/, '');

			if (f.type != 'file' || iso3 == f.name)
				continue;

			if (regionNames[iso3])
				o.value(regionNames[iso3][0], '%s (%s)'.format(regionNames[iso3][1], regionNames[iso3][0]));
			else
				o.value(iso3);
		}
		o.default = 'US';

		o = s.taboption('general', form.ListValue, 'protocol', _('Signalling protocol'),
			_('SIP for internet telephony providers and IMS voice over fibre; MGCP only for operators that provision their lines with it.'));
		o.value('sip', 'SIP');
		o.value('mgcp', 'MGCP');
		o.default = 'sip';

		o = s.taboption('general', form.Flag, 'mwi_subscribe', _('Message waiting'),
			_('Ask the provider for voicemail notifications and show them with the stutter dial tone.'));

		o = s.taboption('network', widgets.NetworkSelect, 'local_interface', _('Interface'),
			_('Network interface the calls are made through, usually the WAN or a dedicated voice VLAN of the provider. Not set means <code>wan</code>.'));
		o.nocreate = true;

		o = s.taboption('network', form.Flag, 'ipv6_enabled', _('Use IPv6'),
			_('Register and send audio over the global IPv6 address of the interface. Enable it only when the provider expects IPv6 voice.'));

		o = s.taboption('network', form.ListValue, 'restrict_source', _('Inbound traffic'),
			_('<em>Keep-alive pinhole</em> keeps the firewall closed: the service refreshes the connection to the provider and replies pass as established traffic. The open modes need firewall rules for the SIP and RTP ports.'));
		o.value('2', _('Keep-alive pinhole (recommended)'));
		o.value('1', _('Open to the provider addresses'));
		o.value('0', _('Open to any address'));
		o.default = '2';

		o = s.taboption('sip', form.ListValue, 'transport', _('Transport'),
			_('How SIP messages travel to the provider. UDP unless the provider asks for TCP or TLS.'));
		o.value('udp', 'UDP');
		o.value('tcp', 'TCP');
		o.value('tls', _('TLS (encrypted)'));
		o.default = 'udp';

		o = s.taboption('sip', form.Flag, 'tls_server_verification_enabled', _('Verify the server certificate'),
			_('Reject a provider whose TLS certificate cannot be verified.'));
		o.depends('transport', 'tls');

		o = s.taboption('sip', form.Flag, 'srtp_enabled', _('Encrypt audio (SRTP)'),
			_('Only when the provider supports SRTP; calls fail otherwise.'));

		o = s.taboption('sip', form.Flag, 'keepalive_enabled', _('NAT keep-alive'),
			_('Send small packets to the provider so that the router keeps the connection open.'));
		o.default = o.enabled;

		o = s.taboption('sip', form.Value, 'keepalive_interval', _('Keep-alive interval'),
			_('Seconds between keep-alive packets, 1 to 3600. Default: 30.'));
		o.datatype = 'range(1,3600)';
		o.placeholder = '30';
		o.depends('keepalive_enabled', '1');

		o = s.taboption('sip', form.ListValue, 'dns_srv_mode', _('DNS SRV lookup'),
			_('Find the provider servers through DNS SRV records. <em>Automatic</em> uses them when the server is a name without a port.'));
		o.value('auto', _('Automatic'));
		o.value('always', _('Always'));
		o.value('never', _('Never'));
		o.default = 'auto';

		o = s.taboption('sip', form.Flag, 'rtcp_enabled', _('RTCP'),
			_('Send call quality reports alongside the audio.'));
		o.default = o.enabled;

		s = m.section(form.NamedSection, 'hal', 'hal', _('Advanced: phone port hardware'),
			_('Set by the board defaults. Change these only when porting the voice service to new hardware: a wrong value leaves the phone ports without line voltage until it is corrected.'));
		s.addremove = false;

		o = s.option(form.Value, 'slic_plugin', _('SLIC plug-in'),
			_('Path of the driver for the line interface chip, e.g. <code>/usr/lib/udsp/udsp-slic-dxs.so</code>.'));

		o = s.option(form.Value, 'spidev', _('SPI device'),
			_('Control bus of the line interface chip, e.g. <code>/dev/spidev1.0</code>.'));

		o = s.option(form.Value, 'tdm_playback', _('PCM playback device'),
			_('ALSA device for the audio to the phone, e.g. <code>hw:0,0</code>.'));

		o = s.option(form.Value, 'tdm_capture', _('PCM capture device'),
			_('ALSA device for the audio from the phone, e.g. <code>hw:0,0</code>.'));

		o = s.option(form.Value, 'channels', _('PCM channels'),
			_('Audio channels the PCM device carries, 1 or 2.'));
		o.datatype = 'range(1,2)';

		o = s.option(form.ListValue, 'sample_rate', _('Sample rate'),
			_('Wideband needs a line interface chip and plug-in that support it.'));
		o.value('8000', _('8 kHz (narrowband)'));
		o.value('16000', _('16 kHz (wideband)'));
		o.default = '16000';

		o = s.option(form.Value, 'pcm_offset_bits', _('PCM data offset'),
			_('Bit clocks between the frame sync and the first data bit, 0 to 15. Empty or 0 uses the plug-in default.'));
		o.datatype = 'range(0,15)';

		o = s.option(form.Value, 'led', _('Phone LED'),
			_('LED under <code>/sys/class/leds</code> that shows the phone port: on when ready, slow blink off hook or in a call, fast blink while ringing, off when the service or the registration is down. E.g. <code>green:tel</code>; empty leaves the LED alone.'));

		o = s.option(form.Value, 'gpio_reset', _('Reset GPIO'),
			_('Name of the exported GPIO under <code>/sys/class/gpio</code> that resets the chip. Default: <code>slic_reset</code>.'));

		return m.render();
	}
});
