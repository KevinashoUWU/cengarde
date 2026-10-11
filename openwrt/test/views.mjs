// Checks of the LuCI views that need no router, VM or browser: they run
// config.js and status.js against a small stand-in for LuCI (the lint job runs
// this with plain node; luci.mjs is the test with the real thing):
//
//   node views.mjs
//
// - config.js: the reordering and split options sit in the Advanced tab with
//   the ranges of the engine, the weights of the split take NAME=WEIGHT for an
//   uplink of the router, from 0 to 1000, once each, and the options that
//   mean nothing without their switch depend on it;
// - status.js: the "Reordering" box shows the engine's "reorder" object (and
//   says so when the engine has it off) and is not there for an engine without
//   it, in English and in Spanish (po/es), which also has to cover its strings.
//
// SPDX-License-Identifier: GPL-2.0-only

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const dir = new URL('../luci-app-cengarde/', import.meta.url);
const view = (name) => readFileSync(new URL(`htdocs/luci-static/resources/view/cengarde/${name}.js`, dir), 'utf8');

// LuCI's String.prototype.format, for the conversions the views use.
String.prototype.format = function (...args) {
	let i = 0;

	return this.replace(/%(?:1024)?(?:\d*\.\d+|\d+)?[a-zA-Z]\w?/g, () => String(args[i++]));
};

// The Spanish translations, as LuCI would load them from po/es.
function po() {
	const out = {};
	let key = null, cur = null;
	const un = (s) => s.replace(/\\(.)/g, (_, c) => (c === 'n' ? '\n' : c));

	for (const line of readFileSync(new URL('po/es/cengarde.po', dir), 'utf8').split('\n')) {
		const m = /^(msgid|msgstr) "(.*)"$/.exec(line), c = /^"(.*)"$/.exec(line);

		if (m && m[1] === 'msgid') { key = un(m[2]); cur = 'id'; }
		else if (m) { out[key] = un(m[2]); cur = 'str'; }
		else if (c && cur === 'id') key += un(c[1]);
		else if (c && cur === 'str') out[key] += un(c[1]);
	}
	return out;
}
const es = po();
const missing = [];
const english = (s) => s;
const spanish = (s) => {
	if (!es[s]) missing.push(s);
	return es[s] || s;
};

// A DOM of plain objects: enough to read text back.
function E(tag, attrs, children) {
	if (Array.isArray(attrs) || typeof attrs === 'string') { children = attrs; attrs = {}; }
	return { tag, attrs: attrs || {}, children: [].concat(children ?? []), style: {}, value: '', select() {} };
}
const text = (n) => (typeof n === 'string' ? n : n == null ? '' : n.children.map(text).join(''));
const find = (n, pred, out = []) => {
	if (n && typeof n == 'object') {
		if (pred(n)) out.push(n);
		n.children.forEach((c) => find(c, pred, out));
	}
	return out;
};

// ---- config.js

class Opt {
	constructor(cls, tab, name, title, descr) { Object.assign(this, { cls, tab, name, title, descr, deps: [] }); }
	getUIElement() { return this.ui || null; }
	formvalue() { return this.ui ? this.ui.value : undefined; }
	depends(d, v) { this.deps.push(typeof d == 'string' ? { [d]: v } : d); }
	value() {}
}
class Value extends Opt { renderWidget() { return E('input'); } }
class Flag extends Opt {}
class DynamicList extends Opt { renderWidget() { return E('div'); } }
class DummyValue extends Opt {}
class ListValue extends Opt {}

async function renderConfig(tr, uplinks) {
	const options = {};
	class Map {
		constructor() {
			this.s = { tab() {}, taboption: (tab, C, name, title, descr) => (options[name] = new C(C.name, tab, name, title, descr)) };
		}
		section() { return this.s; }
		lookupOption(name) { return options[name] ? [ options[name] ] : null; }
		render() { return Promise.resolve(); }
	}
	const form = { Map, NamedSection: class {}, Value, Flag, DynamicList, DummyValue, ListValue };
	const document = { addEventListener() {}, getElementById() { return {}; }, body: E('body') };
	const saved = { secret: 'A'.repeat(43) + '=', uplink: uplinks };
	const uci = { get: (c, s, o) => (c === 'cengarde' && s === 'main' ? saved[o] : undefined), load: async () => {} };
	const fs = { exec_direct: async () => ({}), read: async () => '' };
	const L = { resolveDefault: (p, d) => p.catch(() => d), toArray: (x) => [].concat(x ?? []), bind: (f) => f };
	const ui = { createHandlerFn: (t, f) => f, addTimeLimitedNotification() {} };
	const factory = new Function('view', 'form', 'fs', 'ui', 'uci', 'widgets', 'L', 'E', '_', 'document', 'window', 'navigator',
		'URL', 'Blob', 'confirm', view('config'));
	const v = factory({ extend: (o) => o }, form, fs, ui, uci, { NetworkSelect: class extends Opt {} }, L, E, tr, document, {}, {}, {},
		class {}, () => true);

	await v.render(await v.load());
	return options;
}

for (const [lang, tr] of [ [ 'en', english ], [ 'es', spanish ] ]) {
	const o = await renderConfig(tr, [ 'wan', 'lte2' ]);
	const advanced = (name) => { assert.ok(o[name], `${name} exists`); assert.equal(o[name].tab, 'advanced', `${name} is in the Advanced tab`); return o[name]; };

	// the switches and values the engine takes
	assert.equal(advanced('reorder').cls, 'Flag');
	assert.equal(o.reorder.default, '1', 'reordering is on unless turned off');
	assert.equal(o.reorder.rmempty, false);
	assert.equal(advanced('split').cls, 'Flag');
	assert.notEqual(o.split.default, '1', 'the split is off unless turned on');
	assert.equal(o.split.rmempty, false);
	assert.equal(advanced('reorder_max_wait_ms').datatype, 'range(0,2000)');
	assert.equal(o.reorder_max_wait_ms.placeholder, '200');
	assert.equal(advanced('reorder_bypass').datatype, 'range(0,2048)');
	assert.equal(o.reorder_bypass.placeholder, '128');
	assert.equal(advanced('split_weight').cls, 'DynamicList');

	// what means nothing without its switch is hidden without it
	assert.deepEqual(o.reorder_max_wait_ms.deps, [ { reorder: '1' } ]);
	assert.deepEqual(o.reorder_bypass.deps, [ { reorder: '1' } ]);
	assert.deepEqual(o.split.deps, [ { reorder: '1' } ]);
	assert.deepEqual(o.split_weight.deps, [ { reorder: '1', split: '1' } ]);
	assert.deepEqual(o.reorder.deps, []);

	// the experimental split says what it does badly, and that the VPS follows
	for (const word of lang === 'en' ? [ 'experimental', '300 ms', 'latency', 'queue', 'The VPS follows' ]
		: [ 'experimental', '300 ms', 'latencia', 'cola', 'El VPS sigue' ])
		assert.ok((o.split.title + o.split.descr).includes(word), `${lang}: the split option mentions "${word}"`);

	// the weights: NAME=WEIGHT, an uplink of the router, 0 to 1000, once each
	const check = (entry, listed = []) => {
		o.split_weight.ui = { value: listed };
		return o.split_weight.validate('main', entry);
	};
	for (const good of [ 'wan=0', 'wan=1', 'wan=3', 'lte2=999', 'lte2=1000', 'wan=10', 'wan=100' ])
		assert.equal(check(good), true, `${good} is valid`);
	for (const bad of [ 'wan', 'wan=', '=3', 'wan=-1', 'wan=1001', 'wan=2000', 'wan=10000', 'wan=007', 'wan=01', 'wan=2.5', 'wan=x',
		'wan = 3', ' wan=3', 'wan=3 ', 'wan=3=4', 'wan:3', 'WAN=3', 'lte=3', 'up1=3' ])
		assert.notEqual(check(bad), true, `${bad} is refused`);
	assert.notEqual(check('wan=2', [ 'wan=3' ]), true, 'an uplink with a weight already is refused');
	// LuCI's DynamicList puts the valid entry being typed in its own value: it
	// is not another entry for the same uplink
	assert.equal(check('wan=2', [ 'wan=2' ]), true, 'the entry being typed is not counted twice');
	assert.equal(check('wan=0', [ 'lte2=3', 'wan=0' ]), true, 'nor with others listed');
	assert.notEqual(check('wan=2', [ 'wan=3', 'wan=2' ]), true, 'another entry of the same uplink still is');
	assert.equal(check('lte2=2', [ 'wan=3' ]), true, 'another uplink is taken');
	assert.equal(check('wan=2', [ 'lte2=3' ]), true);
	// the message says what to fix
	assert.ok(String(check('lte=3')).includes('lte'), 'the unknown uplink is named');
	assert.ok(String(check('wan=2', [ 'wan=3' ])).includes('wan'), 'the repeated uplink is named');
	// no uplinks chosen yet: nothing to weigh
	const none = await renderConfig(tr, []);
	none.split_weight.ui = { value: [] };
	assert.notEqual(none.split_weight.validate('main', 'wan=2'), true, 'without uplinks no entry is valid');
	// a single uplink saved as a string, as UCI hands one over
	const one = await renderConfig(tr, 'wan');
	one.split_weight.ui = { value: [] };
	assert.equal(one.split_weight.validate('main', 'wan=2'), true, 'a single uplink');
}

// ---- status.js

function loadStatus(tr) {
	const dom = { content: (node, children) => { node.children = [].concat(children); } };
	const factory = new Function('view', 'dom', 'fs', 'poll', 'ui', 'L', 'E', '_', 'cbi_update_table', view('status'));

	return factory({ extend: (o) => o }, dom, {}, {}, { createHandlerFn: (t, f) => f }, { bind: (f) => f }, E, tr, () => {});
}

const reorder = {
	enabled: true, held: 3, held_max: 41, pool_buffers: 4096, pool_free: 4093, in_order: 100000, held_total: 12345,
	bypassed: 7, split_rx: 9, lost: { rack: 1, cap: 5, overflow: 2, flush: 0 },
	late: { after_rack: 1, after_cap: 4, after_overflow: 2, old: 3 }, far: 0, resyncs: 0, pool_full: 0, max_burst: 12,
	edges_ms: [ 1, 2, 5, 10, 20, 50, 100, 200, 500 ], wait: [ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 ], late_by: [ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 ]
};
const base = (engine) => ({
	enabled: true, running: true, protocol: 4, tunnel_addr: '10.79.1.2', tunnel_ipv6: false,
	tunnel: { handshake: 0, now: 1000, uptime: 500 }, problems: [],
	engine: engine && { version: '0.5.5', upload: { bytes: 1, largest: 1 }, download: { bytes: 1, duplicates: 0 }, links: [], ...engine }
});
// The rows of the Reordering box as [ name, value ] text, or null when there is no box.
const box = (v, st) => {
	const node = E('div');
	let heading;

	v.update(node, st);
	heading = find(node, (n) => n.tag == 'h3').map(text);
	if (!heading.some((h) => h == 'Reordering' || h == 'Reordenamiento'))
		return null;
	return find(node.children[node.children.length - 1], (n) => n.tag == 'tr').map((r) => r.children.map(text));
};

for (const [lang, tr] of [ [ 'en', english ], [ 'es', spanish ] ]) {
	const v = loadStatus(tr);
	const L = (en, sp) => (lang == 'en' ? en : sp);

	// no object, no box: an engine from before the reorderer, a stopped engine, no status
	assert.equal(box(v, base({})), null, `${lang}: no "reorder" object, no box`);
	assert.equal(box(v, base(null)), null, `${lang}: no engine status, no box`);
	assert.equal(box(v, base({ reorder: null })), null, `${lang}: a null "reorder", no box`);
	assert.equal(box(v, base({ reorder: 'on' })), null, `${lang}: a "reorder" that is no object, no box`);
	v.update(E('div'), null);

	// the box
	const rows = box(v, base({ reorder }));
	assert.ok(rows, `${lang}: the box shows`);
	const row = (rows, name) => (rows.find((r) => r[0] == name) || assert.fail(`${lang}: no row "${name}" in ${JSON.stringify(rows)}`))[1];
	const HELD = L('Held now', 'Retenidos ahora');
	const GIVEN_UP = L('Missing packets given up on', 'Faltantes dados por perdidos');
	const LATE = L('Late after release', 'Llegaron después de darse por perdidos');
	assert.equal(rows.length, 5);
	assert.equal(row(rows, HELD), L('3 of 4096', '3 de 4096'));
	assert.equal(row(rows, L('Most held at once', 'Máximo retenido a la vez')), '41');
	assert.equal(row(rows, L('Held in total', 'Retenidos en total')), '12345');
	assert.equal(row(rows, GIVEN_UP), L('5 at the deadline, 2 for lack of room', '5 por el plazo, 2 por falta de lugar'));
	assert.equal(row(rows, LATE), '10'); // 4 after the deadline + 2 after overflow + 1 after RACK + 3 old

	// the pieces an engine may leave out count as zero, and the room as unknown
	const sparse = box(v, base({ reorder: { enabled: true, held: 1 } }));
	assert.equal(row(sparse, HELD), '1');
	assert.equal(row(sparse, LATE), '0');
	assert.equal(row(sparse, GIVEN_UP), L('0 at the deadline, 0 for lack of room', '0 por el plazo, 0 por falta de lugar'));

	// off: the box says so instead of showing zeros
	const node = E('div');
	v.update(node, base({ reorder: { enabled: false } }));
	const last = node.children[node.children.length - 1];
	assert.equal(text(last), L('Reordering is off (Advanced tab).', 'El reordenamiento está desactivado (pestaña Avanzado).'));
	assert.equal(find(last, (n) => n.tag == 'tr').length, 0);

	// the rest of the page is still there
	const page = E('div');
	v.update(page, base({ reorder }));
	assert.deepEqual(find(page, (n) => n.tag == 'h3').map(text),
		L([ 'Status', 'Uplinks', 'Reordering' ], [ 'Estado', 'Enlaces', 'Reordenamiento' ]));
}
assert.deepEqual([ ...new Set(missing) ], [], 'every string the views use has a Spanish translation');

console.log('views: ok');
