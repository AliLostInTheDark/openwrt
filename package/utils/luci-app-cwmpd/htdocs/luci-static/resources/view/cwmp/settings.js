'use strict';
'require view';
'require form';
'require fs';
'require poll';
'require rpc';
'require ui';
'require tools.widgets as widgets';

const callStatus = rpc.declare({ object: 'cwmp', method: 'status', expect: { '': {} } });
const callInform = rpc.declare({ object: 'cwmp', method: 'inform' });
const callBootstrap = rpc.declare({ object: 'cwmp', method: 'bootstrap' });

function when(ts) {
	return ts ? new Date(ts * 1000).toLocaleString() : _('never');
}

function statusTable(st) {
	const rows = [
		[ _('Client'), !st.enabled ? _('Disabled') : st.active ? _('In a session') : _('Idle') ],
		[ _('ACS'), st.acs ? '%s (%s)'.format(st.acs, {
			uci: _('configured'), omci: _('from the OLT over OMCI') }[st.source] || st.source) :
			_('none: set a URL or let the OLT provision one') ],
		[ _('Last session'), when(st.last_start) ],
		[ _('Last result'), st.last_result || '-' ],
		[ _('Failed sessions in a row'), String(st.retries ?? 0) ],
		[ _('Connection request URL'), st.connection_request_url || _('not offered') ],
		[ _('UDP connection request address'), st.udp_connection_request_address || _('not bound') ],
	];

	return E('table', { 'class': 'table' }, rows.map(r =>
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, r[0]),
			E('td', { 'class': 'td left' }, [ r[1] ])
		])));
}

/* The newest session log, as cwmpd wrote it: secrets are masked there. */
function lastLog() {
	return L.resolveDefault(fs.list('/tmp/cwmp'), []).then(files => {
		const logs = files.filter(f => /^session-\d+\.log$/.test(f.name))
			.map(f => f.name).sort();

		return logs.length ? L.resolveDefault(fs.read('/tmp/cwmp/' + logs[logs.length - 1]), '') : '';
	});
}

return view.extend({
	load() {
		return Promise.all([ L.resolveDefault(callStatus(), {}), lastLog() ]);
	},

	render([ st, log ]) {
		let m, s, o;

		const status = E('div', {}, statusTable(st));
		const logView = E('pre', { 'style': 'max-height:30em;overflow:auto;white-space:pre-wrap' },
			[ log || _('No session yet.') ]);

		poll.add(() => Promise.all([ L.resolveDefault(callStatus(), {}), lastLog() ]).then(([ st, log ]) => {
			status.replaceChildren(statusTable(st));
			logView.textContent = log || _('No session yet.');
		}), 5);

		m = new form.Map('cwmp');

		s = m.section(form.NamedSection, 'main', 'cwmp', _('Server'));
		s.tab('general', _('General Settings'));
		s.tab('network', _('Network'));
		s.tab('callback', _('Connection request'));
		s.tab('stun', _('STUN'));

		o = s.taboption('general', form.Flag, 'enabled', _('Enable'),
			_('Connect to the ACS at start-up, periodically and when asked.'));
		o.rmempty = false;

		o = s.taboption('general', form.Value, 'url', _('ACS URL'),
			_('Address of the ACS, <code>http://</code> or <code>https://</code>, e.g. <code>https://acs.example.net:7547/cwmp</code>. Empty uses the ACS the OLT provisions over OMCI, when it does.'));
		o.validate = function(section, value) {
			return !value || /^https?:\/\/[^\s/]+/.test(value) ? true :
				_('Expecting an http:// or https:// URL');
		};

		o = s.taboption('general', form.Value, 'username', _('Username'),
			_('Sent to the ACS when it asks for authentication (HTTP Basic or Digest).'));
		o = s.taboption('general', form.Value, 'password', _('Password'));
		o.password = true;

		o = s.taboption('general', form.Flag, 'omci_acs', _('Use the ACS from the OLT'),
			_('Without a URL above, take the ACS address and credentials the OLT provisions in the OMCI TR-069 management server.'));
		o.default = o.enabled;

		o = s.taboption('general', form.ListValue, 'data_model', _('Data model'),
			_('Root object tree advertised in Inform and parameter RPCs.'));
		o.value('tr181', _('TR-181 (Device.)'));
		o.value('tr098', _('TR-098 (InternetGatewayDevice.)'));
		o.default = 'tr181';

		o = s.taboption('general', form.Flag, 'persist_shadow', _('Persist ACS parameters'),
			_('Keep ACS-written shadow parameters across daemon restarts instead of resetting to defaults.'));

		o = s.taboption('general', form.Value, 'mock_model', _('Mock data model file'),
			_('Path to the TSV file of additional TR-069 parameters. Default: <code>/etc/cwmp/mock.tsv</code>.'));
		o.placeholder = '/etc/cwmp/mock.tsv';

		o = s.taboption('general', form.Flag, 'periodic_inform', _('Periodic inform'),
			_('Report to the ACS at a fixed interval as well as at start-up.'));
		o.default = o.enabled;

		o = s.taboption('general', form.Value, 'periodic_interval', _('Inform interval'),
			_('Seconds between periodic reports, at least 60. Default: 86400 (one day).'));
		o.datatype = 'min(60)';
		o.placeholder = '86400';
		o.depends('periodic_inform', '1');

		o = s.taboption('general', form.Flag, 'capture_downloads', _('Capture downloads'),
			_('Fetch the files the ACS asks this device to download (firmware, configuration) into <code>/tmp/cwmp/downloads</code> for inspection. They are never applied; the latest one is kept.'));
		o.default = o.enabled;

		o = s.taboption('general', form.Value, 'download_max_kib', _('Download size limit'),
			_('KiB kept of a captured download; it lives in RAM. Default: 32768.'));
		o.datatype = 'range(64,131072)';
		o.placeholder = '32768';
		o.depends('capture_downloads', '1');

		o = s.taboption('general', form.Value, 'mock_reboot_delay', _('Pretend reboot time'),
			_('Seconds after an ACS reboot or factory reset before the next Inform, as a real restart would take. Default: 60.'));
		o.datatype = 'range(0,600)';
		o.placeholder = '60';

		o = s.taboption('general', form.Value, 'log_sessions', _('Sessions kept'),
			_('How many session logs <code>/tmp/cwmp</code> keeps.'));
		o.datatype = 'range(1,200)';
		o.placeholder = '20';

		o = s.taboption('network', widgets.NetworkSelect, 'interface', _('Interface'),
			_('Network interface the ACS is reached through, e.g. <code>wan</code> or the operator\'s management VLAN. A host route to the ACS is added through it.'));
		o.nocreate = true;
		o.default = 'wan';

		o = s.taboption('network', form.ListValue, 'family', _('Address family'),
			_('<em>Dual stack</em> tries every address of the ACS, IPv6 and IPv4.'));
		o.value('any', _('Dual stack'));
		o.value('ipv4', _('IPv4 only'));
		o.value('ipv6', _('IPv6 only'));
		o.default = 'any';

		o = s.taboption('network', form.Flag, 'verify_cert', _('Verify the server certificate'),
			_('For an https:// ACS: refuse a certificate the CA bundle does not trust.'));
		o.default = o.enabled;

		o = s.taboption('callback', form.Flag, 'connection_request', _('Allow connection requests'),
			_('Let the ACS start a session by calling this device. Opens the port below on the interface\'s firewall zone.'));

		o = s.taboption('callback', form.Value, 'connection_request_port', _('Port'),
			_('TCP port of the connection request URL. Default: 7547.'));
		o.datatype = 'port';
		o.placeholder = '7547';
		o.depends('connection_request', '1');

		o = s.taboption('callback', form.Value, 'connection_request_path', _('Path'),
			_('Path the ACS requests, e.g. <code>/</code>.'));
		o.placeholder = '/';
		o.depends('connection_request', '1');

		o = s.taboption('callback', form.Value, 'connection_request_username', _('Username'),
			_('The ACS must authenticate with HTTP Digest. Empty accepts any caller: not recommended.'));
		o.depends('connection_request', '1');

		o = s.taboption('callback', form.Value, 'connection_request_password', _('Password'));
		o.password = true;
		o.depends('connection_request', '1');

		o = s.taboption('stun', form.Flag, 'stun_enable', _('Enable STUN (TR-111)'),
			_('Discover the public UDP address and keep a NAT binding open so the ACS can send UDP connection requests behind NAT.'));

		o = s.taboption('stun', form.Value, 'stun_server', _('STUN server'),
			_('Hostname or IP address of the STUN server, e.g. <code>stun.example.net</code>.'));
		o.placeholder = 'stun.example.net';
		o.depends('stun_enable', '1');

		o = s.taboption('stun', form.Value, 'stun_port', _('STUN port'),
			_('UDP port of the STUN server. Default: 3478.'));
		o.datatype = 'port';
		o.placeholder = '3478';
		o.depends('stun_enable', '1');

		o = s.taboption('stun', form.Value, 'stun_username', _('STUN username'));
		o.depends('stun_enable', '1');

		o = s.taboption('stun', form.Value, 'stun_password', _('STUN password'));
		o.password = true;
		o.depends('stun_enable', '1');

		o = s.taboption('stun', form.Value, 'stun_min_keepalive', _('Minimum keepalive (s)'),
			_('Minimum seconds between STUN binding keepalives. Default: 30.'));
		o.datatype = 'uinteger';
		o.placeholder = '30';
		o.depends('stun_enable', '1');

		o = s.taboption('stun', form.Value, 'stun_max_keepalive', _('Maximum keepalive (s)'),
			_('Maximum seconds between STUN binding keepalives. Default: 120.'));
		o.datatype = 'uinteger';
		o.placeholder = '120';
		o.depends('stun_enable', '1');

		s = m.section(form.NamedSection, 'device', 'device', _('Device identity'),
			_('What the Inform reports. Filled from the board on first boot; change only if the operator expects other values.'));

		o = s.option(form.Value, 'manufacturer', _('Manufacturer'));
		o = s.option(form.Value, 'oui', _('Manufacturer OUI'),
			_('Six hexadecimal digits, e.g. <code>A1B2C3</code>.'));
		o.validate = function(section, value) {
			return !value || /^[0-9A-Fa-f]{6}$/.test(value) ? true : _('Expecting six hexadecimal digits');
		};
		o = s.option(form.Value, 'product_class', _('Product class'));
		o = s.option(form.Value, 'serial', _('Serial number'));
		o = s.option(form.Value, 'hardware_version', _('Hardware version'));
		o = s.option(form.Value, 'software_version', _('Software version'));
		o = s.option(form.Value, 'provisioning_code', _('Provisioning code'));

		return m.render().then(mapEl => E([], [
			E('h2', {}, _('TR-069')),
			E('div', { 'class': 'cbi-map-descr' }, _('A TR-069 (CWMP) client that lets the operator\'s auto-configuration server (ACS) provision this device without changing anything on it. Settings the ACS writes are acknowledged and kept in memory only, so its later reads match, and they are logged in <code>/tmp/cwmp/captured.log</code> with passwords hidden. Downloads are captured and never applied; reboots and factory resets are acted out, a reset making the ACS provision from scratch. Uploads are refused.')),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Status')),
				status,
				E('div', { 'class': 'right' }, [
					E('button', {
						'class': 'cbi-button cbi-button-action',
						'click': ui.createHandlerFn(this, () => callInform())
					}, _('Inform now')),
					' ',
					E('button', {
						'class': 'cbi-button cbi-button-negative',
						'title': _('Forget what the ACS set and bootstrap again, as after a factory reset; nothing on this device changes'),
						'click': ui.createHandlerFn(this, () => callBootstrap())
					}, _('Re-provision'))
				])
			]),
			mapEl,
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Last session')),
				E('p', { 'class': 'cbi-section-descr' },
					_('Every message of the newest session, as exchanged. Values of password, key and secret parameters are masked.')),
				logView
			])
		]));
	}
});
