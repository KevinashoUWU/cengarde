'use strict';
'require view';
'require dom';
'require fs';
'require poll';
'require ui';

// SPDX-License-Identifier: GPL-2.0-only

const MTU_MIN = 1280; // the smallest tunnel MTU config.js takes

function problemText(p) {
	switch (p.code) {
	case 'no_secret': return _('No valid pairing secret: generate one in the VPS tab.');
	case 'no_server': return _('No VPS address yet.');
	case 'no_uplinks': return _('No uplinks selected.');
	case 'uplink_unknown': return _('Uplink %s does not exist in the network configuration.').format(p.iface);
	case 'uplink_noroute': return _('Uplink %s has "Use default gateway" off: cengarde needs its default route.').format(p.iface);
	case 'uplink_down': return _('Uplink %s is down.').format(p.iface);
	case 'uplink_ipv6_only': return _('Uplink %s only has IPv6 (its IPv4 is down) and no VPS address is IPv6: add the IPv6 address of the VPS in Settings.').format(p.iface);
	case 'ipv6_companion_conflict': return _('Uplink %s gets no IPv6 interface: the name %s is taken or longer than 15 characters, so cengarde does not see the IPv6 of that uplink.').format(p.iface, p.name);
	case 'ipv6_leak': return _('The LAN still holds the public IPv6 prefix %s: its IPv6 goes around the tunnel, through the uplinks. Turn off "Delegate IPv6 prefixes" on the interface it comes from.').format(p.prefix);
	case 'no_upnp': return _('IP pass is on but miniupnpd is not installed.');
	case 'upnp_no_ip': return _('IP pass is on, but UPnP has no public IP to announce: no VPS address is IPv4. Set a STUN server in Settings > Tunnel.');
	case 'not_running': return _('The engine is not running: see System > System Log.');
	case 'vps_silent': return _('The VPS does not answer on any uplink: check that it runs cengarde with the secret of this router, and that its firewall lets in UDP to the VPS port.');
	case 'path_mtu':
		if (p.fit >= MTU_MIN)
			return _('Uplink %s takes packets of at most %d bytes to the VPS, fewer than the tunnel makes: lower the tunnel MTU to %d.').format(p.iface, p.mtu, p.fit);
		// Lowering the MTU is no way out there.
		if (p.family == 'ipv6')
			return _('Uplink %s takes packets of at most %d bytes to the VPS over IPv6, too few for the tunnel even at its smallest MTU (%d): if the uplink has IPv4 too, put an IPv4 VPS address first in the list.').format(p.iface, p.mtu, MTU_MIN);
		return _('Uplink %s takes packets of at most %d bytes to the VPS, too few for the tunnel even at its smallest MTU (%d): its packets get fragmented, and mobile networks often drop the fragments.').format(p.iface, p.mtu, MTU_MIN);
	default: return p.code;
	}
}

function age(seconds) {
	if (seconds < 120)
		return _('%d s ago').format(seconds);
	if (seconds < 7200)
		return _('%d min ago').format(Math.floor(seconds / 60));
	return _('%d h ago').format(Math.floor(seconds / 3600));
}

function ms(v) {
	return (v == null) ? '-' : '%.1f ms'.format(v);
}

function badge(text, color, title) {
	return E('span', {
		'style': 'display:inline-block;padding:0 .5em;border-radius:.5em;color:#fff;background:' + color,
		'title': title || null
	}, [ text ]);
}

function linkState(l) {
	let b;

	if (l.state == 'paused')
		return badge(_('paused'), '#888');
	if (l.state == 'down')
		b = badge(_('down'), '#888');
	else if (l.state == 'waiting')
		b = badge(_('waiting for the VPS'), '#d80');
	else if (l.state == 'stalled')
		b = badge(_('no answer'), '#c33');
	else if (l.upload == 'muted')
		b = badge(_('muted'), '#d80');
	else
		b = badge(_('active'), '#393');
	return (l.override == 'on') ? E('span', {}, [ b, ' ', E('small', {}, [ _('forced on') ]) ]) : b;
}

// The VPS address a link sends to, without the port, its family, and
// whether the link moved past the first VPS address it can use (on an
// uplink with IPv4 and IPv6, that one may be of the other family).
function vpsAddress(l) {
	const addr = (l.remote || '').replace(/:\d+$/, '').replace(/^\[(.*)\]$/, '$1');
	let parts;

	if (!addr)
		return '-';
	parts = [ addr, ' ', E('small', {}, [ l.family == 'ipv6' ? 'IPv6' : 'IPv4' ]) ];
	if (l.candidate > 0)
		parts.push(' ', badge(_('failover'), '#d80',
			_('The first VPS address it can use did not answer. Failovers so far: %d').format(l.failovers)));
	return E('span', {}, parts);
}

// IP pass as asked of the VPS, and as the VPS reports it.
function ipPass(p) {
	if (p.requested == null)
		return null;
	if (p.server == 'none')
		return badge(_('the VPS does not apply it (passthrough_file missing in its cengarde.conf)'), '#c33');
	if (p.server != p.requested)
		return badge(_('waiting for the VPS to confirm'), '#d80');
	return (p.server == 'on') ? badge(_('on at the VPS'), '#393') : _('off');
}

function sleep(ms) {
	return new Promise(function(resolve) { window.setTimeout(resolve, ms); });
}

return view.extend({
	load: function() {
		return L.resolveDefault(fs.exec_direct('/usr/sbin/cengarde-setup', [ 'status' ], 'json'), null);
	},

	refresh: function() {
		return this.load().then(L.bind(this.update, this, this.node));
	},

	// Pause an uplink (off), or hand it back to the configuration (auto).
	handleLink: function(name, what) {
		return fs.exec('/usr/sbin/cengarde', [ 'ctl', 'link', name, what ]).then(L.bind(function(res) {
			if (res.code != 0)
				ui.addNotification(null, E('p', {}, [ (res.stderr || res.stdout || '').trim() ]), 'error');
			return sleep(1200).then(L.bind(this.refresh, this));
		}, this));
	},

	renderSummary: function(st) {
		const rows = [];
		const e = st.engine;
		let service, tunnel;

		if (!st.enabled)
			service = badge(_('disabled'), '#888');
		else if (st.running)
			service = badge(_('running'), '#393');
		else
			service = badge(_('stopped'), '#c33');
		rows.push([ _('Service'), service ]);

		if (st.tunnel.handshake > 0) {
			const ago = st.tunnel.now - st.tunnel.handshake;

			/* Older than the boot, or in the future: the clock was set after
			 * the handshake (no battery-backed clock), and its age is unknown. */
			if (ago < 0 || (st.tunnel.uptime > 0 && ago > st.tunnel.uptime))
				tunnel = badge(_('handshake before the clock was set'), '#d80',
					_('The router set its clock after this handshake: the next one, within about two minutes of traffic, shows its age.'));
			else
				tunnel = (ago > 180) ? badge(_('no handshake for %s').format(age(ago)), '#c33')
					: E('span', {}, [ badge(_('connected'), '#393'), ' ', _('handshake %s').format(age(ago)) ]);
		} else {
			tunnel = st.enabled ? badge(_('waiting for the VPS'), '#d80') : '-';
		}
		rows.push([ _('WireGuard tunnel'), tunnel ]);

		if (e && e.passthrough && ipPass(e.passthrough))
			rows.push([ _('IP pass'), ipPass(e.passthrough) ]);

		if (e) {
			rows.push([ _('Traffic'), _('%s up, %s down').format(
				'%1024.2mB'.format(e.upload.bytes), '%1024.2mB'.format(e.download.bytes)) ]);
			rows.push([ _('Duplicate copies dropped'), '%d'.format(e.download.duplicates) ]);
			rows.push([ _('Version'), st.protocol ? _('%s, protocol %d').format(e.version, st.protocol) : e.version ]);
		}

		return E('table', { 'class': 'table' }, rows.map(function(r) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '33%' }, [ r[0] ]),
				E('td', { 'class': 'td left' }, [ r[1] ])
			]);
		}));
	},

	renderLinks: function(st) {
		const links = (st.engine && st.engine.links) || [];
		let firsts = 0;
		const table = E('table', { 'class': 'table cengarde-links' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, [ _('Uplink') ]),
				E('th', { 'class': 'th' }, [ _('State') ]),
				E('th', { 'class': 'th' }, [ _('RTT') ]),
				E('th', { 'class': 'th' }, [ _('Behind the fastest') ]),
				E('th', { 'class': 'th' }, [ _('VPS address') ]),
				E('th', { 'class': 'th' }, [ _('Path MTU') ]),
				E('th', { 'class': 'th' }, [ _('From the VPS') ]),
				E('th', { 'class': 'th' }, [ _('Arrived first') ]),
				E('th', { 'class': 'th' }, [ _('Sent') ]),
				E('th', { 'class': 'th' }, [ _('Mutes') ]),
				E('th', { 'class': 'th' }, [ ' ' ])
			])
		]);

		links.forEach(function(l) { firsts += l.rx_first; });
		cbi_update_table(table, links.map(L.bind(function(l) {
			const manual = l.override && l.override != 'auto';
			const out = (l.state == 'paused' || l.state == 'down'); // its figures are old

			return [
				E('span', { 'title': l.name }, [ l.label || l.name, l.label ? E('small', {}, [ ' (' + l.name + ')' ]) : '' ]),
				linkState(l),
				ms(out ? null : l.rtt_ms),
				ms(out ? null : l.upload_behind_ms),
				out ? '-' : vpsAddress(l),
				(out || !l.path_mtu) ? '-' : '%d'.format(l.path_mtu),
				out ? '-' : l.download_muted ? badge(_('muted by the VPS'), '#d80') : _('in use'),
				firsts ? '%.0f %%'.format(100 * l.rx_first / firsts) : '-',
				'%1024.2mB'.format(l.tx_bytes),
				'%d'.format(l.upload_mutes),
				E('button', {
					'class': 'cbi-button ' + (manual ? 'cbi-button-apply' : 'cbi-button-neutral'),
					'data-link': l.name,
					'title': manual ? _('Back to what the configuration says') : _('Takes the uplink out until you resume it or cengarde restarts'),
					'click': ui.createHandlerFn(this, 'handleLink', l.name, manual ? 'auto' : 'off')
				}, [ l.override == 'off' ? _('Resume') : manual ? _('Automatic') : _('Pause') ])
			];
		}, this)), E('em', {}, [ st.running ? _('No uplink is up yet.') : _('The engine is not running.') ]));

		return table;
	},

	renderProblems: function(st) {
		const problems = st.problems.map(problemText);

		if (st.engine && st.engine.config_error)
			problems.push(_('The last change was not applied, cengarde goes on with the previous settings: %s').format(st.engine.config_error));
		return E('div', {}, problems.map(function(text) {
			return E('div', { 'class': 'alert-message warning cengarde-problem' }, [ text ]);
		}));
	},

	update: function(node, st) {
		if (!st) {
			dom.content(node, E('div', { 'class': 'alert-message error' },
				[ _('Could not read the status (is the cengarde package installed?).') ]));
			return;
		}
		dom.content(node, [
			this.renderProblems(st),
			E('h3', {}, [ _('Status') ]),
			this.renderSummary(st),
			E('h3', {}, [ _('Uplinks') ]),
			this.renderLinks(st),
			E('p', { 'class': 'cbi-section-descr' }, [
				_('Behind the fastest: how much later this uplink delivers than the quickest one; past the limit it gets muted (no traffic, probes only) until it catches up. Arrived first: share of the download that came through this uplink before any other copy. Pause: takes an uplink out without touching the configuration, until you resume it or cengarde restarts.'),
				' ',
				_('VPS address: where the uplink sends to; "failover" when the first VPS address it can use did not answer. Path MTU: the largest packet the path to the VPS takes.')
			])
		]);
	},

	render: function(st) {
		const node = E('div', { 'class': 'cbi-section' });

		this.node = node;
		this.update(node, st);
		poll.add(L.bind(this.refresh, this), 3);

		return E([], [
			E('h2', {}, [ _('cengarde') ]),
			E('div', { 'class': 'cbi-map-descr' }, [
				_('Every packet goes through all active uplinks; the VPS keeps the first copy.')
			]),
			node
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
