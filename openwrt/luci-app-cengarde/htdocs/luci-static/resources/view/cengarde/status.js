'use strict';
'require view';
'require dom';
'require fs';
'require poll';
'require ui';

// SPDX-License-Identifier: GPL-2.0-only

function problemText(p) {
	switch (p.code) {
	case 'no_secret': return _('No valid pairing secret: generate one in the VPS tab.');
	case 'no_server': return _('No VPS address yet.');
	case 'no_uplinks': return _('No uplinks selected.');
	case 'uplink_unknown': return _('Uplink %s does not exist in the network configuration.').format(p.iface);
	case 'uplink_noroute': return _('Uplink %s has "Use default gateway" off: cengarde needs its default route.').format(p.iface);
	case 'uplink_down': return _('Uplink %s is down.').format(p.iface);
	case 'no_upnp': return _('IP pass is on but miniupnpd is not installed.');
	case 'not_running': return _('The engine is not running: see System > System Log.');
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

function badge(text, color) {
	return E('span', {
		'style': 'display:inline-block;padding:0 .5em;border-radius:.5em;color:#fff;background:' + color
	}, [ text ]);
}

function linkState(l) {
	if (l.state == 'down')
		return badge(_('down'), '#888');
	if (l.state == 'stalled')
		return badge(_('no answer'), '#c33');
	if (l.upload == 'muted')
		return badge(_('muted'), '#d80');
	return badge(_('active'), '#393');
}

return view.extend({
	load: function() {
		return L.resolveDefault(fs.exec_direct('/usr/sbin/cengarde-setup', [ 'status' ], 'json'), null);
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

			tunnel = (ago > 180) ? badge(_('no handshake for %s').format(age(ago)), '#c33')
				: E('span', {}, [ badge(_('connected'), '#393'), ' ', _('handshake %s').format(age(ago)) ]);
		} else {
			tunnel = st.enabled ? badge(_('waiting for the VPS'), '#d80') : '-';
		}
		rows.push([ _('WireGuard tunnel'), tunnel ]);

		if (e) {
			rows.push([ _('Traffic'), _('%s up, %s down').format(
				'%1024.2mB'.format(e.upload.bytes), '%1024.2mB'.format(e.download.bytes)) ]);
			rows.push([ _('Duplicate copies dropped'), '%d'.format(e.download.duplicates) ]);
			rows.push([ _('Version'), e.version ]);
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
		const table = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, [ _('Uplink') ]),
				E('th', { 'class': 'th' }, [ _('State') ]),
				E('th', { 'class': 'th' }, [ _('RTT') ]),
				E('th', { 'class': 'th' }, [ _('Behind the fastest') ]),
				E('th', { 'class': 'th' }, [ _('From the VPS') ]),
				E('th', { 'class': 'th' }, [ _('Arrived first') ]),
				E('th', { 'class': 'th' }, [ _('Sent') ]),
				E('th', { 'class': 'th' }, [ _('Mutes') ])
			])
		]);

		links.forEach(function(l) { firsts += l.rx_first; });
		cbi_update_table(table, links.map(function(l) {
			return [
				E('span', { 'title': l.name }, [ l.label || l.name, l.label ? E('small', {}, [ ' (' + l.name + ')' ]) : '' ]),
				linkState(l),
				ms(l.rtt_ms),
				ms(l.upload_behind_ms),
				l.download_muted ? badge(_('muted by the VPS'), '#d80') : _('in use'),
				firsts ? '%.0f %%'.format(100 * l.rx_first / firsts) : '-',
				'%1024.2mB'.format(l.tx_bytes),
				'%d'.format(l.upload_mutes)
			];
		}), E('em', {}, [ st.running ? _('No uplink is up yet.') : _('The engine is not running.') ]));

		return table;
	},

	renderProblems: function(st) {
		return E('div', {}, st.problems.map(function(p) {
			return E('div', { 'class': 'alert-message warning cengarde-problem' }, [ problemText(p) ]);
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
				_('Behind the fastest: how much later this uplink delivers than the quickest one; past the limit it gets muted (no traffic, probes only) until it catches up. Arrived first: share of the download that came through this uplink before any other copy.')
			])
		]);
	},

	render: function(st) {
		const node = E('div', { 'class': 'cbi-section' });

		this.update(node, st);
		poll.add(L.bind(function() {
			return L.resolveDefault(fs.exec_direct('/usr/sbin/cengarde-setup', [ 'status' ], 'json'), null)
				.then(L.bind(this.update, this, node));
		}, this), 3);

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
