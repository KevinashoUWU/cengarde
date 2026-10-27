'use strict';
'require view';
'require dom';
'require fs';
'require poll';
'require ui';

// SPDX-License-Identifier: GPL-3.0-or-later

const MTU_MIN = 1280; // the smallest tunnel MTU config.js takes

// What the engine says went wrong looking up a name of the VPS: its error, in words.
function resolveError(e) {
	switch (e) {
	case '':
	case 'timeout': return _('no answer');
	case 'servfail':
	case 'refused': return _('the DNS server failed');
	case 'nxdomain': return _('the name does not exist');
	case 'nodata': return _('no addresses');
	case 'no link': return _('no uplink to ask through');
	default: return e;
	}
}

function problemText(p) {
	switch (p.code) {
	case 'no_secret': return _('No valid pairing secret: generate one in the VPS tab.');
	case 'tunnel_addr_invalid': return _('The address in the tunnel is not valid (10.79.X.Y, from 10.79.0.2 to 10.79.255.254): the tunnel is left as it was. Fix it in the VPS tab, or empty it to use the one derived from the secret.');
	case 'no_server': return _('No VPS address yet.');
	case 'no_uplinks': return _('No uplinks selected.');
	case 'uplink_unknown': return _('Uplink %s does not exist in the network configuration.').format(p.iface);
	case 'uplink_noroute': return _('Uplink %s has "Use default gateway" off: cengarde needs its default route.').format(p.iface);
	case 'uplink_down': return _('Uplink %s is down.').format(p.iface);
	case 'uplink_ipv6_only': return _('Uplink %s only has IPv6 (its IPv4 is down) and no VPS address is IPv6: add the IPv6 address of the VPS in Settings.').format(p.iface);
	case 'ipv6_companion_conflict': return _('Uplink %s gets no IPv6 interface: the name %s is taken or longer than 15 characters, so cengarde does not see the IPv6 of that uplink.').format(p.iface, p.name);
	case 'no_ula_prefix': return _('IPv6 through the tunnel is on, but the router has no ULA prefix, so the LAN has no IPv6 address to send through it: set one in Network > Interfaces > Global network options.');
	case 'ipv6_leak': return _('The LAN still holds the public IPv6 prefix %s: its IPv6 goes around the tunnel, through the uplinks. Turn off "Delegate IPv6 prefixes" on the interface it comes from.').format(p.prefix);
	case 'no_upnp': return _('IP pass is on but miniupnpd is not installed.');
	case 'upnp_no_ip': return _('IP pass is on, but UPnP has no public IP to announce: no VPS address is an IPv4 address (a name does not count, its IP may change). Set a STUN server in Settings > Tunnel.');
	case 'server_unresolved': return _('The VPS name %s could not be resolved (%s).').format(p.name, resolveError(p.error));
	case 'not_running': return _('The engine is not running: see System > System Log.');
	case 'vps_silent': return _('The VPS does not answer on any uplink: check that it runs cengarde with the secret of this router, and that its firewall lets in UDP to the VPS port.');
	case 'vps_refusing': return _('The VPS answers but refuses this router: it is turned off there, or the VPS has no room for another session. Run "cengarde-vps-setup list" on the VPS.');
	case 'link_thread_stalled': return _('The thread of uplink %s has not run for %d s with packets waiting; the other uplinks go on. If it happens again, set "Threads per uplink" to Off in Settings > Advanced.').format(p.iface, p.seconds);
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

// A two-column table of [ name, value ] rows.
function rowsTable(rows) {
	return E('table', { 'class': 'table' }, rows.map(function(r) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, [ r[0] ]),
			E('td', { 'class': 'td left' }, [ r[1] ])
		]);
	}));
}

function badge(text, color, title) {
	return E('span', {
		'style': 'display:inline-block;padding:0 .5em;border-radius:.5em;color:#fff;background:' + color,
		'title': title || null
	}, [ text ]);
}

// An uplink on standby for one direction: slower than the best one there, it
// carries only probes until it catches up. Not a fault, so not in a warning
// colour: each direction is decided apart (upload here, download at the VPS),
// and carriers are often faster one way than the other.
function direction(standby, title) {
	return standby ? badge(_('standby'), '#68a', title) : _('in use');
}

// An uplink the split leaves out for its delay (the engine's split_share):
// in use, but without a share. It carries a copy of every packet with
// split_spread_copy (the engine's default, and what an engine up to 0.6.2
// did, which does not say), or only what goes on every uplink (the small
// packets) without it.
function splitNote(l, copy) {
	if (l.split_share != 'spread')
		return [];
	return [ ' ', copy ? badge(_('copies only'), '#68a',
		_('More than the largest delay difference (Advanced tab) behind the fastest uplink: it carries a copy of every packet instead of a share of the split, until it catches up.'))
		: badge(_('out of the split'), '#68a',
		_('More than the largest delay difference (Advanced tab) behind the fastest uplink: it carries only the small packets, which go on every uplink, instead of a share of the split, until it catches up.')) ];
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

// A name of the VPS: where it points now, and how long ago the DNS answered
// (the engine looks it up itself, through the uplinks) or "from /etc/hosts".
// A name with no address shows a dash: its problem is in the list above.
function vpsName(n) {
	const addrs = n.addresses || [];
	const parts = [ addrs.length ? addrs.join(', ') : '-' ];

	if (addrs.length && n.state == 'hosts')
		parts.push(' ', E('small', {}, [ _('from /etc/hosts') ]));
	else if (addrs.length && n.answered_ms_ago != null)
		parts.push(' ', E('small', {}, [ _('answered %s').format(age(Math.floor(n.answered_ms_ago / 1000))) ]));
	return E('span', {}, parts);
}

// IP pass as asked of the VPS, and as the VPS reports it.
function ipPass(p) {
	if (p.requested == null)
		return null;
	if (p.server == 'none')
		return badge(_('the VPS does not apply it (no passthrough_file or forward_file in its cengarde.conf)'), '#c33');
	// A VPS with several routers gives the whole range to one of them.
	if (p.requested == 'on' && p.server == 'off')
		return badge(_('not granted by the VPS: another of its routers holds it, or this one may not have it (run "cengarde-vps-setup forward" on the VPS)'), '#d80');
	if (p.server != p.requested)
		return badge(_('waiting for the VPS to confirm'), '#d80');
	return (p.server == 'on') ? badge(_('on at the VPS'), '#393') : _('off');
}

function sleep(ms) {
	return new Promise(function(resolve) { window.setTimeout(resolve, ms); });
}

// How many queue samples per uplink the page keeps: one per refresh (every
// 3 s), five minutes.
const QUEUE_KEEP = 100;

// The p-th percentile (0-100) of a list of numbers, or null for none.
function percentile(list, p) {
	const v = list.filter(function(x) { return typeof x == 'number'; }).sort(function(a, b) { return a - b; });

	if (!v.length)
		return null;
	return v[Math.min(v.length - 1, Math.floor(p / 100 * v.length))];
}

function mbit(v) {
	return (typeof v == 'number') ? '%.1f Mbit/s'.format(v) : '-';
}

// The link types as config.js names them.
function linkType(t) {
	switch (t) {
	case 'cellular': return _('Cellular (LTE/5G)');
	case 'starlink': return _('Starlink');
	case 'fiber': return _('Fiber');
	case 'ethernet': return _('Cable, DSL or Ethernet');
	case 'radio': return _('Radio link (point to point)');
	default: return _('Automatic');
	}
}

// Whose queue it is (the engine's sig.queue_owner, story 018): this
// router's own traffic, or other users of the cell.
function queueOwner(sig) {
	switch (sig && sig.queue_owner) {
	case 'mine': return _('own traffic');
	case 'others': return badge(_('other users'), '#d80',
		_('The uplink delivered more than 15 % less when its queue grew: other users of the cell are taking its capacity.'));
	default: return '-';
	}
}

// How it loses (the engine's sig.loss_kind): at random without a queue, as
// Starlink does, or when its queue overflows, as a cellular modem does.
function lossKind(sig) {
	switch (sig && sig.loss_kind) {
	case 'random': return badge(_('random'), '#68a',
		_('It loses packets with no queue, as Starlink does: repair data (parity or erasure codes) fixes this best.'));
	case 'congestive': return badge(_('queue overflow'), '#d80',
		_('It loses when its queue overflows: sending it less helps more than sending it repairs.'));
	default: return _('none');
	}
}

// What redund_spare does, in words (config.js's names).
function spareMode(m) {
	switch (m) {
	case 'mix': return _('Mixed: copies, or parity when an uplink is choked');
	case 'parity': return _('Parity (XOR)');
	case 'rlc': return _('RLC erasure code');
	case 'rs': return _('Reed-Solomon erasure code');
	case 'copies': return _('Partial copies');
	default: return _('Full copies on every uplink');
	}
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

		/* Connected while some uplink is live. */
		const live = e && Array.isArray(e.links) && e.links.some(function(l) { return l.state == 'live'; });

		tunnel = live ? badge(_('connected'), '#393') : (st.enabled ? badge(_('waiting for the VPS'), '#d80') : '-');
		rows.push([ _('Tunnel (not encrypted)'), tunnel ]);

		if (st.tunnel_addr)
			rows.push([ _('Address in the tunnel'), st.tunnel_addr ]);

		if (st.tunnel_ipv6 && st.tunnel_ula)
			rows.push([ _('IPv6 address in the tunnel'), st.tunnel_ula ]);

		if (e && Array.isArray(e.names))
			e.names.forEach(function(n) { rows.push([ _('VPS name %s').format(n.name), vpsName(n) ]); });

		if (e && e.passthrough && ipPass(e.passthrough))
			rows.push([ _('IP pass'), ipPass(e.passthrough) ]);

		if (e) {
			rows.push([ _('Traffic'), _('%s up, %s down').format(
				'%1024.2mB'.format(e.upload.bytes), '%1024.2mB'.format(e.download.bytes)) ]);
			rows.push([ _('Duplicate copies dropped'), '%d'.format(e.download.duplicates) ]);
			rows.push([ _('Version'), st.protocol ? _('%s, protocol %d').format(e.version, st.protocol) : e.version ]);
		}

		return rowsTable(rows);
	},

	// Reordering at the receiver, from the engine's "reorder" object (counters
	// since the engine started). An engine without the object has no box.
	renderReorder: function(st) {
		const r = st.engine && st.engine.reorder;
		const n = function(v) { return (typeof v == 'number') ? v : 0; };
		let lost, late, rows;

		if (!r || typeof r != 'object')
			return null;
		if (r.enabled === false)
			return E('p', { 'class': 'cbi-section-descr' }, [ _('Reordering is off (Advanced tab).') ]);

		lost = r.lost || {};
		late = r.late || {};
		rows = [
			[ _('Held now'), (typeof r.pool_buffers == 'number' && r.pool_buffers > 0)
				? _('%d of %d').format(n(r.held), r.pool_buffers) : '%d'.format(n(r.held)) ],
			[ _('Most held at once'), '%d'.format(n(r.held_max)) ],
			[ _('Held in total'), '%d'.format(n(r.held_total)) ],
			[ _('Missing packets given up on'), _('%d at the deadline, %d for lack of room').format(n(lost.cap), n(lost.overflow)) ],
			[ _('Late after release'), E('span', {
				'title': _('Arrived after the engine gave up on them: after the deadline %d, after running out of room %d, after the other uplinks had gone past %d, too old to tell %d.').format(
					n(late.after_cap), n(late.after_overflow), n(late.after_rack), n(late.old))
			}, [ '%d'.format(n(late.after_cap) + n(late.after_overflow) + n(late.after_rack) + n(late.old)) ]) ]
		];
		return E('div', {}, [
			rowsTable(rows),
			E('p', { 'class': 'cbi-section-descr' }, [
				_('Held now: packets that arrived ahead of one that has not, out of how many there is room for. Most held at once and Held in total count since the engine started.'),
				' ',
				_('Missing packets given up on: the wait for the packet ended at its deadline, or the room ran out first. If it arrives afterwards it counts as late after release; many of those mean that the longest wait (Advanced tab) is too short for the slowest uplink.')
			])
		]);
	},

	renderLinks: function(st) {
		const links = (st.engine && st.engine.links) || [];
		const up = (st.engine && st.engine.upload) || {};
		const copy = up.split_spread_copy !== false;
		let firsts = 0;
		const table = E('table', { 'class': 'table cengarde-links' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, [ _('Uplink') ]),
				E('th', { 'class': 'th' }, [ _('State') ]),
				E('th', { 'class': 'th' }, [ _('RTT') ]),
				E('th', { 'class': 'th' }, [ _('Behind the fastest') ]),
				E('th', { 'class': 'th' }, [ _('VPS address') ]),
				E('th', { 'class': 'th' }, [ _('Path MTU') ]),
				E('th', { 'class': 'th' }, [ _('Upload (to the VPS)') ]),
				E('th', { 'class': 'th' }, [ _('Download (from the VPS)') ]),
				E('th', { 'class': 'th' }, [ _('Arrived first') ]),
				E('th', { 'class': 'th' }, [ _('Sent') ]),
				E('th', { 'class': 'th' }, [ _('Upload to standby') ]),
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
				out ? '-' : E('span', {}, [ direction(l.upload == 'muted',
					_('Behind the fastest uplink by more than the limit: the router sends through the others, and only probes through this one, until it catches up.')) ].concat(splitNote(l, copy))),
				out ? '-' : direction(l.download_muted,
					_('Slower than the best uplink from the VPS: the VPS sends through the others, and only probes through this one, until it catches up.')),
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

	// Each uplink's health: its type, the capacity the engine measures, its
	// queue now and over the last minutes (p50 and p95 of what this page
	// saw), whose queue it is and how it loses.
	renderHealth: function(st) {
		const links = (st.engine && st.engine.links) || [];
		const hist = this.qhist || (this.qhist = {});
		const table = E('table', { 'class': 'table cengarde-health' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, [ _('Uplink') ]),
				E('th', { 'class': 'th' }, [ _('Type') ]),
				E('th', { 'class': 'th' }, [ _('Capacity') ]),
				E('th', { 'class': 'th' }, [ _('Queue now') ]),
				E('th', { 'class': 'th' }, [ _('Queue p50 / p95') ]),
				E('th', { 'class': 'th' }, [ _('Queue caused by') ]),
				E('th', { 'class': 'th' }, [ _('Losses') ])
			])
		]);

		cbi_update_table(table, links.map(function(l) {
			const out = (l.state == 'paused' || l.state == 'down');
			const h = hist[l.name] || (hist[l.name] = []);
			let p50, p95;

			if (!out && typeof l.queue_ms == 'number') {
				h.push(l.queue_ms);
				if (h.length > QUEUE_KEEP)
					h.shift();
			}
			p50 = percentile(h, 50);
			p95 = percentile(h, 95);
			return [
				E('span', { 'title': l.name }, [ l.label || l.name ]),
				linkType(l.type),
				out ? '-' : mbit(l.capacity_mbit),
				out ? '-' : ms(l.queue_ms),
				(p50 == null) ? '-' : '%.0f / %.0f ms'.format(p50, p95),
				out ? '-' : queueOwner(l.sig),
				out ? '-' : lossKind(l.sig)
			];
		}), E('em', {}, [ st.running ? _('No uplink is up yet.') : _('The engine is not running.') ]));
		return table;
	},

	// The protection of the upload in redundancy (redund_spare) and the
	// rescue, from the engine's upload.spare, rescue and lost_all. An engine
	// without them (before 0.9) has no box.
	renderProtection: function(st) {
		const e = st.engine;
		const n = function(v) { return (typeof v == 'number') ? v : 0; };
		let sp, rows, first, rs;

		if (!e || !e.upload || !('spare' in e.upload))
			return null;
		sp = e.upload.spare;
		rs = e.rescue || {};
		rows = [ [ _('Protection mode'), spareMode(sp ? sp.mode : 'no') ] ];
		if (sp) {
			first = (e.links || []).filter(function(l) { return l.id == sp.first; })[0];
			rows.push([ _('Main uplink'), first ? (first.label || first.name) : '-' ]);
			rows.push([ _('Now'), sp.now == 'parity' ? _('parity, one every %d packets').format(n(sp.k)) : _('full copies') ]);
			rows.push([ _('Copies on the backup uplinks'), _('%d sent, %d left out').format(n(sp.copies), n(sp.copies_skipped)) ]);
			rows.push([ _('Repair packets sent'), '%d'.format(n(sp.parities)) ]);
		}
		// The repair data with the split or a lone uplink (fec): the code,
		// and FEC v3's mode and repairs when it runs
		if (e.upload.fec && e.upload.fec.on) {
			const f = e.upload.fec, r = f.rlc;

			rows.push([ _('Repair data (FEC)'), f.code == 'rlc' ? _('RLC sliding window (FEC v3)') : _('XOR parity') ]);
			if (r && typeof r == 'object') {
				rows.push([ _('Repairs now'), r.mode == 'bonding' ? _('over the split, %s over %d ms').format(n(r.pct) + ' %', n(r.window_ms))
					: r.mode == 'single' ? _('on the lone uplink, %s over %d ms').format(n(r.pct) + ' %', n(r.window_ms))
					: n(r.quiet) > 0 ? _('none: no uplink lost lately') : _('none') ]);
				// the share follows the loss (fec_pct_min under fec_pct)
				if (r.pct_max != null && n(r.pct_min) < n(r.pct_max))
					rows.push([ _('Repair share'), _('%s to %s, by the losses').format(n(r.pct_min) + ' %', n(r.pct_max) + ' %') ]);
				rows.push([ _('Repair packets sent'), _('%d, %s of the data').format(n(r.repairs), (n(r.overhead_pm) / 10).toFixed(1) + ' %') ]);
			} else {
				rows.push([ _('Repair packets sent'), '%d'.format(n(f.parities)) ]);
			}
		}
		// what the VPS's repairs of the download rebuilt here (story 026)
		if (e.download_fec && e.download_fec.on && (e.download_fec.fec3 || n(e.download_fec.rlc_recovered)))
			rows.push([ _('Rebuilt from the VPS\'s repairs'), '%d'.format(n(e.download_fec.rlc_recovered)) ]);
		rows.push([ _('Lost on every uplink'), '%d'.format(n(e.lost_all)) ]);
		rows.push([ _('Rescue'), rs.on ? badge(_('on'), '#393') : _('off') ]);
		if (rs.on || rs.keeps)
			rows.push([ _('Rescued'), _('%d asked of the VPS, %d sent again for the VPS, %d repair packets on request, %d left to TCP').format(
				n(rs.nacks_sent), n(rs.sent), n(rs.parq && rs.parq.repairs), n(rs.left_to_tcp)) ]);
		return E('div', {}, [
			rowsTable(rows),
			E('p', { 'class': 'cbi-section-descr' }, [
				_('Lost on every uplink: packets that no uplink delivered and that nothing repaired, counted with a delay of up to a few minutes. The mode is set in the Protection tab.')
			])
		]);
	},

	// The protocontroller (Layer 4 features tab), from the engine's
	// protocontroller object: none when it is off or the engine has none.
	renderProtoctl: function(st) {
		const p = st.engine && st.engine.protocontroller;
		const n = function(v) { return (typeof v == 'number') ? v : 0; };

		if (!p || typeof p != 'object' || !p.mode || p.mode == 'no')
			return null;
		return rowsTable([
			[ _('Mode'), p.mode == 'b' ? _('B (GRIT): cengarde\'s stream in the middle') : _('A: TCP in the middle') ],
			[ _('Connections now'), '%d'.format(n(p.flows)) ],
			[ _('Connections in total'), _('%d opened, %d closed').format(n(p.opened), n(p.closed)) ],
			[ _('From the LAN / to the LAN'), '%1024.2mB / %1024.2mB'.format(n(p.sock_rx_bytes), n(p.sock_tx_bytes)) ],
			[ _('Sent again in the middle'), '%d'.format(n(p.retx)) ]
		]);
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
		let sections, reorder, protection, protoctl;

		if (!st) {
			dom.content(node, E('div', { 'class': 'alert-message error' },
				[ _('Could not read the status (is the cengarde package installed?).') ]));
			return;
		}
		sections = [
			this.renderProblems(st),
			E('h3', {}, [ _('Status') ]),
			this.renderSummary(st),
			E('h3', {}, [ _('Uplinks') ]),
			this.renderLinks(st),
			E('p', { 'class': 'cbi-section-descr' }, [
				_('Upload and download are decided apart, by the end that sends: an uplink can be on standby one way and in use the other, as is usual when carriers are faster one way (upload through one, download through another). On standby it carries only probes until it catches up with the best, and at least the minimum of active uplinks (Advanced tab) always stays in use.'),
				' ',
				_('Behind the fastest: how much later this uplink delivers the upload than the quickest one; past the limit its upload goes on standby. Arrived first: share of the download that came through this uplink before any other copy. Pause: takes an uplink out without touching the configuration, until you resume it or cengarde restarts.'),
				' ',
				_('VPS address: where the uplink sends to; "failover" when the first VPS address it can use did not answer. Path MTU: the largest packet the path to the VPS takes.')
			])
		];
		sections.push(E('h3', {}, [ _('Uplink health') ]), this.renderHealth(st),
			E('p', { 'class': 'cbi-section-descr' }, [
				_('Capacity: what the engine measures that the uplink carries now. Queue: how much longer than its fastest the uplink takes now, and its median and 95th percentile over the last five minutes on this page. Queue caused by: this router\'s own traffic, or other users of the cell. Losses: at random with no queue (repair data helps), or when the queue overflows (sending less helps).')
			]));
		protection = this.renderProtection(st);
		if (protection)
			sections.push(E('h3', {}, [ _('Protection') ]), protection);
		protoctl = this.renderProtoctl(st);
		if (protoctl)
			sections.push(E('h3', {}, [ _('Protocontroller') ]), protoctl);
		// Only an engine that reorders reports it.
		reorder = this.renderReorder(st);
		if (reorder)
			sections.push(E('h3', {}, [ _('Reordering') ]), reorder);
		dom.content(node, sections);
	},

	render: function(st) {
		const node = E('div', { 'class': 'cbi-section' });

		this.node = node;
		this.update(node, st);
		poll.add(L.bind(this.refresh, this), 3);

		return E([], [
			E('h2', {}, [ _('cengarde') ]),
			E('div', { 'class': 'cbi-map-descr' }, [
				_('Every packet goes through all active uplinks and the other end keeps the first copy; with the split on, each big packet goes through one uplink and the other end puts them back in order.')
			]),
			node
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
