'use strict';
'require view';
'require form';
'require fs';
'require ui';
'require uci';
'require tools.widgets as widgets';

// SPDX-License-Identifier: GPL-2.0-only

const TEMPLATE = '/usr/share/cengarde/cloud-config.yaml';

// Same substitutions as "cengarde-setup cloud-config", on the values in the
// form, so the text follows unsaved changes.
function cloudConfig(template, secret, port, ipPass) {
	return template
		.replace(/^(\s+)REPLACE_SECRET$/m, '$1' + (secret || 'REPLACE_SECRET'))
		.replace(/^(\s*CENGARDE_PORT=).*$/m, '$1' + (port || '65500'))
		.replace(/^(\s*PASSTHROUGH=).*$/m, '$1' + (ipPass == '1' ? 'yes' : 'no'));
}

// The value in the form once it is on the page, the saved one before.
function value(map, name) {
	const found = map.lookupOption(name, 'main');

	if (found && found[0].getUIElement('main'))
		return found[0].formvalue('main');
	return uci.get('cengarde', 'main', name);
}

return view.extend({
	load: function() {
		return Promise.all([
			L.resolveDefault(fs.read(TEMPLATE), ''),
			L.resolveDefault(uci.load('upnpd'), null),
			uci.load('cengarde')
		]);
	},

	render: function(data) {
		const template = data[0];
		const hasUpnp = uci.get('upnpd', 'config') != null;
		let m, s, o, textarea;

		const refresh = function() {
			const on = function(name, dflt) { return (value(m, name) || dflt) == '1'; };

			if (textarea)
				textarea.value = cloudConfig(template, value(m, 'secret'), value(m, 'port'),
					on('ip_pass', '0') && on('tunnel', '1') && on('route_all', '1') ? '1' : '0');
		};

		m = new form.Map('cengarde', _('cengarde'),
			_('Sends every packet of a WireGuard tunnel to a VPS over all the selected uplinks at once, and the first copy to arrive wins: one uplink failing or lagging no longer drops the connection. Setup: copy the cloud-config from the VPS tab when creating the VPS, then enter its IP here, pick the uplinks and enable.'));

		s = m.section(form.NamedSection, 'main', 'cengarde');
		s.addremove = false;
		s.tab('general', _('General'));
		s.tab('vps', _('VPS'));
		s.tab('tunnel', _('Tunnel'));
		s.tab('advanced', _('Advanced'));

		o = s.taboption('general', form.Flag, 'enabled', _('Enabled'));
		o.rmempty = false;

		o = s.taboption('general', form.Value, 'server', _('VPS address'),
			_('Public IP of the VPS, as the provider shows it. An IP and not a name: with all traffic going through the tunnel, a name could not be resolved before the tunnel is up.'));
		o.datatype = 'ipaddr';
		o.placeholder = '203.0.113.10';

		o = s.taboption('general', form.Value, 'port', _('VPS port'),
			_('UDP port of cengarde on the VPS.'));
		o.datatype = 'port';
		o.placeholder = '65500';
		o.onchange = refresh;

		o = s.taboption('general', widgets.NetworkSelect, 'uplink', _('Uplinks'),
			_('The interfaces to bond: one per modem or VLAN, each with its own default route. cengarde gives them a metric of their own if they share one and puts them in the wan firewall zone.'));
		o.multiple = true;
		o.nocreate = true;
		o.filter = function(section_id, value) {
			return value != 'lan' && value != 'loopback' && value != 'wgcg';
		};

		o = s.taboption('vps', form.Value, 'secret', _('Pairing secret'),
			_('Every key of the tunnel derives from it: the VPS only needs this value, which goes in its cloud-config. A new secret means updating the VPS too.'));
		o.password = true;
		o.rmempty = false;
		o.validate = function(section_id, value) {
			return /^[A-Za-z0-9+/]{43}=$/.test(value) ? true :
				_('Expected the output of "cengarde genkey" (44 characters)');
		};
		o.onchange = refresh;
		o.renderWidget = function(section_id, option_index, cfgvalue) {
			const widget = form.Value.prototype.renderWidget.apply(this, [section_id, option_index, cfgvalue]);
			const opt = this;

			return E('div', { 'style': 'display:flex;flex-wrap:wrap;gap:.5em;align-items:center' }, [
				widget,
				E('button', {
					'class': 'cbi-button cbi-button-neutral',
					'click': ui.createHandlerFn(this, function(ev) {
						ev.preventDefault();
						if (!confirm(_('A VPS set up with the current secret will stop working until it gets the new one. Generate a new secret?')))
							return;
						return fs.exec_direct('/usr/sbin/cengarde', ['genkey']).then(function(out) {
							opt.getUIElement(section_id).setValue(out.trim());
							refresh();
						});
					})
				}, [ _('Generate') ])
			]);
		};

		o = s.taboption('vps', form.DummyValue, '_cloudconfig', _('VPS cloud-config'),
			_('Paste it as "user data" (cloud-init) when creating an Ubuntu 22.04 or 24.04 VPS, e.g. on Vultr. It builds the same cengarde version as this router and sets up WireGuard, NAT and IP pass. Setup log on the VPS: /var/log/cloud-init-output.log.'));
		o.write = function() {};
		o.remove = function() {};
		o.renderWidget = function(section_id) {
			textarea = E('textarea', {
				'class': 'cbi-input-textarea',
				'readonly': 'readonly',
				'wrap': 'off',
				'rows': 20,
				'style': 'width:100%;font-family:monospace;font-size:12px',
				'id': 'cengarde-cloud-config'
			});
			refresh();

			return E('div', {}, [
				textarea,
				E('div', { 'style': 'display:flex;gap:.5em;margin-top:.5em' }, [
					E('button', {
						'class': 'cbi-button cbi-button-action',
						'click': function(ev) {
							ev.preventDefault();
							textarea.select();
							if (navigator.clipboard && window.isSecureContext)
								navigator.clipboard.writeText(textarea.value);
							else
								document.execCommand('copy');
							ui.addTimeLimitedNotification(null, E('p', _('Copied to the clipboard.')), 3000, 'info');
						}
					}, [ _('Copy') ]),
					E('button', {
						'class': 'cbi-button cbi-button-neutral',
						'click': function(ev) {
							ev.preventDefault();
							const a = E('a', {
								'href': URL.createObjectURL(new Blob([ textarea.value ], { type: 'text/yaml' })),
								'download': 'cengarde-cloud-config.yaml'
							});
							document.body.appendChild(a);
							a.click();
							a.remove();
						}
					}, [ _('Download') ])
				])
			]);
		};

		o = s.taboption('tunnel', form.Flag, 'tunnel', _('Manage the WireGuard tunnel'),
			_('Creates the interface wgcg (10.79.0.2/30) with the keys derived from the secret, in the wan firewall zone. Off: set up WireGuard yourself, cengarde only listens on 127.0.0.1 at the VPS port.'));
		o.default = '1';
		o.rmempty = false;
		o.onchange = refresh;

		o = s.taboption('tunnel', form.Flag, 'route_all', _('Route all traffic through the tunnel'),
			_('Through routes more specific than the default ones, which the uplinks keep for cengarde itself.'));
		o.default = '1';
		o.rmempty = false;
		o.depends('tunnel', '1');
		o.onchange = refresh;

		o = s.taboption('tunnel', form.Flag, 'ip_pass', _('IP pass'),
			_('The VPS forwards TCP and UDP ports 1024-65000 to this router and UPnP hands them on to the LAN: devices and services here get the public IP of the VPS. The VPS follows this switch on its own, within a second of saving.') +
			(hasUpnp ? '' : '<br /><strong>' + _('Needs miniupnpd-nftables (and luci-app-upnp to see the mappings).') + '</strong>'));
		o.rmempty = false;
		o.depends({ tunnel: '1', route_all: '1' });
		o.onchange = refresh;

		o = s.taboption('tunnel', form.DynamicList, 'dns', _('DNS servers'),
			_('Used through the tunnel; the uplinks stop announcing their carriers\' DNS servers, which usually refuse queries coming from the VPS.'));
		o.datatype = 'ipaddr';
		o.placeholder = '1.1.1.1';
		o.depends('tunnel', '1');

		o = s.taboption('tunnel', form.Value, 'mtu', _('Tunnel MTU'));
		o.datatype = 'range(1280,1420)';
		o.placeholder = '1380';
		o.depends('tunnel', '1');

		o = s.taboption('advanced', form.Value, 'mute_behind_ms', _('Mute after falling behind (ms)'),
			_('A link that stays this far behind the fastest one for the settle time stops carrying traffic until it catches up. 0: never.'));
		o.datatype = 'range(0,60000)';
		o.placeholder = '150';

		o = s.taboption('advanced', form.Value, 'mute_settle_ms', _('Settle time (ms)'));
		o.datatype = 'range(100,600000)';
		o.placeholder = '2000';

		o = s.taboption('advanced', form.Value, 'min_active_links', _('Minimum active links'),
			_('Never mute below this many links.'));
		o.datatype = 'range(1,16)';
		o.placeholder = '2';

		o = s.taboption('advanced', form.Value, 'probe_interval_ms', _('Probe interval (ms)'),
			_('While there is traffic; each idle link is probed every second.'));
		o.datatype = 'range(100,60000)';
		o.placeholder = '100';

		o = s.taboption('advanced', form.Value, 'busy_poll_us', _('Busy polling (µs)'),
			_('Keeps polling this long after traffic instead of sleeping: lower latency, more CPU. 0: off.'));
		o.datatype = 'range(0,1000000)';
		o.placeholder = '0';

		o = s.taboption('advanced', form.ListValue, 'log_level', _('Log level'));
		o.value('error', _('Errors'));
		o.value('warn', _('Warnings'));
		o.value('info', _('Information'));
		o.value('debug', _('Debug'));
		o.default = 'info';

		return m.render();
	}
});
