'use strict';
'require baseclass';
'require rpc';
'require network';

const callStatus = rpc.declare({
	object: 'xpon.public',
	method: 'status',
	expect: { '': {} }
});

return baseclass.extend({
	title: '',

	load: function() {
		return Promise.all([
			L.resolveDefault(callStatus(), {}),
			L.resolveDefault(network.getDevices(), [])
		]);
	},

	render: function(data) {
		var status = (data && data[0]) || {};
		var devs = (data && data[1]) || [];
		var pon0 = devs.filter(function(d) { return d.getName() == 'pon0'; })[0];
		var up = pon0 ? pon0.getCarrier() : false;
		var mode = status.mode;

		document.querySelectorAll('.ifacebox').forEach(function(box) {
			var head = box.querySelector('.ifacebox-head');
			var body = box.querySelector('.ifacebox-body');
			var img = body && body.querySelector('img');
			if (!head || !body || !img || head.textContent.trim() != 'pon0')
				return;

			img.src = L.resource('xpon/pon_%s.svg').format(up ? 'up' : 'down');
			var label = body.lastChild;
			if (!label || label.nodeType !== 3)
				return;

			if (!up) {
				label.textContent = _('no link');
			} else if (mode == 'gpon') {
				var span = E('span', {
					'title': _('GPON nominal optical line rate: 2.488 Gbit/s downstream, 1.244 Gbit/s upstream')
				}, [ '2.5\u202fGbE' ]);
				body.replaceChild(span, label);
			} else if (mode == 'epon') {
				var wspan = E('span', {
					'title': _('EPON nominal optical line rate: 1.25 Gbit/s both ways, 1 Gbit/s of data')
				}, [ '1\u202fGbE' ]);
				body.replaceChild(wspan, label);
			}
		});

		return null;
	}
});
