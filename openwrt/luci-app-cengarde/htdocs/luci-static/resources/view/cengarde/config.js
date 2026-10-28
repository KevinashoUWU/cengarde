'use strict';
'require view';
'require form';
'require fs';
'require ui';
'require uci';
'require tools.widgets as widgets';

// SPDX-License-Identifier: GPL-3.0-or-later

const TEMPLATE = '/usr/share/cengarde/cloud-config.yaml';
const MAX_SERVERS = 8; // the engine's CG_MAX_SERVERS
// Largest tunnel MTU in a 1500-byte path: 1500 - (IP, UDP and cengarde
// headers), 20 + 8 + 24 over IPv4, 40 + 8 + 24 over IPv6.
const MTU_IPV6 = 1424;

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

// The types an uplink can be (the engine's [link] type, story 015), as
// cengarde-setup takes them in "list uplink_type 'UPLINK=TYPE'".
const LINK_TYPES = [ 'auto', 'cellular', 'starlink', 'fiber', 'ethernet', 'radio' ];

// The type of uplink in a list of UPLINK=TYPE entries: the last valid entry
// for it (as cengarde-setup reads them), 'auto' without one.
function uplinkType(list, uplink) {
	let t = 'auto';

	L.toArray(list).forEach(function(e) {
		const i = String(e).indexOf('=');

		if (i > 0 && String(e).substring(0, i) == uplink && LINK_TYPES.indexOf(String(e).substring(i + 1)) >= 0)
			t = String(e).substring(i + 1);
	});
	return t;
}

// The list with uplink set to type: its entries out, then UPLINK=TYPE at the
// end unless the type is auto (the engine's default, nothing to write).
function setUplinkType(list, uplink, type) {
	const out = L.toArray(list).filter(function(e) { return String(e).indexOf(uplink + '=') != 0; });

	if (type && type != 'auto')
		out.push(uplink + '=' + type);
	return out;
}

// What bonding and redundancy bring as defaults (cengarde-setup's
// mode_presets): an option set apart overrides them. The core switches
// (split, protocontroller, subflow, link_cc) belong to the mode outright.
const PRESETS = {
	bonding: { tcp_aware: '1', rescue: '1', split_spread_copy: '0' },
	redundancy: { tcp_aware: '1', rescue: '1' }
};

function presetOf(mode, name, dflt) {
	return (PRESETS[mode] && PRESETS[mode][name] != null) ? PRESETS[mode][name] : dflt;
}

// Same substitutions as "cengarde-setup cloud-config", on the values in the
// form, so the text follows unsaved changes. Without an address (a new secret
// that is not saved yet) its placeholder stays, and the VPS derives it.
function cloudConfig(template, secret, port, ipPass, ipv6, name, addr, mode) {
	return template
		.replace(/^(\s+)REPLACE_SECRET$/m, '$1' + (secret || 'REPLACE_SECRET'))
		.replace(/^(\s*CENGARDE_PORT=).*$/m, '$1' + (port || '65500'))
		.replace(/^(\s*PASSTHROUGH=).*$/m, '$1' + (ipPass == '1' ? 'yes' : 'no'))
		.replace(/^(\s*TUNNEL_IPV6=).*$/m, '$1' + (ipv6 == '1' ? 'yes' : 'no'))
		.replace(/^(\s*ROUTER_NAME=).*$/m, '$1' + (isName(name) ? name : 'router'))
		.replace(/^(\s*ROUTER_TUNNEL_ADDR=).*$/m, '$1' + (addr || 'REPLACE_TUNNEL_ADDR'))
		.replace(/REPLACE_MODE$/m, mode || 'custom');
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

// One of cengarde-vps-admin's commands on the VPS (cengarde-setup vps):
// its output, or its error as a rejected promise.
function vpsRun(words) {
	return fs.exec('/usr/sbin/cengarde-setup', [ 'vps' ].concat(words)).then(function(r) {
		if (r.code != 0)
			throw new Error((r.stderr || r.stdout || '').trim() || _('The VPS did not answer (code %d).').format(r.code));
		return (r.stdout || '').trim();
	});
}

// KEY=VALUE lines into an object.
function vpsParse(out) {
	const o = {};

	String(out).split('\n').forEach(function(l) {
		const i = l.indexOf('=');

		if (i > 0)
			o[l.substring(0, i)] = l.substring(i + 1);
	});
	return o;
}

// The VPS tab's panel: the command that authorizes this router's key on the
// VPS, what the VPS has, and a line per setting to change it.
function vpsPanel() {
	const select = function(opts) {
		return E('select', { 'class': 'cbi-input-select' }, opts.map(function(v) {
			return E('option', { 'value': v[0] }, [ v[1] ]);
		}));
	};
	const cmd = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'readonly': 'readonly',
		'style': 'width:100%;font-family:monospace;font-size:12px', 'id': 'cengarde-vps-key' });
	const out = E('pre', { 'style': 'white-space:pre-wrap;margin:.5em 0;font-size:12px', 'id': 'cengarde-vps-out' });
	const mode = select([ [ 'bonding', _('Bonding') ], [ 'redundancy', _('Redundancy') ], [ 'custom', _('Custom') ] ]);
	const fec = select([ [ 'off', _('Off') ], [ 'on', _('Upload: the routers\' repairs rebuilt') ], [ 'rlc', _('Upload and download (RLC)') ] ]);
	const pctl = select([ [ 'on', _('On') ], [ 'off', _('Off') ] ]);
	const key = E('select', { 'class': 'cbi-input-select' });
	const val = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'style': 'width:8em', 'placeholder': _('value') });
	let busy = false;

	const show = function(text, err) {
		out.textContent = text;
		out.style.color = err ? 'var(--error-color, #c00)' : '';
	};
	const read = function() {
		show(_('Reading the VPS…'));
		return vpsRun([ 'get' ]).then(function(text) {
			const v = vpsParse(text);
			const was = key.value;

			mode.value = v.mode || 'custom';
			fec.value = v.fec || 'off';
			pctl.value = v.protocontroller || 'off';
			key.innerHTML = '';
			(v.keys || '').split(' ').filter(Boolean).forEach(function(k) {
				const set = v['local.' + k];

				key.appendChild(E('option', { 'value': k }, [ k + (set != null ? ' = ' + set : '') ]));
			});
			if (was)
				key.value = was;
			show(text.split('\n').filter(function(l) { return l.indexOf('keys=') != 0; }).join('\n'));
		}).catch(function(e) { show(e.message, true); });
	};
	const run = function(words) {
		if (busy)
			return;
		busy = true;
		show(_('On the VPS: %s…').format(words.join(' ')));
		return vpsRun(words).then(function(text) {
			return read().then(function() {
				if (text)
					show(text + '\n\n' + out.textContent);
			});
		}).catch(function(e) { show(e.message, true); }).finally(function() { busy = false; });
	};
	const button = function(label, cls, fn) {
		return E('button', { 'class': 'cbi-button ' + cls, 'click': function(ev) { ev.preventDefault(); fn(); } }, [ label ]);
	};
	const row = function(label, items) {
		return E('div', { 'style': 'display:flex;flex-wrap:wrap;gap:.5em;align-items:center;margin-top:.5em' },
			[ E('span', { 'style': 'min-width:9em' }, [ label ]) ].concat(items));
	};

	fs.exec('/usr/sbin/cengarde-setup', [ 'vps-key' ]).then(function(r) {
		const line = (r.stdout || '').trim();

		if (r.code == 0 && line)
			cmd.value = 'sudo cengarde-vps-setup admin-key add \'' + line + '\'';
		else
			show((r.stderr || '').trim() || _('No key: save and apply with this option on first.'), true);
	}).catch(function(e) { show(e.message, true); });

	return E('div', {}, [
		E('div', { 'style': 'display:flex;gap:.5em;align-items:center' }, [
			cmd,
			button(_('Copy'), 'cbi-button-action', function() { if (cmd.value) copyField(cmd); })
		]),
		row(_('Mode'), [ mode, button(_('Apply'), 'cbi-button-apply', function() { run([ 'mode', mode.value ]); }) ]),
		row(_('FEC'), [ fec, button(_('Apply'), 'cbi-button-apply', function() { run([ 'fec', fec.value ]); }) ]),
		row(_('Protocontroller'), [ pctl, button(_('Apply'), 'cbi-button-apply', function() { run([ 'protocontroller', pctl.value ]); }) ]),
		row(_('Engine setting'), [ key, val,
			button(_('Set'), 'cbi-button-apply', function() {
				if (key.value && /^[A-Za-z0-9._-]{1,16}$/.test(val.value))
					run([ 'set', key.value, val.value ]);
				else
					show(_('Pick a setting and give it a number or a word (up to 16 characters).'), true);
			}),
			button(_('Remove'), 'cbi-button-remove', function() { if (key.value) run([ 'unset', key.value ]); })
		]),
		E('div', { 'style': 'margin-top:.5em' }, [ button(_('Read'), 'cbi-button-neutral', read) ]),
		out
	]);
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
					on('tunnel_ipv6', '0') && on('tunnel', '1') && on('route_all', '1') ? '1' : '0', name, addr,
					value(m, 'mode') || 'custom');
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
			_('Carries the traffic of the router through a tunnel to a VPS, sending every packet over all the selected uplinks at once, and the first copy to arrive wins: one uplink failing or lagging no longer drops the connection. Setup: copy the cloud-config from the VPS tab when creating the VPS, then enter its IP here, pick the uplinks and enable.'));

		s = m.section(form.NamedSection, 'main', 'cengarde');
		s.addremove = false;
		s.tab('general', _('General'));
		s.tab('vps', _('VPS'));
		s.tab('tunnel', _('Tunnel'));
		s.tab('protection', _('Protection'), _('How each packet is protected while every uplink carries the upload (the split off): full copies, or partial copies and repair data in what each backup uplink has to spare, and asking the VPS for what is still missing. With the split on, or a lone uplink, the repair data below (FEC).'));
		s.tab('layer4', _('Layer 4 features'), _('What cengarde does with the TCP it carries, beyond moving packets.'));
		s.tab('advanced', _('Advanced'));

		o = s.taboption('general', form.Flag, 'enabled', _('Enabled'));
		o.rmempty = false;

		// The mode of the saved configuration (no mode: custom, as before).
		const savedMode = function() { return uci.get('cengarde', 'main', 'mode') || 'custom'; };

		o = s.taboption('general', form.ListValue, 'mode', _('Mode'),
			_('Bonding adds the uplinks up: each large packet goes over the uplink where it arrives first, each uplink with its own rate, and the TCP of the LAN travels as GRIT, cengarde\'s own stream (in the field: the download at 93 % of OpenMPTCProuter\'s and the upload above it, at half its ping). Redundancy copies every packet over every uplink and the first copy wins, and the TCP of the LAN travels as a plain TCP between the router and the VPS (protocontroller A): the safest in a cut and for live broadcasts, and the most data. Custom: the Protection, Layer 4 and Advanced tabs decide everything, as before. The VPS has to match: "sudo cengarde-vps-setup mode bonding" (or redundancy) there, or from the VPS tab when this router administers it. A change restarts the engine.'));
		o.value('bonding', _('Bonding (add the uplinks up)'));
		o.value('redundancy', _('Redundancy (every packet over every uplink)'));
		o.value('custom', _('Custom (the other tabs decide)'));
		o.cfgvalue = function() { return savedMode(); };
		o.rmempty = false;
		o.onchange = refresh;

		// A flag whose default is the mode's (PRESETS): it shows what applies,
		// and is stored only when it differs, so that it follows the mode.
		const modeFlag = function(o, name, dflt) {
			o.rmempty = false;
			o.cfgvalue = function(section_id) {
				const v = uci.get('cengarde', section_id, name);

				return v != null ? v : presetOf(savedMode(), name, dflt);
			};
			o.write = function(section_id, v) {
				if (v == presetOf(value(m, 'mode') || 'custom', name, dflt))
					uci.unset('cengarde', section_id, name);
				else
					uci.set('cengarde', section_id, name, v);
			};
		};

		o = s.taboption('general', form.DynamicList, 'server', _('VPS addresses'),
			_('Public IPs of the VPS as the provider shows them, IPv4 and IPv6, in order of preference: each uplink uses the first one of a family it has, and tries the next one when the VPS does not answer there. A name works too: the router looks it up through each uplink (the carrier\'s DNS first), never through the tunnel, and follows it when its IP changes (dynamic DNS). An MTU above 1424 does not fit if the name has an IPv6 address that the uplinks use.'));
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

			return value != 'lan' && value != 'loopback' && value != 'cg0' && (chosen || !companion);
		};
		const uplinkFilter = o.filter;

		// Each uplink's type, a dropdown under the list for each uplink in it
		// (one option per interface that could be an uplink, shown while it
		// is chosen), kept in "list uplink_type 'UPLINK=TYPE'". In bonding and
		// redundancy alike: the engine measures each uplink by its type, and
		// tells the VPS.
		uci.sections('network', 'interface').map(function(n) { return n['.name']; })
			.concat(L.toArray(uci.get('cengarde', 'main', 'uplink')))
			.filter(function(n, i, all) { return all.indexOf(n) == i && uplinkFilter('main', n); })
			.forEach(function(name) {
				o = s.taboption('general', form.ListValue, 'uplink_type_' + name, _('Type of %s').format(name),
					_('What kind of link it is. Today only Starlink changes anything, and only when large packets are split across the uplinks by expected arrival (split and split_eta): Starlink drops instead of queueing when it is full, so cengarde holds it to the queue it has shown and sends the rest over the other uplinks while they deliver it soon enough, and the VPS does the same with the download (in the lab, with Starlink and two cellular uplinks, 41 to 60 Mbit/s). The other types behave as Automatic for now.'));
				o.value('auto', _('Automatic'));
				o.value('cellular', _('Cellular (LTE/5G)'));
				o.value('starlink', _('Starlink'));
				o.value('fiber', _('Fiber'));
				o.value('ethernet', _('Cable, DSL or Ethernet'));
				o.value('radio', _('Radio link (point to point)'));
				o.default = 'auto';
				o.depends({ uplink: name, '!contains': true });
				o.cfgvalue = function(section_id) {
					return uplinkType(uci.get('cengarde', section_id, 'uplink_type'), name);
				};
				// The list with this uplink's entry changed; nothing touched
				// when it stays the same (a hidden uplink without an entry).
				const store = function(section_id, v) {
					const was = L.toArray(uci.get('cengarde', section_id, 'uplink_type'));
					const list = setUplinkType(was, name, v);

					if (JSON.stringify(list) == JSON.stringify(was))
						return;
					if (list.length)
						uci.set('cengarde', section_id, 'uplink_type', list);
					else
						uci.unset('cengarde', section_id, 'uplink_type');
				};
				o.write = store;
				o.remove = function(section_id) { store(section_id, 'auto'); };
			});

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
			_('For a new VPS. Paste it as "user data" (cloud-init) when creating an Ubuntu 22.04 or 24.04 VPS, e.g. on Vultr. It builds the same cengarde version as this router and sets up the tunnel, NAT and IP pass, with the name and address of this router. Setup log on the VPS: /var/log/cloud-init-output.log.'));
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

		o = s.taboption('vps', form.Flag, 'vps_admin', _('Administer the VPS from here'),
			_('The settings of the VPS (its mode, the FEC of the download, the protocontroller and some of the engine\'s) are the same for every router it serves: one router administers them, over SSH with a key of its own that can do that and nothing else. On: this router\'s mode goes to the VPS on each change, and the panel below reads and changes the rest. Save and apply first.'));
		o.default = '0';

		o = s.taboption('vps', form.Value, 'vps_ssh_host', _('VPS address for SSH'),
			_('Empty: the first VPS address (General tab).'));
		o.datatype = 'host';
		o.depends('vps_admin', '1');

		o = s.taboption('vps', form.Value, 'vps_ssh_port', _('VPS SSH port'));
		o.datatype = 'port';
		o.placeholder = '22';
		o.depends('vps_admin', '1');

		o = s.taboption('vps', form.DummyValue, '_vpsadmin', _('The VPS\'s settings'),
			_('First, once, run the command below on the VPS: it lets this router\'s key change these settings and nothing else (cengarde-vps-admin). Then "Read" shows what the VPS has, and each "Apply" changes it at once, for every router of the VPS; the engine there restarts or reloads.'));
		o.write = function() {};
		o.remove = function() {};
		o.depends('vps_admin', '1');
		o.renderWidget = function(section_id) {
			return vpsPanel();
		};

		o = s.taboption('tunnel', form.Flag, 'tunnel', _('Manage the tunnel'),
			_('cengarde carries the packets itself through its own interface cg0: authenticated (nobody can inject or replay packets) but NOT encrypted, so the carriers see the traffic. On: cg0 gets this router\'s address in the tunnel (VPS tab) and its routes, in the wan firewall zone. Off: cengarde still carries what reaches cg0, but its interface, routes and firewall zone are yours to set up.'));
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

		o = s.taboption('layer4', form.ListValue, 'protocontroller', _('Protocontroller'),
			_('With A or B the router answers the TCP connections of the LAN itself and the VPS opens a fresh one to each destination (cubic, BBR once that leg loses). Between them each connection travels as a plain TCP through the tunnel (A) or, experimental, as GRIT, a reliable stream of cengarde paced by what the uplinks carry instead of by loss (B). It helps TCP uploads such as RTMP over lossy uplinks and long paths beyond the VPS. UDP, QUIC and the rest go through the tunnel as always. The VPS needs "sudo cengarde-vps-setup protocontroller on". A change restarts the engine.'));
		o.value('no', _('No (the tunnel as always)'));
		o.value('a', _('A: TCP in the middle'));
		o.value('b', _('B (GRIT): cengarde\'s stream in the middle (experimental)'));
		o.default = 'no';
		o.depends({ tunnel: '1', mode: 'custom' });
		o.onchange = refresh;

		o = s.taboption('layer4', form.DummyValue, '_pctl_mode', _('Protocontroller'));
		o.rawhtml = true;
		o.cfgvalue = function() {
			return value(m, 'mode') == 'bonding'
				? _('B (GRIT), with each uplink\'s subflow and rate: set by the bonding mode (General tab).')
				: _('A, a plain TCP in the middle: set by the redundancy mode (General tab).');
		};
		o.depends('mode', 'bonding');
		o.depends('mode', 'redundancy');

		o = s.taboption('layer4', form.DynamicList, 'protocontroller_port', _('Protocontroller ports'),
			_('Only the TCP to these destination ports (or ranges such as 8000-8100). Empty: every port.'));
		o.datatype = 'or(port, portrange)';
		o.depends({ protocontroller: 'a', mode: 'custom' });
		o.depends({ protocontroller: 'b', mode: 'custom' });
		o.depends('mode', 'bonding');
		o.depends('mode', 'redundancy');

		o = s.taboption('layer4', form.DynamicList, 'protocontroller_dest', _('Protocontroller destinations'),
			_('Only the TCP to these addresses or prefixes. Empty: every address that is not private, local or link-local.'));
		o.datatype = 'or(ipaddr, cidr)';
		o.depends({ protocontroller: 'a', mode: 'custom' });
		o.depends({ protocontroller: 'b', mode: 'custom' });
		o.depends('mode', 'bonding');
		o.depends('mode', 'redundancy');

		o = s.taboption('layer4', form.Flag, 'tcp_aware', _('TCP-aware tunnel'),
			_('The router and the VPS look at the TCP inside the tunnel: its ACKs and handshakes are handed on at once instead of waiting behind a missing packet, and a TCP retransmission is copied over every uplink only when the tunnel lost the original on all of them (from the ACKs it has seen), so a spurious one does not load every uplink. Off: TCP is carried like any other packet.'));
		modeFlag(o, 'tcp_aware', '0');

		o = s.taboption('protection', form.ListValue, 'redund_spare', _('Protection mode'),
			_('Full copies: every packet over every uplink, as always; the safest in a cut, and the most data. Mixed: full copies while they fit, and XOR parity on an uplink that cannot carry them all. Parity: XOR only, the least data. RLC and Reed-Solomon: erasure codes that rebuild several lost packets at once from what the backup uplinks have to spare (in the lab, the most TCP over a choked backup uplink); RLC repairs sooner. Partial copies: copies only of what fits. Every mode but full copies needs "fec = yes" on the VPS to rebuild the upload.'));
		o.value('no', _('Full copies on every uplink'));
		o.value('mix', _('Mixed: copies, or parity when an uplink is choked'));
		o.value('parity', _('Parity (XOR)'));
		o.value('rlc', _('RLC erasure code'));
		o.value('rs', _('Reed-Solomon erasure code'));
		o.value('copies', _('Partial copies'));
		o.default = 'no';

		o = s.taboption('protection', form.ListValue, 'redund_spare_first', _('Main uplink'),
			_('The uplink that carries every packet while the others protect it. Automatic: the one with the least delay that is not choked. A main uplink that chokes or fails is replaced until it recovers.'));
		o.value('', _('Automatic'));
		L.toArray(uci.get('cengarde', 'main', 'uplink')).forEach(function(n) { o.value(n, n); });
		[ 'mix', 'parity', 'rlc', 'rs', 'copies' ].forEach(function(v) { o.depends('redund_spare', v); });

		o = s.taboption('protection', form.Value, 'pace_peers_ms', _('Copy window next to Starlink (ms)'),
			_('With a Starlink uplink, the others carry a copy of a packet only if it would arrive within this long of the first one. Empty: 60 ms, the Starlink preset. 250 is what worked in the field with cellular uplinks behind Starlink. 0: no limit.'));
		o.datatype = 'range(0,2000)';
		o.placeholder = '60';

		o = s.taboption('protection', form.Flag, 'fec', _('Repair data (FEC)'),
			_('With the split on (Advanced tab) or a lone uplink, repair packets go with the data, so the VPS rebuilds what an uplink loses without asking for it. The VPS needs "sudo cengarde-vps-setup fec on" to rebuild them. A change restarts the engine.'));
		o.default = '0';
		o.rmempty = false;

		o = s.taboption('protection', form.ListValue, 'fec_code', _('Repair code'),
			_('XOR parity: one parity per group of split packets, one per uplink; it rebuilds one lost packet per group. RLC sliding window (FEC v3): repairs over every packet of the last moments, a fixed share of the data spread over the uplinks, so an uplink that stalls for a moment is rebuilt from what the others carry, and scattered losses on a lone uplink too.'));
		o.value('xor', _('XOR parity'));
		o.value('rlc', _('RLC sliding window (FEC v3)'));
		o.default = 'xor';
		o.depends('fec', '1');

		o = s.taboption('protection', form.Value, 'fec_pct', _('Repair data (% of the upload)'),
			_('The most of the bytes that goes in repairs: all of it for 30 s after an uplink stalls or loses a burst, and with scattered losses 25 times the loss up to it. More rebuilds longer stalls (an uplink of four stalling 50 ms needs about 25 %) and costs as much of the capacity of the uplinks.'));
		o.datatype = 'range(1,50)';
		o.placeholder = '20';
		o.depends('fec_code', 'rlc');

		o = s.taboption('protection', form.Value, 'fec_pct_min', _('Repair data when nothing is lost (%)'),
			_('The share when no uplink lost at random or in a burst lately (losses with a full queue do not count: they are TCP slowing down). 0 sends no repairs on clean uplinks; the same as the repair data above keeps a fixed share.'));
		o.datatype = 'range(0,50)';
		o.placeholder = '0';
		o.depends('fec_code', 'rlc');

		o = s.taboption('protection', form.Value, 'fec_window_ms', _('Repair window (ms)'),
			_('How far back each repair reaches: it has to cover a stall and the time to rebuild it, within the wait of the VPS for a missing packet.'));
		o.datatype = 'range(10,1000)';
		o.placeholder = '300';
		o.depends('fec_code', 'rlc');

		o = s.taboption('protection', form.ListValue, 'fec_single', _('Repairs on a lone uplink'),
			_('With one uplink, repairs cannot save a cut (they are lost with it); they rebuild scattered losses. Automatic: only while the uplink loses at random (as Starlink does), not when its queue overflows, and never in a cut.'));
		o.value('auto', _('Automatic'));
		o.value('yes', _('Always'));
		o.value('no', _('Never'));
		o.default = 'auto';
		o.depends('fec_code', 'rlc');

		o = s.taboption('protection', form.Flag, 'rescue', _('Rescue missing packets'),
			_('A packet that no uplink delivered is asked for again (as parity when an erasure code is on) before the receiver gives up on it, within 60 ms. The router keeps what it sent for the VPS to ask, and asks the VPS for the download. The VPS needs rescue_kb and rescue_wait_ms in its cengarde.conf for its side. In the lab it doubled TCP with full copies over a lossy main uplink. On in bonding and redundancy unless set apart.'));
		modeFlag(o, 'rescue', '0');

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
			_('In a 1500-byte path, packets of up to 1444 bytes fit over IPv4 and of up to 1424 over IPv6. Larger ones get fragmented, and mobile networks often drop the fragments.'));
		o.datatype = 'range(1280,1444)';
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
			_('The end that receives holds a packet that arrives ahead of a missing one until the missing one comes or its time runs out, and hands them on in order. The VPS does the same with what it receives. While every packet is copied over all the uplinks nothing is held, so it changes nothing and costs almost nothing; it is what makes the split below usable.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('advanced', form.Value, 'reorder_max_wait_ms', _('Longest wait for a missing packet (ms)'),
			_('How long a packet that arrived early waits for the missing one before it is handed on anyway. It waits less when the slowest uplink would have delivered the missing one by then. A longer wait copes with slower uplinks, and adds delay when a packet is really lost.'));
		o.datatype = 'range(0,2000)';
		o.placeholder = '200';
		o.depends('reorder', '1');

		o = s.taboption('advanced', form.Value, 'reorder_bypass', _('Packets never held, up to (bytes)'),
			_('Packets of up to this size (empty TCP ACKs, small control packets) are handed on at once. 0: none. A larger value lets small data packets overtake the big ones that are held, which TCP and QUIC can take for losses.'));
		o.datatype = 'range(0,2048)';
		o.placeholder = '128';
		o.depends('reorder', '1');

		o = s.taboption('advanced', form.Flag, 'split', _('Split large packets across the uplinks (experimental)'),
			_('Instead of a copy over every uplink, each large packet (over 256 bytes, by default) goes over one uplink, chosen by weight; the small ones still go over all of them. This adds up the speed of the uplinks (in the lab, two equal uplinks carried 1.98 times what one does), but it protects less than copying: an uplink that dies keeps its share of the packets until the engine takes it out (about 300 ms by estimate, 1.5 s before that), and the packets sent to it in the meantime are lost; the slowest uplink in the split sets the latency of all (see the largest delay difference below); and a weight above what an uplink can carry fills the queue of its modem and makes the uplink unstable. The VPS follows this router: it splits the download the same way, with the same weights, when the router asks for it, and needs no setting. It needs "Reorder at the receiver".'));
		o.rmempty = false;
		o.depends({ reorder: '1', mode: 'custom' });

		o = s.taboption('advanced', form.Flag, 'subflow', _('Subflow per uplink'),
			_('Each uplink numbers what it carries and says back what it lost within itself (protocol 12), so a late packet on another uplink is never taken for a loss, and GRIT resends only what was really lost. Both ends need it. On in bonding.'));
		o.rmempty = false;
		o.depends('mode', 'custom');

		o = s.taboption('advanced', form.Flag, 'link_cc', _('Each uplink its own rate'),
			_('With the subflow on: each uplink runs its own rate controller (a BBR per uplink) fed by what the other end says it received, which decides the split and how fast GRIT sends. Both ends need it. On in bonding.'));
		o.rmempty = false;
		o.depends({ mode: 'custom', subflow: '1' });

		o = s.taboption('advanced', form.DynamicList, 'split_weight', _('Weight of each uplink in the split'),
			_('One entry per uplink, as NAME=WEIGHT (for example wan=3), with NAME as in the list of uplinks of the General tab and a weight from 0 to 1000. An uplink gets a share of the split packets in proportion to its weight; with no entry its weight is 1. 0: the uplink carries a copy of every packet, split or not. The weights do not follow the speed of the uplinks: choose them in proportion to what each one carries.'));
		o.placeholder = 'wan=2';
		o.depends({ reorder: '1', split: '1', mode: 'custom' });
		o.depends({ reorder: '1', mode: 'bonding' });
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
			_('Only the uplinks whose delay towards the VPS is at most this much behind the fastest one get a share of the split; the others leave it (the next option says what they carry), and get their share back once they stay within three quarters of it for the settle time. The VPS does the same with the download, with its own setting (30 ms unless changed there). Measured in the lab: two equal uplinks split give almost twice one uplink, but 50 ms apart most of that gain is lost, and 150 ms apart the speed collapses: the slowest uplink in the split sets the pace of all. 0: no limit. Bonding: 250 unless set.'));
		o.datatype = 'range(0,2000)';
		o.placeholder = '30';
		o.depends({ reorder: '1', split: '1', mode: 'custom' });
		o.depends({ reorder: '1', mode: 'bonding' });

		o = s.taboption('advanced', form.Flag, 'split_spread_copy', _('Copies to the uplinks out of the split'),
			_('On (as up to version 0.6.2): an uplink that leaves the split for its delay carries a copy of every packet, to fill the gaps of the others, and with fewer than two uplinks left in the split everything goes on every uplink. Off: it carries only the small packets, which go on every uplink, and one uplink left in the split carries everything that is split. An uplink falls behind most often because its modem\'s queue is full: in the first field test the copies loaded the late uplinks even more, and the router dropped thousands of packets; in the lab they helped QUIC. Only for the upload: the VPS has its own setting (on unless changed there).'));
		modeFlag(o, 'split_spread_copy', '1');
		o.depends({ reorder: '1', split: '1', mode: 'custom' });
		o.depends({ reorder: '1', mode: 'bonding' });

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
