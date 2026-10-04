// End-to-end test of luci-app-cengarde with Playwright (Chromium), in
// Spanish as a browser set to es-CL gets it:
//
//   node luci.mjs [BASE_URL] [SCREENSHOT_DIR]
//
// Against the router VM of vm.sh (uplinks up1-up3, VPS at 1.2.3.4, see
// e2e.sh): configures cengarde from the web UI only (VPS addresses,
// uplinks, IP pass, enable), checks that an address with a mask and an MTU
// of 1420 are refused and that the cloud-config follows the form, applies,
// and waits on the status page for the tunnel, the three uplinks and the
// VPS confirming IP pass; then pauses an uplink there and resumes it.
// Fails on any JavaScript error after logging in.
//
// SPDX-License-Identifier: GPL-2.0-only

import { chromium } from 'playwright';
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
const shot = (name) => page.screenshot({ path: `${shots}/${name}.png`, fullPage: true });

function check(cond, what) {
	if (!cond)
		throw new Error(`check failed: ${what}`);
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

try {
	step('login');
	await page.goto(`${base}/cgi-bin/luci/`);
	await page.fill('input[name=luci_username]', 'root');
	await page.fill('input[name=luci_password]', '');
	await Promise.all([ page.waitForNavigation(), page.press('input[name=luci_password]', 'Enter') ]);
	loggedIn = true;

	step('settings page');
	await page.goto(`${base}/cgi-bin/luci/admin/services/cengarde/config`);
	await page.waitForSelector(id('widget.cbid.cengarde.main.server'));
	await shot('01-settings-empty');

	step('VPS addresses: a mask is refused, two addresses go in');
	// Typed key by key: LuCI checks the field on keyup, and Enter only adds
	// a valid entry to the list.
	const server = page.locator(id('widget.cbid.cengarde.main.server'));
	const listed = () => page.locator(`${id('cbid.cengarde.main.server')} .item input[type="hidden"]`)
		.evaluateAll((items) => items.map((e) => e.value));
	await server.pressSequentially('1.2.3.4/24');
	await server.press('Enter');
	check(await invalid(server) && (await listed()).length === 0, '1.2.3.4/24 is refused');
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

	step('VPS tab: secret and cloud-config');
	await tab('vps');
	const secret = await page.inputValue(id('widget.cbid.cengarde.main.secret'));
	check(/^[A-Za-z0-9+/]{43}=$/.test(secret), 'a secret was generated on first boot');
	let cc = await page.inputValue('#cengarde-cloud-config');
	check(cc.includes(`\n      ${secret}\n`), 'the cloud-config carries the secret');
	check(/CENGARDE_REF=[0-9a-f]{40};/.test(cc), 'the cloud-config pins the commit of the package');
	check(/PASSTHROUGH=no/.test(cc), 'no passthrough while IP pass is off');
	await shot('03-settings-vps');

	step('Tunnel tab: an MTU of 1420 is refused, IP pass on, the cloud-config follows');
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
	await shot('04-settings-tunnel');
	await tab('vps');
	cc = await page.inputValue('#cengarde-cloud-config');
	check(/PASSTHROUGH=yes/.test(cc), 'the cloud-config follows the IP pass switch before saving');

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
	check(connected, 'the WireGuard tunnel is connected');
	check(rows === UPLINKS.length, `all ${UPLINKS.length} uplinks active (got ${rows})`);
	check(await page.locator('.cengarde-problem').count() === 0, 'no problems reported');

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
