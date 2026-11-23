'use strict';
'require view';
'require form';
'require uci';
'require xpon.widgets as widgets';

/* Field limits follow epon-oamd's buffers (src/epon-oamd.c, struct oam). */
function limitLength(o, max) {
	var renderWidget = o.renderWidget;

	o.renderWidget = function() {
		var node = renderWidget.apply(this, arguments);
		var input = node.querySelector ? node.querySelector('input') : null;

		if (input)
			input.setAttribute('maxlength', max);
		return node;
	};
}

function asciiField(o, max) {
	limitLength(o, max);
	o.validate = function(section_id, value) {
		if (!/^[\x20-\x7e]*$/.test(value))
			return _('Only printable ASCII characters are allowed');
		if (value.length > max)
			return _('At most %d characters').format(max);
		return true;
	};
}

function hexField(o, digits) {
	limitLength(o, digits);
	o.validate = function(section_id, value) {
		return !value || new RegExp('^[0-9a-fA-F]{' + digits + '}$').test(value) ||
			_('%d hexadecimal digits').format(digits);
	};
}

return view.extend({
	load: function() {
		return uci.load('epon');
	},

	render: function() {
		var m, s, o;

		m = new form.Map('epon', _('EPON OAM Configuration'),
			_('IEEE 802.3ah and China Telecom (CTC) extended OAM settings. Each option indicates when a saved change takes effect on the OLT.'));
		s = m.section(form.NamedSection, 'main', 'epon', _('Authentication'));
		o = s.option(form.Flag, 'enabled', _('Enable IEEE 802.3ah OAM'),
			_('Respond to OLT OAM discovery and provisioning. Without OAM, MPCP registration completes but user service remains down.'));
		o.default = '1';
		o = s.option(form.Flag, 'active_discovery', _('Active OAM Discovery'),
			_('Initiate IEEE 802.3ah OAM discovery instead of waiting for the OLT. Keep disabled unless OAM discovery stalls after MPCP registration.'));
		o.default = '0';
		o = s.option(form.Value, 'onu_mac', _('ONU MAC Address'),
			_('LLID 0 MAC address registered with the OLT. Leave empty to use the factory MAC address. Changes take effect on the next MPCP registration.'));
		o.datatype = 'macaddr';
		limitLength(o, 17);
		o = s.option(form.Value, 'loid', _('LOID'),
			_('Logical ONU ID provisioned on the OLT (up to 24 ASCII characters).'));
		asciiField(o, 24);
		o = s.option(form.Value, 'loid_password', _('LOID Password'),
			_('Logical ONU ID password (up to 12 ASCII characters).'));
		o.password = true;
		asciiField(o, 12);

		s = m.section(form.NamedSection, 'main', 'epon', _('LAN VLAN'));
		o = s.option(form.ListValue, 'vlan_mode', _('VLAN Mode'),
			_('OLT Managed applies the CTC Port VLAN mode from the OLT; Transparent passes frames unmodified; Tagged adds the VLAN ID below upstream and strips it downstream.'));
		o.value('olt', _('OLT Managed'));
		o.value('transparent', _('Transparent'));
		o.value('tag', _('Tagged'));
		o.default = 'olt';
		o = s.option(form.Value, 'vlan_id', _('VLAN ID'));
		o.datatype = 'range(1,4094)';
		o.depends('vlan_mode', 'tag');

		s = m.section(form.NamedSection, 'main', 'epon', _('ONU Identity'),
			_('Reported to the OLT via CTC ONU Serial Number attributes. Leave empty to use built-in defaults.'));
		o = s.option(form.Value, 'vendor_id', _('Vendor ID'), _('Up to 4 ASCII characters.'));
		asciiField(o, 4);
		o = s.option(form.Value, 'model', _('Model'), _('Up to 4 ASCII characters.'));
		asciiField(o, 4);
		o = s.option(form.Value, 'hardware_version', _('Hardware Version'), _('Up to 8 ASCII characters.'));
		asciiField(o, 8);
		o = s.option(form.Value, 'software_version', _('Software Version'), _('Up to 16 ASCII characters.'));
		asciiField(o, 16);
		o = s.option(form.Value, 'extended_model', _('Extended Model'),
			_('Reported on CTC 3.0 links (up to 16 ASCII characters).'));
		asciiField(o, 16);

		s = m.section(form.NamedSection, 'main', 'epon', _('Emulated ONU Identity'),
			_('Optional OAM identifiers copied from a replaced ONU. Leave empty to advertise this device\'s native identifiers.'));
		o = s.option(form.Value, 'oam_oui', _('OAM OUI'), _('6 hexadecimal digits (e.g. b48618).'));
		hexField(o, 6);
		o = s.option(form.Value, 'oam_vendor_info', _('OAM Vendor-Specific Info'), _('8 hexadecimal digits.'));
		hexField(o, 8);
		o = s.option(form.Value, 'chipset_id', _('CTC Chipset ID'), _('16 hexadecimal digits.'));
		hexField(o, 16);

		s = m.section(form.NamedSection, 'main', 'epon', _('VSOL Inter-Authentication'),
			_('Shared secret used by VSOL OLTs to derive the authentication key from the ONU MAC address. Leave empty unless required by the OLT.'));
		o = s.option(form.Value, 'interauth_secret', _('Shared Secret'), _('Up to 64 ASCII characters.'));
		o.password = true;
		asciiField(o, 64);

		s = m.section(form.NamedSection, 'main', 'epon', _('Logging'));
		o = s.option(form.ListValue, 'debug', _('Log Level'),
			_('Normal logs state transitions and errors; Debug also logs unsupported OLT requests, churning key updates, and VLAN changes.'));
		o.value('0', _('Normal'));
		o.value('1', _('Debug'));

		/*
		 * procd restarts epon-oamd when its command line changes, which every
		 * option but the ONU MAC is on; init writes that one to the MAC.
		 */
		m.children.forEach(function(section) {
			section.children.forEach(function(option) {
				widgets.annotate(option, option.option == 'onu_mac' ? 'next' : 'drop');
			});
		});

		return m.render().then(function(node) {
			var descr = node.querySelector('.cbi-map-descr');

			widgets.style();
			node.insertBefore(widgets.legend([ 'next', 'drop' ]),
					  descr ? descr.nextSibling : node.firstChild);
			return node;
		});
	}
});
