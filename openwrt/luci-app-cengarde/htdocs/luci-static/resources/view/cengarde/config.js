'use strict';
'require view';
'require form';
'require fs';
'require ui';
'require uci';
'require tools.widgets as widgets';

// SPDX-License-Identifier: GPL-2.0-only

const TEMPLATE = '/usr/share/cengarde/cloud-config.yaml';
const MAX_SERVERS = 8; // the engine's CG_MAX_SERVERS
// Largest tunnel MTU in a 1500-byte path: 1500 - (IP, UDP, cengarde and
// WireGuard headers), 20 + 8 + 24 + 32 over IPv4, 40 + 8 + 24 + 32 over IPv6.
const MTU_IPV6 = 1396;

// A router's name on the VPS and an address in the tunnel, as the VPS and
// cengarde-setup take them: cg-NAME has to fit in an interface name, and
// 10.79.0.1 is the VPS's own.
function isName(name) {
	return /^[a-z0-9][a-z0-9-]{0,11}$/.test(name) && name != 'default';
}

function isTunnelAddr(addr) {
	const m = /^10\.79\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})$/.exec(addr);
	const n = m ? +m[1] * 256 + +m[2] : -1;

	return m != null && +m[1] <= 255 && +m[2] <= 255 && n >= 2 && n <= 65534;
}

// An entry of the split weights, as cengarde-setup takes it: NAME=WEIGHT, a
// whole number from 0 to 1000 without leading zeros.
function splitWeight(entry) {
	const m = /^([^=\s]+)=(0|[1-9][0-9]{0,2}|1000)$/.exec(String(entry));

	return m ? { uplink: m[1], weight: +m[2] } : null;
}

// Whether an entry can go in the list of split weights (true), or why not: it
// has the form above, its uplink is one of the uplinks of the router, and the
// others (the entries already in the list) give that uplink no weight yet.
// The list's value holds the entry being typed too once it is valid (LuCI's
// DynamicList adds the pending input to getValue()), so an entry equal to it
// is that entry, not another one: LuCI never lists the same entry twice.
function checkSplitWeight(entry, uplinks, others) {
	const w = splitWeight(entry);

	if (!w)
		return _('Expected NAME=WEIGHT, with a weight from 0 to 1000');
	if (uplinks.indexOf(w.uplink) < 0)
		return _('%s is not one of the uplinks (General tab)').format(w.uplink);
	if (others.some(function(e) { const o = e !== entry ? splitWeight(e) : null; return o != null && o.uplink == w.uplink; }))
		return _('%s already has a weight').format(w.uplink);
	return true;
}

// Same substitutions as "cengarde-setup cloud-config", on the values in the
// form, so the text follows unsaved changes. Without an address (a new secret
// that is not saved yet) its placeholder stays, and the VPS derives it.
function cloudConfig(template, secret, port, ipPass, ipv6, name, addr) {
	return template
		.replace(/^(\s+)REPLACE_SECRET$/m, '$1' + (secret || 'REPLACE_SECRET'))
		.replace(/^(\s*CENGARDE_PORT=).*$/m, '$1' + (port || '65500'))
		.replace(/^(\s*PASSTHROUGH=).*$/m, '$1' + (ipPass == '1' ? 'yes' : 'no'))
		.replace(/^(\s*TUNNEL_IPV6=).*$/m, '$1' + (ipv6 == '1' ? 'yes' : 'no'))
		.replace(/^(\s*ROUTER_NAME=).*$/m, '$1' + (isName(name) ? name : 'router'))
		.replace(/^(\s*ROUTER_TUNNEL_ADDR=).*$/m, '$1' + (addr || 'REPLACE_TUNNEL_ADDR'));
}

function copyField(field) {
	field.select();
	if (navigator.clipboard && window.isSecureContext)
		navigator.clipboard.writeText(field.value);
	else
		document.execCommand('copy');
	ui.addTimeLimitedNotification(null, E('p', _('Copied to the clipboard.')), 3000, 'info');
}

// The value in the form once it is on the page, the saved one before.
function value(map, name) {
	const found = map.lookupOption(name, 'main');

	if (found && found[0].getUIElement('main'))
		return found[0].formvalue('main');
	return uci.get('cengarde', 'main', name);
}

// The engine's protocol and the address in the tunnel derived from the saved
// secret (tunnel_addr_derived), which the browser cannot compute (BLAKE2s).
function readStatus() {
	return L.resolveDefault(fs.exec_direct('/usr/sbin/cengarde-setup', [ 'status' ], 'json'), null);
}

function isIPv6(addr) {
	return String(addr).indexOf(':') >= 0;
}

return view.extend({
	load: function() {
		return Promise.all([
			L.resolveDefault(fs.read(TEMPLATE), ''),
			L.resolveDefault(uci.load('upnpd'), null),
			L.resolveDefault(uci.load('network'), null),
			uci.load('cengarde'),
			readStatus()
		]);
	},

	render: function(data) {
		const template = data[0];
		const hasUpnp = uci.get('upnpd', 'config') != null;
		let status = data[4] || {};
		let savedSecret = uci.get('cengarde', 'main', 'secret') || '';
		let m, s, o, textarea, addCommand, addNote;

		const text = function(name) { return String(value(m, name) || '').trim(); };

		// This router's address in the tunnel as the form stands: the one set,
		// else the one derived from the saved secret; null when it is not known
		// (a new secret that is not saved yet, or an address that is not valid).
		const tunnelAddr = function() {
			const set = text('tunnel_addr');

			if (set)
				return isTunnelAddr(set) ? set : null;
			return (status.tunnel_addr_derived && text('secret') == savedSecret) ? status.tunnel_addr_derived : null;
		};

		const refresh = function() {
			const on = function(name, dflt) { return (value(m, name) || dflt) == '1'; };
			const name = text('vps_name') || 'router';
			const addr = tunnelAddr();

			if (textarea)
				textarea.value = cloudConfig(template, value(m, 'secret'), value(m, 'port'),
					on('ip_pass', '0') && on('tunnel', '1') && on('route_all', '1') ? '1' : '0',
					on('tunnel_ipv6', '0') && on('tunnel', '1') && on('route_all', '1') ? '1' : '0', name, addr);
			if (addCommand) {
				const ready = addr != null && isName(name);

				addCommand.value = !ready ? '' : 'sudo cengarde-vps-setup add ' + name +
					' --tunnel-addr ' + addr + (status.protocol ? ' --proto ' + status.protocol : '');
				addNote.style.display = ready ? 'none' : '';
			}
		};

		// A new secret changes the derived address, which only the saved
		// configuration can tell: ask again once the changes are applied.
		document.addEventListener('uci-applied', function() {
			readStatus().then(function(st) {
				const input = document.getElementById('widget.cbid.cengarde.main.tunnel_addr');

				status = st || {};
				savedSecret = uci.get('cengarde', 'main', 'secret') || '';
				if (input)
					input.placeholder = status.tunnel_addr_derived || '';
				refresh();
			});
		});

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

		o = s.taboption('general', form.DynamicList, 'server', _('VPS addresses'),
			_('Public IPs of the VPS as the provider shows them, IPv4 and IPv6, in order of preference: each uplink uses the first one of a family it has, and tries the next one when the VPS does not answer there. A name works too: the router looks it up through each uplink (the carrier\'s DNS first), never through the tunnel, and follows it when its IP changes (dynamic DNS). An MTU above 1396 does not fit if the name has an IPv6 address that the uplinks use.'));
		o.datatype = 'host(0)'; // a name or an IP, without a mask
		o.placeholder = '203.0.113.10';
		o.validate = function(section_id) {
			return (L.toArray(this.formvalue(section_id)).length > MAX_SERVERS)
				? _('At most %d addresses').format(MAX_SERVERS) : true;
		};

		o = s.taboption('general', form.Value, 'port', _('VPS port'),
			_('UDP port of cengarde on the VPS.'));
		o.datatype = 'port';
		o.placeholder = '65500';
		o.onchange = refresh;

		o = s.taboption('general', widgets.NetworkSelect, 'uplink', _('Uplinks'),
			_('The interfaces to bond: one per modem or VLAN, each with its own default route. cengarde gives them a metric of their own if they share one and puts them in the wan firewall zone.'));
		o.multiple = true;
		o.nocreate = true;
		// Not the IPv6 companions (cengarde's own, or any on top of another
		// interface), unless one was chosen already.
		o.filter = function(section_id, value) {
			const chosen = L.toArray(uci.get('cengarde', 'main', 'uplink')).indexOf(value) >= 0;
			const companion = uci.get('network', value, 'cengarde_owned') == '1' ||
				String(uci.get('network', value, 'device') || '').charAt(0) == '@';

			return value != 'lan' && value != 'loopback' && value != 'wgcg' && (chosen || !companion);
		};

		o = s.taboption('vps', form.Value, 'secret', _('Pairing secret'),
			_('Every key of the tunnel, and its address unless you set one, derives from it: the VPS only needs this value, which goes in its cloud-config. A new secret means updating the VPS too.'));
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

		o = s.taboption('vps', form.Value, 'vps_name', _('Name on the VPS'),
			_('How a VPS that serves several routers calls this one: 1 to 12 characters of a-z, 0-9 and -, not starting with - and not "default". Each router of a VPS needs its own. Empty: router.'));
		o.placeholder = 'router';
		o.onchange = refresh;
		o.validate = function(section_id, v) {
			return (!v || isName(v)) ? true :
				_('1 to 12 characters of a-z, 0-9 and -, not starting with - and not "default"');
		};

		o = s.taboption('vps', form.Value, 'tunnel_addr', _('Address in the tunnel'),
			_('The address of this router inside the tunnel, 10.79.X.Y from 10.79.0.2 to 10.79.255.254; each router of a VPS needs its own. Empty: the one derived from the saved secret, which the field shows in grey (after a new secret, save and apply to see it). A router installed before this version keeps 10.79.0.2: changing it means telling the VPS too.'));
		o.placeholder = status.tunnel_addr_derived || '';
		o.onchange = refresh;
		o.validate = function(section_id, v) {
			return (!v || isTunnelAddr(v)) ? true : _('Expected 10.79.X.Y, from 10.79.0.2 to 10.79.255.254');
		};

		o = s.taboption('vps', form.DummyValue, '_cloudconfig', _('VPS cloud-config'),
			_('For a new VPS. Paste it as "user data" (cloud-init) when creating an Ubuntu 22.04 or 24.04 VPS, e.g. on Vultr. It builds the same cengarde version as this router and sets up WireGuard, NAT and IP pass, with the name and address of this router. Setup log on the VPS: /var/log/cloud-init-output.log.'));
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
							copyField(textarea);
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

		o = s.taboption('vps', form.DummyValue, '_addcommand', _('This router on a VPS that already serves others'),
			_('Instead of the cloud-config, run this on the VPS over SSH: it adds this router next to the others, with its own name and address in the tunnel. When it asks, paste the pairing secret shown above: it is never part of the command.'));
		o.write = function() {};
		o.remove = function() {};
		o.renderWidget = function(section_id) {
			addCommand = E('input', {
				'class': 'cbi-input-text',
				'type': 'text',
				'readonly': 'readonly',
				'style': 'width:100%;font-family:monospace;font-size:12px',
				'id': 'cengarde-add-command'
			});
			addNote = E('div', { 'class': 'cbi-value-description', 'id': 'cengarde-add-note' }, [
				_('The command shows once the name and the address in the tunnel are valid and known: after a new secret, save and apply first.')
			]);
			refresh();

			return E('div', {}, [
				addCommand,
				addNote,
				E('div', { 'style': 'margin-top:.5em' }, [
					E('button', {
						'class': 'cbi-button cbi-button-action',
						'click': function(ev) {
							ev.preventDefault();
							if (addCommand.value)
								copyField(addCommand);
						}
					}, [ _('Copy') ])
				])
			]);
		};

		o = s.taboption('tunnel', form.Flag, 'tunnel', _('Manage the WireGuard tunnel'),
			_('Creates the interface wgcg with the keys derived from the secret and this router\'s address in the tunnel (VPS tab), in the wan firewall zone. Off: set up WireGuard yourself, cengarde only listens on 127.0.0.1 at the VPS port.'));
		o.default = '1';
		o.rmempty = false;
		o.onchange = refresh;

		o = s.taboption('tunnel', form.Flag, 'route_all', _('Route all traffic through the tunnel'),
			_('Through routes more specific than the default ones, which the uplinks keep for cengarde itself.'));
		o.default = '1';
		o.rmempty = false;
		o.depends('tunnel', '1');
		o.onchange = refresh;

		o = s.taboption('tunnel', form.Flag, 'tunnel_ipv6', _('IPv6 through the tunnel'),
			_('Carries IPv6 through the tunnel too, translated (NAT66) here and at the VPS: this router gets a ULA address in the tunnel, derived from the secret, and with "Route all traffic through the tunnel" the IPv6 of the router and of the LAN leaves through the VPS. The VPS needs IPv6 of its own and the command "sudo cengarde-vps-setup ipv6 on" (the cloud-config of a new VPS follows this switch when all traffic goes through the tunnel). The LAN needs a ULA prefix (Network > Interfaces > Global network options); its devices have only that, so they prefer IPv4 for sites that have both, and this serves mainly IPv6-only sites. Off until it has been tested on real hardware.'));
		o.rmempty = false;
		o.depends('tunnel', '1');
		o.onchange = refresh;

		o = s.taboption('tunnel', form.Flag, 'ip_pass', _('IP pass'),
			_('The VPS forwards TCP and UDP ports 1024-65000 to this router and UPnP hands them on to the LAN: devices and services here get the public IP of the VPS. The VPS follows this switch on its own, within a second of saving.') +
			(hasUpnp ? '' : '<br /><strong>' + _('Needs miniupnpd-nftables (and luci-app-upnp to see the mappings).') + '</strong>'));
		o.rmempty = false;
		o.depends({ tunnel: '1', route_all: '1' });
		o.onchange = refresh;

		o = s.taboption('tunnel', form.Value, 'stun_host', _('STUN server'),
			_('Only for IP pass when no VPS address is IPv4: UPnP then learns the public IPv4 of the VPS from this STUN server, through the tunnel (e.g. stun.cloudflare.com). Empty: UPnP announces no public IP and hands no ports on to the LAN.'));
		o.datatype = 'host(1)'; // a name or IPv4: miniupnpd asks over IPv4
		o.depends('ip_pass', '1');

		o = s.taboption('tunnel', form.Value, 'stun_port', _('STUN port'));
		o.datatype = 'port';
		o.placeholder = '3478';
		o.depends('ip_pass', '1');

		o = s.taboption('tunnel', form.DynamicList, 'dns', _('DNS servers'),
			_('Used through the tunnel; the uplinks stop announcing their carriers\' DNS servers, which usually refuse queries coming from the VPS.'));
		o.datatype = 'ipaddr(1)';
		o.placeholder = '1.1.1.1';
		o.depends('tunnel', '1');
		o.renderWidget = function(section_id, option_index, cfgvalue) {
			const widget = form.DynamicList.prototype.renderWidget.apply(this, [section_id, option_index, cfgvalue]);

			this.warning = E('div', { 'class': 'cbi-value-description', 'style': 'display:none;color:#c33' }, [
				_('IPv6 servers are left out while IPv6 does not go through the tunnel: the router would reach them around it. Without an IPv4 server, 1.1.1.1 and 9.9.9.9 are used.')
			]);
			return E('div', {}, [ widget, this.warning ]);
		};
		// A warning, not an error: the list is kept for when IPv6 goes
		// through the tunnel.
		o.validate = function(section_id) {
			if (this.warning)
				this.warning.style.display = L.toArray(this.formvalue(section_id)).some(isIPv6) ? '' : 'none';
			return true;
		};

		o = s.taboption('tunnel', form.Value, 'mtu', _('Tunnel MTU'),
			_('In a 1500-byte path, packets of up to 1416 bytes fit over IPv4 and of up to 1396 over IPv6. Larger ones get fragmented, and mobile networks often drop the fragments.'));
		o.datatype = 'range(1280,1416)';
		o.placeholder = '1380';
		o.depends('tunnel', '1');
		// The form checks every field again on each change, so this follows
		// the VPS addresses too.
		o.validate = function(section_id, v) {
			return (+v > MTU_IPV6 && L.toArray(value(m, 'server')).some(isIPv6))
				? _('At most %d with an IPv6 VPS address').format(MTU_IPV6) : true;
		};

		o = s.taboption('advanced', form.Flag, 'uplink_ipv6', _('IPv6 on the uplinks'),
			_('Adds a DHCPv6 interface on top of each DHCP or static uplink (named after it, ending in 6), so that cengarde can reach the VPS over the IPv6 of the modems. Their IPv6 prefixes never reach the LAN.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('advanced', form.Value, 'server_failover_ms', _('Next VPS address after (ms)'),
			_('An uplink that gets no answer from the VPS for this long tries the next VPS address it can use, IPv4 or IPv6, in the list\'s order. 0: never.'));
		o.datatype = 'range(0,3600000)';
		o.placeholder = '10000';
		// The engine wants 0 or at least three idle probes: probe_idle_ms
		// (1000 unless set in UCI), raised to the probe interval when larger.
		o.validate = function(section_id, v) {
			const idle = Math.max(+value(m, 'probe_idle_ms') || 1000, +value(m, 'probe_interval_ms') || 100);

			return (+v && +v < 3 * idle)
				? _('0 (never) or at least %d ms with this probe interval').format(3 * idle) : true;
		};

		o = s.taboption('advanced', form.Value, 'mute_behind_ms', _('Standby after falling behind (ms)'),
			_('An uplink that stays this far behind the fastest one in a direction for the settle time goes on standby that way (probes only) until it catches up; the other direction is decided apart. 0: never.'));
		o.datatype = 'range(0,60000)';
		o.placeholder = '150';

		o = s.taboption('advanced', form.Value, 'mute_settle_ms', _('Settle time (ms)'));
		o.datatype = 'range(100,600000)';
		o.placeholder = '2000';

		o = s.taboption('advanced', form.Value, 'min_active_links', _('Minimum active links'),
			_('No uplink goes on standby if that would leave fewer than this many in use.'));
		o.datatype = 'range(1,16)';
		o.placeholder = '2';

		o = s.taboption('advanced', form.Flag, 'reorder', _('Reorder at the receiver'),
			_('The end that receives holds a packet that arrives ahead of a missing one until the missing one comes or its time runs out, and hands them to WireGuard in order. The VPS does the same with what it receives. While every packet is copied over all the uplinks nothing is held, so it changes nothing and costs almost nothing; it is what makes the split below usable.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('advanced', form.Value, 'reorder_max_wait_ms', _('Longest wait for a missing packet (ms)'),
			_('How long a packet that arrived early waits for the missing one before it is handed on anyway. It waits less when the slowest uplink would have delivered the missing one by then. A longer wait copes with slower uplinks, and adds delay when a packet is really lost.'));
		o.datatype = 'range(0,2000)';
		o.placeholder = '200';
		o.depends('reorder', '1');

		o = s.taboption('advanced', form.Value, 'reorder_bypass', _('Packets never held, up to (bytes)'),
			_('WireGuard packets of up to this size (keepalives, empty TCP ACKs) are handed on at once, and so are the handshakes. 0: only the handshakes. A larger value lets small data packets overtake the big ones that are held, which TCP and QUIC can take for losses.'));
		o.datatype = 'range(0,2048)';
		o.placeholder = '128';
		o.depends('reorder', '1');

		o = s.taboption('advanced', form.Flag, 'split', _('Split large packets across the uplinks (experimental)'),
			_('Instead of a copy over every uplink, each large packet (over 256 bytes, by default) goes over one uplink, chosen by weight; the small ones and the handshakes still go over all of them. This adds up the speed of the uplinks, but today it does worse than copying: an uplink that dies keeps its share of the packets until the engine takes it out (about 300 ms by estimate, 1.5 s before that), and the packets sent to it in the meantime are lost; the slowest uplink in the split sets the latency of all (see the largest delay difference below); and a weight above what an uplink can carry fills the queue of its modem and makes the uplink unstable. The VPS follows this router: it splits the download the same way, with the same weights, when the router asks for it, and needs no setting. It needs "Reorder at the receiver".'));
		o.rmempty = false;
		o.depends('reorder', '1');

		o = s.taboption('advanced', form.DynamicList, 'split_weight', _('Weight of each uplink in the split'),
			_('One entry per uplink, as NAME=WEIGHT (for example wan=3), with NAME as in the list of uplinks of the General tab and a weight from 0 to 1000. An uplink gets a share of the split packets in proportion to its weight; with no entry its weight is 1. 0: the uplink carries a copy of every packet, split or not. The weights do not follow the speed of the uplinks: choose them in proportion to what each one carries.'));
		o.placeholder = 'wan=2';
		o.depends({ reorder: '1', split: '1' });
		// LuCI calls this with the entry being typed, and with '' for the list
		// itself (its element has no value of its own) on each change of the
		// list and when the page is saved. The entry: its weight, an uplink of
		// this router, and not one that has a weight already. The list: the
		// same for each entry, the first wrong one says why (one saved before
		// its uplink was removed, a repeat set through uci).
		o.validate = function(section_id, v) {
			const uplinks = L.toArray(value(m, 'uplink')), list = L.toArray(this.formvalue(section_id));

			if (v != null && v !== '')
				return checkSplitWeight(v, uplinks, list);
			for (let i = 0; i < list.length; i++) {
				const r = checkSplitWeight(list[i], uplinks, list);

				if (r !== true)
					return r;
			}
			return true;
		};

		o = s.taboption('advanced', form.Value, 'split_spread_ms', _('Largest delay difference to split (ms)'),
			_('Only the uplinks whose delay towards the VPS is at most this much behind the fastest one get a share of the split; the others carry a copy of every packet, as with weight 0, and get their share back once they stay within three quarters of it for the settle time. The VPS does the same with the download, with its own setting (30 ms unless changed there). Measured in the lab: two equal uplinks split give almost twice one uplink, but 50 ms apart most of that gain is lost, and 150 ms apart the speed collapses: the slowest uplink in the split sets the pace of all. 0: no limit.'));
		o.datatype = 'range(0,2000)';
		o.placeholder = '30';
		o.depends({ reorder: '1', split: '1' });

		o = s.taboption('advanced', form.Value, 'probe_interval_ms', _('Probe interval (ms)'),
			_('While there is traffic; each idle link is probed every second, or at this interval if it is longer. The next VPS address is tried after three idle probes at the least.'));
		o.datatype = 'range(100,60000)';
		o.placeholder = '100';

		o = s.taboption('advanced', form.Value, 'busy_poll_us', _('Busy polling (µs)'),
			_('Keeps polling this long after traffic instead of sleeping: lower latency, more CPU. 0: off.'));
		o.datatype = 'range(0,1000000)';
		o.placeholder = '0';

		o = s.taboption('advanced', form.ListValue, 'link_threads', _('Threads per uplink'),
			_('Off runs the code of every release so far. On gives each uplink a thread of its own, so that a stalled uplink or thread no longer holds back the others; it takes more CPU at low traffic, and a stall of the main thread, which checks every packet, still holds back every uplink at once. One thread runs the new structure without its threads, to tell a problem of the threads from one of the new code. A change restarts the engine.'));
		o.value('legacy', _('Off (the usual code)'));
		o.value('off', _('One thread (new structure)'));
		o.value('on', _('On (one thread per uplink)'));
		o.default = 'legacy';

		o = s.taboption('advanced', form.ListValue, 'log_level', _('Log level'));
		o.value('error', _('Errors'));
		o.value('warn', _('Warnings'));
		o.value('info', _('Information'));
		o.value('debug', _('Debug'));
		o.default = 'info';

		return m.render();
	}
});
