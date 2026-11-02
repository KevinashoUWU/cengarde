#!/usr/bin/env python3
"""h3perf: a real browser's QUIC (Chromium, HTTP/3 forced) or TCP (HTTP/2,
QUIC off): 15 s of download, 15 s of upload (4 loops of 2 MB POSTs), and the
time of a tiny request every 250 ms during each, as the latency under load.
Targets: google (dl.google.com, Google's QUIC sender; download only) and vps
(Caddy, quic-go, on the VPS's tunnel address; down and up). Prints one JSON
object.

    h3perf.py h3|h2|auto google|vps|cloudflare [--secs 15] [--spki HASH]
              [--map HOST=IPV4]...

h3 forces QUIC on the target, h2 turns QUIC off, auto leaves Chromium to
choose (as a user's browser: TCP first, QUIC once the site offers it, TCP
again when UDP is refused). cloudflare is speed.cloudflare.com, TCP only
(it offers no HTTP/3): down and up.
"""
import argparse
import json
import sys

from playwright.sync_api import sync_playwright

T = {
    "google": dict(origin="dl.google.com:443", page="https://dl.google.com/cengarde-h3perf",
                   down="/linux/direct/google-chrome-stable_current_amd64.deb", up=None, ping="/robots.txt"),
    "cloudflare": dict(origin="speed.cloudflare.com:443", page="https://speed.cloudflare.com/cdn-cgi/trace",
                       down="/__down?bytes=2000000000", up="/__up", ping="/cdn-cgi/trace"),
    "vps": dict(origin="10.79.0.1:5210", page="https://10.79.0.1:5210/index.html",
                down="/big.bin", up="/up", ping="/index.html"),
}

JS = r"""
async ([secs, down, up, ping]) => {
  const pct = (a, p) => { if (!a.length) return null; const s = [...a].sort((x, y) => x - y);
    return s[Math.min(s.length - 1, Math.floor(p * s.length))]; };
  const pinger = (stop) => { const t = []; const loop = async () => {
      while (!stop.v) { const t0 = performance.now();
        try { await (await fetch(ping + '?p=' + Math.random(), {cache: 'no-store'})).arrayBuffer(); t.push(performance.now() - t0); } catch (e) {}
        await new Promise(r => setTimeout(r, 250)); } };
    return {t, done: loop()}; };
  const out = {};
  // idle latency
  { const stop = {v: false}; const p = pinger(stop); await new Promise(r => setTimeout(r, 3000)); stop.v = true; await p.done;
    out.idle_ms = {p50: pct(p.t, 0.5), p99: pct(p.t, 0.99), n: p.t.length}; }
  // download
  { const stop = {v: false}; const p = pinger(stop); const ac = new AbortController();
    let bytes = 0; const t0 = performance.now(); let t1 = t0;
    setTimeout(() => ac.abort(), secs * 1000);
    try { const r = await fetch(down, {cache: 'no-store', signal: ac.signal});
      const rd = r.body.getReader(); for (;;) { const {done, value} = await rd.read(); if (done) break; bytes += value.length; t1 = performance.now(); }
    } catch (e) {}
    stop.v = true; await p.done;
    out.down = {mbps: bytes * 8 / ((t1 - t0) / 1000) / 1e6, bytes, loaded_ms: {p50: pct(p.t, 0.5), p99: pct(p.t, 0.99), n: p.t.length}}; }
  await new Promise(r => setTimeout(r, 2000));
  // upload: 4 loops of 2 MB POSTs
  if (up) { const stop = {v: false}; const p = pinger(stop); const body = new Uint8Array(2000000);
    for (let i = 0; i < body.length; i += 65536) crypto.getRandomValues(body.subarray(i, Math.min(i + 65536, body.length)));
    let bytes = 0; const t0 = performance.now(); const end = t0 + secs * 1000;
    const loop = async () => { while (performance.now() < end) { try { const r = await fetch(up, {method: 'POST', body, cache: 'no-store'}); await r.arrayBuffer(); if (performance.now() <= end + 5000) bytes += body.length; } catch (e) { await new Promise(r => setTimeout(r, 200)); } } };
    await Promise.all([loop(), loop(), loop(), loop()]);
    const t1 = performance.now(); stop.v = true; await p.done;
    out.up = {mbps: bytes * 8 / ((t1 - t0) / 1000) / 1e6, bytes, loaded_ms: {p50: pct(p.t, 0.5), p99: pct(p.t, 0.99), n: p.t.length}}; }
  out.protocols = [...new Set(performance.getEntriesByType('resource').map(e => e.nextHopProtocol))];
  return out;
}
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("proto", choices=["h3", "h2", "auto"])
    ap.add_argument("target", choices=list(T))
    ap.add_argument("--secs", type=int, default=15)
    ap.add_argument("--spki", default="")
    ap.add_argument("--map", action="append", default=[])
    a = ap.parse_args()
    t = T[a.target]
    args = ["--no-sandbox"]
    if a.spki:
        args.append(f"--ignore-certificate-errors-spki-list={a.spki}")
    if a.map:  # IPv4 only: the testnet's tunnel carries no IPv6
        args.append("--host-resolver-rules=" + ", ".join("MAP %s %s" % tuple(m.split("=", 1)) for m in a.map))
    if a.proto == "h3":
        args += ["--enable-quic", f"--origin-to-force-quic-on={t['origin']}"]
    elif a.proto == "h2":
        args += ["--disable-quic"]
    with sync_playwright() as p:
        b = p.chromium.launch(executable_path="/usr/bin/chromium", args=args, headless=True)
        pg = b.new_page()
        pg.goto(t["page"], timeout=20000)
        r = pg.evaluate(JS, [a.secs, t["down"], t["up"], t["ping"]])
        r["proto_req"] = a.proto
        r["target"] = a.target
        r["browser"] = b.version
        b.close()
    json.dump(r, sys.stdout)
    print()


if __name__ == "__main__":
    main()
