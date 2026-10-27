// End-to-end test of luci-app-cengarde with Playwright (Chromium), in
// Spanish as a browser set to es-CL gets it:
//
//   node luci.mjs [BASE_URL] [SCREENSHOT_DIR]
//
// Against the router VM of vm.sh (uplinks up1-up3, VPS at 1.2.3.4, see
// e2e.sh): configures cengarde from the web UI only (VPS addresses,
// uplinks, IP pass, enable), checks that an address with a mask, an MTU of
// 1420 and a VPS failover time below three probe intervals are refused and
// that the cloud-config follows the form (the IP pass and IPv6 through the
// tunnel switches too: the second is off by default and left off), applies,
// and waits on the status page for the tunnel, the three uplinks and the
// VPS confirming IP pass; then pauses an uplink there and resumes it.
// In the VPS tab, the cloud-config and the command that adds this router to a
// VPS that serves others carry its name and the tunnel address derived from
// the secret (which only the router can compute), and follow a name and an
// address typed in the form; invalid ones are refused. The VPS addresses take
// host names too (not with a mask or a space). In the Advanced tab, with the
// split on and two weights entered, the settings save and apply (LuCI checks
// the list of weights as a whole when it saves), and the engine's
// configuration on the router (read over ssh with vm.sh, as e2e.sh does)
// carries the split and the two weights, and the type of up1, set to
// Starlink in the dropdown each uplink gets in the General tab (the others
// stay auto); then the split goes off again before the final save and apply.
// Fails on any JavaScript error after logging in.
//
//   node luci.mjs BASE_URL SCREENSHOT_DIR names
//
// instead only logs in and checks the status page of the "VPS name" step of
// e2e.sh: vps.test with its address and how long ago the DNS answered, and
// nope.test, which nobody knows, as a problem.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { chromium } from 'playwright';
import { execFileSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';

const base = process.argv[2] || 'http://127.0.0.1:8080';
const shots = process.argv[3] || 'screenshots';
// The engine sticks to the first address that answers: 1.2.3.5 is never
// tried while 1.2.3.4 works.
const VPS = [ '1.2.3.4', '1.2.3.5' ];
const UPLINKS = [ 'up1', 'up2', 'up3' ];
const errors = [];
let loggedIn = false;

mkdirSync(shots, { recursive: true });
const browser = await chromium.launch();
const page = await browser.newPage({ locale: 'es-CL', viewport: { width: 1280, height: 900 } });
page.setDefaultTimeout(60000);
// The login page itself probes some RPCs before there is a session.
page.on('pageerror', (e) => { if (loggedIn) errors.push(e.message); });

const id = (s) => `[id="${s}"]`;
const step = (s) => console.log(`luci: ${s}`);
// A full-page screenshot taken while a text field has focus leaves Chromium
// scrolled to the bottom of the page until the field loses focus, and then
// the page jumps back to where it was. If that happens inside the next click
// (its mousedown takes the focus away), the button moves away before the
// mouseup and the click is lost: with the weights' input focused, Save &
// Apply did nothing. So no field keeps the focus through a screenshot.
async function shot(name) {
	await page.evaluate(() => document.activeElement && document.activeElement.blur());
	await page.screenshot({ path: `${shots}/${name}.png`, fullPage: true });
}

function check(cond, what) {
	if (!cond)
		throw new Error(`check failed: ${what}`);
}

// A command on the router VM (vm.sh ssh router), its output.
function onRouter(cmd) {
	return execFileSync(new URL('vm.sh', import.meta.url).pathname, [ 'ssh', 'router', cmd ], { encoding: 'utf8' });
}

// The [link DEV] section of an engine configuration.
const linkSection = (conf, dev) => conf.split('\n\n').find((b) => b.startsWith(`[link ${dev}]`)) || '';

// Save and apply from the page's buttons: true once LuCI says the changes
// are applied, else the save error LuCI shows instead (the form refused
// them), or that neither came within 2 minutes.
async function saveApply() {
	const never = new Promise(() => {});
	const applied = page.evaluate(() => new Promise((resolve) =>
		document.addEventListener('uci-applied', () => resolve(true), { once: true })));
	const refused = page.waitForSelector('.modal em', { timeout: 120000 })
		.then((e) => e.innerText(), () => never);
	const late = new Promise((resolve) => setTimeout(() => resolve('no answer in 2 minutes'), 120000).unref());

	await page.click('.cbi-page-actions .cbi-button-apply li[data-value="0"]');
	return Promise.race([ applied, refused, late ]);
}

// Polls the status page (it refreshes itself every 3 s) until cond holds.
async function until(cond, what, ms = 120000) {
	const deadline = Date.now() + ms;

	while (Date.now() < deadline) {
		if (await cond())
			return;
		await page.waitForTimeout(1000);
	}
	throw new Error(`timed out waiting for ${what}`);
}

const invalid = (field) => field.evaluate((e) => e.classList.contains('cbi-input-invalid'));
const activeRows = () => page.locator('table.cengarde-links tr.tr:has-text("activo")').count();
const row = (label) => page.locator(`table.cengarde-links tr.tr:has-text("${label} (")`);

// LuCI gives checkboxes a random id; data-widget-id names them.
async function flag(name, on) {
	const box = page.locator(`[data-widget-id="widget.cbid.cengarde.main.${name}"]`);

	await box.setChecked(on);
	check(await box.isChecked() === on, `${name} is ${on ? 'on' : 'off'}`);
}

async function tab(name) {
	await page.click(`ul.cbi-tabmenu li[data-tab="${name}"] a`);
	await page.waitForTimeout(300);
}

// The status page while e2e.sh has vps.test (the uplinks' DNS says 1.2.3.4) and
// nope.test (nobody knows it) as the VPS names.
async function namesStatus() {
	const nameRow = (n) => page.locator(`table.table tr.tr:has-text("Nombre del VPS ${n}")`);
	const problem = (n) => page.locator(`.cengarde-problem:has-text("El nombre del VPS ${n} no se pudo resolver")`);

	step('status page: the VPS names');
	await page.goto(`${base}/cgi-bin/luci/admin/services/cengarde/status`);
	await until(async () => await nameRow('vps.test').count() > 0 && await nameRow('nope.test').count() > 0 &&
		await problem('nope.test').count() > 0, 'the VPS names on the status page', 60000);
	await shot('07-status-names');
	const found = await nameRow('vps.test').innerText();
	check(found.includes('1.2.3.4') && /resuelto hace \d+ (s|min)/.test(found), `vps.test with its address and its age (${found})`);
	check((await nameRow('nope.test').locator('td').nth(1).innerText()).trim() === '-', 'nope.test has no address');
	check(await problem('vps.test').count() === 0, 'vps.test is no problem');
	check(errors.length === 0, `no JavaScript errors (${errors.join('; ')})`);
}

try {
	step('login');
	await page.goto(`${base}/cgi-bin/luci/`);
	await page.fill('input[name=luci_username]', 'root');
	await page.fill('input[name=luci_password]', '');
	await Promise.all([ page.waitForNavigation(), page.press('input[name=luci_password]', 'Enter') ]);
	loggedIn = true;
	if (process.argv[4] === 'names') {
		await namesStatus();
		step('ok');
		await browser.close();
		process.exit(0);
	}

	step('settings page');
	await page.goto(`${base}/cgi-bin/luci/admin/services/cengarde/config`);
	await page.waitForSelector(id('widget.cbid.cengarde.main.server'));
	await shot('01-settings-empty');

	step('VPS addresses: a mask is refused, names are taken, two addresses go in');
	// Typed key by key: LuCI checks the field on keyup, and Enter only adds
	// a valid entry to the list.
	const server = page.locator(id('widget.cbid.cengarde.main.server'));
	const listed = () => page.locator(`${id('cbid.cengarde.main.server')} .item input[type="hidden"]`)
		.evaluateAll((items) => items.map((e) => e.value));
	await server.pressSequentially('1.2.3.4/24');
	await server.press('Enter');
	check(await invalid(server) && (await listed()).length === 0, '1.2.3.4/24 is refused');
	// A name is taken as it is typed (not entered: it would stay in the list)
	// unless it has a space, starts with - or is a number with dots that is no
	// address.
	await server.fill('');
	await server.pressSequentially('vps.example.com');
	check(!await invalid(server), 'a name is taken');
	for (const bad of [ '-vps.example.com', 'vps example.com', '10.0.0.256' ]) {
		await server.fill('');
		await server.pressSequentially(bad);
		check(await invalid(server), `${bad} is refused`);
	}
	await server.fill('');
	for (const addr of VPS) {
		await server.pressSequentially(addr);
		await server.press('Enter');
	}
	check(JSON.stringify(await listed()) === JSON.stringify(VPS), `the VPS addresses, in order (${await listed()})`);

	await page.click(id('cbid.cengarde.main.uplink'));
	for (const up of UPLINKS) {
		// the open dropdown also keeps a hidden copy of each item
		const item = page.locator(`${id('cbid.cengarde.main.uplink')} li[data-value="${up}"]`)
			.filter({ visible: true }).first();
		if (await item.getAttribute('selected') === null)
			await item.click();
	}
	await page.click('h2[name="content"], .cbi-map h2');
	await flag('enabled', true);
	await shot('02-settings-general');

	step('General tab: a type dropdown for each uplink chosen, auto unless set; up1 is Starlink');
	const linkType = (up) => page.locator(id(`widget.cbid.cengarde.main.uplink_type_${up}`));
	for (const up of UPLINKS) {
		check(await linkType(up).isVisible(), `${up} has a type dropdown`);
		check(await linkType(up).inputValue() === 'auto', `${up} is auto by default`);
	}
	await linkType('up1').selectOption('starlink');
	check(await linkType('up1').inputValue() === 'starlink', 'up1 set to starlink');

	step('VPS tab: secret and cloud-config');
	await tab('vps');
	const secret = await page.inputValue(id('widget.cbid.cengarde.main.secret'));
	check(/^[A-Za-z0-9+/]{43}=$/.test(secret), 'a secret was generated on first boot');
	let cc = await page.inputValue('#cengarde-cloud-config');
	check(cc.includes(`\n      ${secret}\n`), 'the cloud-config carries the secret');
	check(/CENGARDE_REF=[0-9a-f]{40};/.test(cc), 'the cloud-config pins the commit of the package');
	check(/PASSTHROUGH=no/.test(cc), 'no passthrough while IP pass is off');
	check(/^\s*ROUTER_NAME=router$/m.test(cc), 'the cloud-config names the router "router" by default');
	const derived = (/^\s*ROUTER_TUNNEL_ADDR=(10\.79\.\d+\.\d+)$/m.exec(cc) || [])[1];
	check(derived, 'the cloud-config carries the tunnel address derived from the secret');
	const addCommand = () => page.inputValue('#cengarde-add-command');
	const proto = (/^sudo cengarde-vps-setup add router --tunnel-addr \S+ --proto (\d+)$/.exec(await addCommand()) || [])[1];
	check(proto, `the command to add this router to a VPS (${await addCommand()})`);
	check(await addCommand() === `sudo cengarde-vps-setup add router --tunnel-addr ${derived} --proto ${proto}`,
		'the command carries the same address as the cloud-config');
	check(!(await addCommand()).includes(secret), 'the secret is not in the command');
	check(await page.locator(id('widget.cbid.cengarde.main.tunnel_addr')).getAttribute('placeholder') === derived,
		'the address field shows the derived one while empty');
	await shot('03-settings-vps');

	step('VPS tab: a name and an address typed in the form, invalid ones refused');
	const vpsName = page.locator(id('widget.cbid.cengarde.main.vps_name'));
	const tunnelAddr = page.locator(id('widget.cbid.cengarde.main.tunnel_addr'));
	const typeIn = async (field, v) => {
		await field.fill('');
		if (v)
			await field.pressSequentially(v);
		await field.blur();
	};
	for (const bad of [ 'Casa', '-casa', 'default', 'una-casa-muy-larga' ]) {
		await typeIn(vpsName, bad);
		check(await invalid(vpsName), `the name ${bad} is refused`);
	}
	await typeIn(vpsName, 'casa-ana');
	check(!await invalid(vpsName), 'the name casa-ana is taken');
	for (const bad of [ '10.79.0.1', '10.79.0.0', '10.79.255.255', '10.80.1.2', '10.79.1.256', '10.79.01.2' ]) {
		await typeIn(tunnelAddr, bad);
		check(await invalid(tunnelAddr), `the address ${bad} is refused`);
	}
	await typeIn(tunnelAddr, '10.79.7.8');
	check(!await invalid(tunnelAddr), 'the address 10.79.7.8 is taken');
	cc = await page.inputValue('#cengarde-cloud-config');
	check(/^\s*ROUTER_NAME=casa-ana$/m.test(cc) && /^\s*ROUTER_TUNNEL_ADDR=10\.79\.7\.8$/m.test(cc),
		'the cloud-config follows the name and the address in the form');
	check(await addCommand() === `sudo cengarde-vps-setup add casa-ana --tunnel-addr 10.79.7.8 --proto ${proto}`,
		'the command follows the name and the address in the form');
	await typeIn(tunnelAddr, '');
	await typeIn(vpsName, '');
	cc = await page.inputValue('#cengarde-cloud-config');
	check(/^\s*ROUTER_NAME=router$/m.test(cc) && cc.includes(`ROUTER_TUNNEL_ADDR=${derived}\n`) &&
		await addCommand() === `sudo cengarde-vps-setup add router --tunnel-addr ${derived} --proto ${proto}`,
		'emptied, the name is router again and the address the derived one');

	step('Tunnel tab: an MTU of 1420 is refused, IP pass and IPv6 on, the cloud-config follows');
	await tab('tunnel');
	const mtu = page.locator(id('widget.cbid.cengarde.main.mtu'));
	const mtuSaved = await mtu.inputValue();
	await mtu.fill('');
	await mtu.pressSequentially('1420');
	check(await invalid(mtu), 'an MTU of 1420 is refused');
	await mtu.fill('');
	await mtu.pressSequentially(mtuSaved);
	await mtu.blur();
	check(!await invalid(mtu), `the MTU is valid again (${mtuSaved || 'default'})`);
	await flag('ip_pass', true);
	const ipv6 = page.locator('[data-widget-id="widget.cbid.cengarde.main.tunnel_ipv6"]');
	check(await ipv6.count() === 1 && !await ipv6.isChecked(),
		'IPv6 through the tunnel is a switch of the Tunnel tab, off by default');
	await flag('tunnel_ipv6', true);
	await shot('04-settings-tunnel');
	await tab('vps');
	cc = await page.inputValue('#cengarde-cloud-config');
	check(/PASSTHROUGH=yes/.test(cc), 'the cloud-config follows the IP pass switch before saving');
	check(/^\s*TUNNEL_IPV6=yes$/m.test(cc), 'the cloud-config follows the IPv6 switch before saving');
	await tab('tunnel');
	await flag('tunnel_ipv6', false);
	await tab('vps');
	cc = await page.inputValue('#cengarde-cloud-config');
	check(/^\s*TUNNEL_IPV6=no$/m.test(cc) && /PASSTHROUGH=yes/.test(cc), 'and goes back to no with it, IP pass on');

	step('Advanced tab: the VPS failover time follows the probe interval');
	await tab('advanced');
	const failover = page.locator(id('widget.cbid.cengarde.main.server_failover_ms'));
	const probe = page.locator(id('widget.cbid.cengarde.main.probe_interval_ms'));
	const type = async (field, v) => {
		await field.fill('');
		if (v)
			await field.pressSequentially(v);
		await field.blur();
	};
	await type(probe, '2000');
	await type(failover, '5000');
	check(await invalid(failover), 'failover 5000 is refused with probes every 2000 ms');
	await type(failover, '6000');
	check(!await invalid(failover), 'failover 6000 is taken with probes every 2000 ms');
	await type(failover, '');
	await type(probe, '');
	check(!await invalid(failover) && !await invalid(probe), 'both back to their defaults');

	step('Advanced tab: reordering is on and the split off; the weights are for uplinks, 0 to 1000');
	const maxWait = page.locator(id('widget.cbid.cengarde.main.reorder_max_wait_ms'));
	const bypass = page.locator(id('widget.cbid.cengarde.main.reorder_bypass'));
	const reorder = page.locator('[data-widget-id="widget.cbid.cengarde.main.reorder"]');
	const split = page.locator('[data-widget-id="widget.cbid.cengarde.main.split"]');
	const weights = page.locator(id('widget.cbid.cengarde.main.split_weight'));
	check(await page.locator('text=Reordenar en el receptor').count() > 0, 'the reordering option is in Spanish');
	check(await reorder.isChecked(), 'reordering at the receiver is on by default');
	check(!await split.isChecked(), 'the split is off by default');
	check(!await weights.isVisible(), 'the weights are hidden while the split is off');
	for (const [ field, bad, good ] of [ [ maxWait, '2001', '2000' ], [ bypass, '2049', '0' ] ]) {
		await type(field, bad);
		check(await invalid(field), `${bad} is refused`);
		await type(field, good);
		check(!await invalid(field), `${good} is taken`);
		await type(field, '');
	}
	await flag('split', true);
	check(await weights.isVisible(), 'the weights show with the split on');
	check(await page.locator('text=experimental').count() > 0, 'the split says it is experimental');
	// up1 to up3 are the uplinks chosen in the General tab
	for (const bad of [ 'up1', 'up1=1001', 'up1=007', 'up1=-1', 'up9=2' ]) {
		await type(weights, bad);
		check(await invalid(weights), `the weight ${bad} is refused`);
	}
	for (const good of [ 'up1=0', 'up2=3', 'up3=1000' ]) {
		await type(weights, good);
		check(!await invalid(weights), `the weight ${good} is taken`);
	}
	await type(weights, '');

	step('Advanced tab: with the split on and two weights, the settings save and apply, and the engine gets them');
	const weightList = page.locator(id('cbid.cengarde.main.split_weight'));
	const listedWeights = () => weightList.locator('.item input[type="hidden"]').evaluateAll((items) => items.map((e) => e.value));
	for (const w of [ 'up1=3', 'up2=0' ]) {
		await type(weights, w);
		await weights.press('Enter');
	}
	check(JSON.stringify(await listedWeights()) === '["up1=3","up2=0"]', `two weights listed (${await listedWeights()})`);
	check(!await invalid(weightList), 'the list of weights is valid as a whole');
	await shot('04b-settings-advanced');
	const res = await saveApply();
	check(res === true, `saved and applied with the split on (${res})`);
	// The engine's configuration, as e2e.sh reads it, and the file the engine
	// was started with (the apply writes it).
	const conf = onRouter('cengarde-setup engine-conf');
	check(/^split = on$/m.test(conf), 'engine-conf: split = on');
	check(/^weight = 3$/m.test(linkSection(conf, 'eth1')) && /^weight = 0$/m.test(linkSection(conf, 'eth2')) &&
		!/^weight/m.test(linkSection(conf, 'eth3')) && (conf.match(/^weight = /mg) || []).length === 2,
	`engine-conf: weight 3 on up1 (eth1) and 0 on up2 (eth2), none on up3:\n${conf.replace(/^key = .*$/m, 'key = ...')}`);
	check(/^type = starlink$/m.test(linkSection(conf, 'eth1')) && (conf.match(/^type = /mg) || []).length === 1,
		'engine-conf: type starlink on up1 (eth1), none on the others (auto)');
	await until(async () => onRouter("grep -c '^weight = ' /var/etc/cengarde.conf || true").trim() === '2',
		'the engine running with the two weights', 30000);
	await page.waitForTimeout(3000); // LuCI reloads the page after an apply
	await page.goto(`${base}/cgi-bin/luci/admin/services/cengarde/config`);
	await page.waitForSelector(id('widget.cbid.cengarde.main.server'));
	await tab('advanced');
	check(await split.isChecked() && JSON.stringify(await listedWeights()) === '["up1=3","up2=0"]',
		'the page shows the split and the weights as saved');
	await tab('general');
	check(await linkType('up1').inputValue() === 'starlink' && await linkType('up2').inputValue() === 'auto',
		'the page shows up1 as Starlink and up2 as auto, as saved');
	await tab('advanced');
	await flag('reorder', false);
	check(!await split.isVisible() && !await maxWait.isVisible(), 'without reordering the split and its settings are hidden');
	await flag('reorder', true);
	await flag('split', false);
	check(!await weights.isVisible(), 'the weights hide again with the split off');

	step('save and apply');
	await tab('general');
	const applied = page.evaluate(() => new Promise((resolve) =>
		document.addEventListener('uci-applied', () => resolve(true), { once: true })));
	await page.click('.cbi-page-actions .cbi-button-apply li[data-value="0"]');
	await applied;
	await page.waitForTimeout(3000);

	step('status page: tunnel and uplinks');
	await page.goto(`${base}/cgi-bin/luci/admin/services/cengarde/status`);
	const deadline = Date.now() + 120000;
	let rows = 0, connected = false;
	while (Date.now() < deadline) {
		rows = await activeRows();
		connected = await page.locator('text=conectado').count() > 0;
		if (rows === UPLINKS.length && connected)
			break;
		await page.waitForTimeout(3000);
	}
	await shot('05-status');
	check(connected, 'the tunnel is connected');
	check(rows === UPLINKS.length, `all ${UPLINKS.length} uplinks active (got ${rows})`);
	check(await page.locator('table.cengarde-links th:text-is("Subida")').count() === 1 &&
		await page.locator('table.cengarde-links th:text-is("Bajada")').count() === 1,
		'upload and download each have a column (in use or on standby)');
	check(await page.locator('table.cengarde-links:has-text("silenciado")').count() === 0, 'no uplink reads "silenciado"');
	check(await page.locator('.cengarde-problem').count() === 0, 'no problems reported');
	check(await page.locator(`td:has-text("${derived}")`).count() > 0, 'the status shows the tunnel address');

	step('status page: the VPS confirms IP pass');
	await until(async () => await page.locator('text=activo en el VPS').count() > 0, 'IP pass on at the VPS', 30000);

	step('status page: pause up3, then resume it');
	await row('up3').locator('button:has-text("Pausar")').click();
	await until(async () => await row('up3').locator('text=en pausa').count() > 0 &&
		await activeRows() === UPLINKS.length - 1, 'up3 paused');
	await shot('06-status-paused');
	await row('up3').locator('button:has-text("Reanudar")').click();
	await until(async () => await activeRows() === UPLINKS.length, 'up3 back');
	check(await row('up3').locator('button:has-text("Pausar")').count() === 1, 'up3 can be paused again');

	check(errors.length === 0, `no JavaScript errors (${errors.join('; ')})`);
	step('ok');
} catch (e) {
	await shot('99-failure').catch(() => {});
	console.error(`luci: ${e.message}`);
	process.exitCode = 1;
} finally {
	await browser.close();
}
