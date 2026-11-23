/* Licensed to the public under the Apache License 2.0. */

'use strict';
'require baseclass';

return baseclass.extend({
	title: _('xPON Optics'),

	rrdargs(graph, host, plugin, plugin_instance, dtype) {
		const rv = [];
		const types = graph.dataTypes(host, plugin, plugin_instance);

		if (types.indexOf('power') > -1) {
			rv.push({
				title: "%H: Optical power on %pi",
				vlabel: "dBm",
				number_format: "%5.2lf dBm",
				alt_autoscale: true,
				data: {
					types: [ "power" ],
					instances: {
						power: [ "rx", "tx" ]
					},
					options: {
						power_rx: {
							title: "1490nm (RX)",
							color: "0000ff",
							transform_rpn: "1000,*,LOG,10,LOG,/,10,*",
							noarea: true,
							overlay: true,
							weight: 1
						},
						power_tx: {
							title: "1310nm (TX)",
							color: "00aa00",
							transform_rpn: "1000,*,LOG,10,LOG,/,10,*",
							noarea: true,
							overlay: true,
							weight: 2
						}
					}
				}
			});
		}

		if (types.indexOf('temperature') > -1) {
			rv.push({
				title: "%H: BOSA temperature on %pi",
				vlabel: "\xb0C",
				number_format: "%4.1lf \xb0C",
				alt_autoscale: true,
				data: {
					types: [ "temperature" ],
					options: {
						temperature: {
							title: "Temperature",
							color: "ff0000",
							noarea: true
						}
					}
				}
			});
		}

		if (types.indexOf('current') > -1) {
			rv.push({
				detail: true,
				title: "%H: Laser bias current on %pi",
				vlabel: "mA",
				number_format: "%5.2lf mA",
				alt_autoscale: true,
				data: {
					types: [ "current" ],
					options: {
						current: {
							title: "Bias current",
							color: "ff8800",
							transform_rpn: "1000,*",
							noarea: true
						}
					}
				}
			});
		}

		if (types.indexOf('voltage') > -1) {
			rv.push({
				detail: true,
				title: "%H: BOSA supply voltage on %pi",
				vlabel: "V",
				number_format: "%4.2lf V",
				alt_autoscale: true,
				data: {
					types: [ "voltage" ],
					options: {
						voltage: {
							title: "Supply voltage",
							color: "8800ff",
							noarea: true
						}
					}
				}
			});
		}

		return rv;
	}
});
