'use strict';
'require baseclass';
'require form';

return baseclass.extend({
	title: _('xPON Optical Telemetry Plugin Configuration'),
	description: _('The xpon plugin collects SFF-8472 optical transceiver DDM telemetry (RX/TX optical power, BOSA temperature, laser bias current, and supply voltage) through the hwmon device of the PON interface (/sys/class/net/<device>/device/hwmon).'),

	addFormOptions(s) {
		let o;

		o = s.option(form.Flag, 'enable', _('Enable this plugin'));
		o.default = '0';

		o = s.option(form.Value, 'Device', _('PON interface'));
		o.default = 'pon0';
		o.placeholder = 'pon0';
		o.optional = true;
		o.depends('enable', '1');
	},

	configSummary(section) {
		return _('Monitoring xPON optical telemetry on %s').format(section.Device || 'pon0');
	}
});
