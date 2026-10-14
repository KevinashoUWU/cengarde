#!/usr/bin/env python3
"""The radio of each modem, every second, from a PC that reaches the CPEs by
ssh (story 012): serving cell, carrier aggregation and network info of
Quectel modems, in the line protocol of SmoothStream's cell-monitor, so that
bench/fieldrec.py --rf crosses it with cengarde-rec's recording as it does
with cell-monitor's (whose 5 s are too coarse for a queue that fills in one).

    python3 bench/radiorec.py -m cpe1:/dev/ttyUSB2:ENTEL -m cpe1:/dev/ttyUSB6:WOM \\
        -m cpe2:/dev/ttyUSB2:MOVISTAR -m cpe3:/dev/ttyUSB2:CLARO -o radio/ [-i 1] [-s SECS]

A modem is HOST:DEVICE:LEG[:CARRIER[:MODEM_ID]]: HOST an ssh destination
(an alias of ~/.ssh/config, keys loaded: BatchMode), DEVICE an AT port of
the modem that nothing else polls (two processes on one port read each
other's answers; cell-monitor and cell-watchdog keep theirs), LEG the label
of the cengarde link it carries (fieldrec matches it with the link's label
or name), CARRIER and MODEM_ID tags (LEG by default). One ssh per modem
keeps `socat - DEVICE` open; every interval it asks, read-only:

    AT+QENG="servingcell"  cell_serving_lte, cell_serving_nr (NSA or SA)
    AT+QCAINFO             cell_ca, one point per component carrier
    AT+QNWINFO             cell_nwinfo (the RAT and band changes)

and writes those points, plus cell_monitor (up=1, or up=0 while the modem
does not answer), stamped with this PC's clock when the serving cell's
answer came (keep it on NTP, as the Pi). Files in DIR:

    radio-DATE.lp          line protocol, ns timestamps: --rf radio-DATE.lp
    radio-DATE.raw.jsonl   every answer as it came ({"t_ms", "modem", "cmd",
                           "lines"}), to parse again what this parser got wrong

No AT command here changes anything in the modem. A lost ssh comes back
with backoff (2 s doubling to 30 s). SIGINT or SIGTERM stops it cleanly.

SPDX-License-Identifier: GPL-2.0-only
"""
import argparse
import json
import os
import re
import select
import signal
import subprocess
import sys
import threading
import time

CMDS = ('AT+QENG="servingcell"', "AT+QCAINFO", "AT+QNWINFO")
LTE_BW_MHZ = {6: 1.4, 15: 3.0, 25: 5.0, 50: 10.0, 75: 15.0, 100: 20.0}  # resource blocks
LTE_BW_CODE_MHZ = {0: 1.4, 1: 3.0, 2: 5.0, 3: 10.0, 4: 15.0, 5: 20.0}   # QENG's ul/dl_bandwidth
NR_BW_MHZ = {0: 5, 1: 10, 2: 15, 3: 20, 4: 25, 5: 30, 6: 40, 7: 50, 8: 60, 9: 70, 10: 80, 11: 90,
             12: 100, 13: 200, 14: 400}


def san_tag(s):
    """As cell-monitor.sh's san_tag (and fieldrec.san_tag): lower case, white
    space to _, only a-z 0-9 _ . - kept."""
    s = (s or "").replace('"', "").replace("\r", "").lower()
    return re.sub(r"[^a-z0-9_.\-]", "", re.sub(r"\s+", "_", s))


def fields_of(line):
    """The comma-separated values after "+XXX: ", quotes off."""
    body = line.split(":", 1)[1] if ":" in line else line
    return [v.strip().strip('"') for v in body.split(",")]


def num(v, base=10):
    """An int, or None for "-", "" or anything else."""
    try:
        return int(v, base)
    except (TypeError, ValueError):
        return None


def band_tag(s):
    """ "LTE BAND 7" -> ("lte", "b7"); "NR5G BAND 78" -> ("nr5g", "n78"); a bare number -> b<n>."""
    m = re.search(r"(\d+)\s*$", s or "")
    n = m.group(1) if m else ""
    if "NR" in (s or "").upper():
        return "nr5g", "n" + n
    return "lte", "b" + n


def parse_servingcell(lines):
    """AT+QENG="servingcell" -> [(measurement, tags, fields)]. The three
    shapes of Quectel's RM5xx: LTE alone on one line; EN-DC (NSA) as a
    "servingcell" line with the state, then an "LTE" and a "NR5G-NSA" line;
    SA on one line."""
    out, state = [], None
    for line in lines:
        f = fields_of(line)
        if not f:
            continue
        if f[0] == "servingcell":
            state = san_tag(f[1]) if len(f) > 1 else None
            f = f[2:]
            if not f:
                continue
        if f[0] == "LTE" and len(f) >= 17:
            # "LTE",duplex,mcc,mnc,cellid,pci,earfcn,band,ul_bw,dl_bw,tac,rsrp,rsrq,rssi,sinr,cqi,tx_power,srxlev
            g = f + ["-"] * 18
            fl = {"mcc": num(g[2]), "mnc": num(g[3]), "cellid": num(g[4], 16), "pci": num(g[5]),
                  "earfcn": num(g[6]), "ul_bw_code": num(g[8]), "dl_bw_code": num(g[9]), "tac": num(g[10], 16),
                  "rsrp": num(g[11]), "rsrq": num(g[12]), "rssi": num(g[13]), "sinr": num(g[14]),
                  "cqi": num(g[15]), "tx_power": num(g[16]), "srxlev": num(g[17])}
            out.append(("cell_serving_lte", {"state": state or "unknown", "duplex": san_tag(g[1]), "band": "b" + g[7]}, fl))
        elif f[0] == "NR5G-NSA" and len(f) >= 9:
            # "NR5G-NSA",mcc,mnc,pci,rsrp,sinr,rsrq,arfcn,band,nr_dl_bw,scs
            g = f + ["-"] * 11
            fl = {"mcc": num(g[1]), "mnc": num(g[2]), "pci": num(g[3]), "rsrp": num(g[4]), "sinr": num(g[5]),
                  "rsrq": num(g[6]), "arfcn": num(g[7]), "nr_bw_code": num(g[9]), "scs_code": num(g[10])}
            out.append(("cell_serving_nr", {"state": state or "unknown", "mode": "nsa", "band": "n" + g[8]}, fl))
        elif f[0] == "NR5G-SA" and len(f) >= 14:
            # "NR5G-SA",duplex,mcc,mnc,cellid,pci,tac,arfcn,band,nr_dl_bw,rsrp,rsrq,sinr,scs,srxlev
            g = f + ["-"] * 15
            fl = {"mcc": num(g[2]), "mnc": num(g[3]), "cellid": num(g[4], 16), "pci": num(g[5]), "tac": num(g[6], 16),
                  "arfcn": num(g[7]), "nr_bw_code": num(g[9]), "rsrp": num(g[10]), "rsrq": num(g[11]),
                  "sinr": num(g[12]), "scs_code": num(g[13]), "srxlev": num(g[14])}
            out.append(("cell_serving_nr", {"state": state or "unknown", "mode": "sa", "duplex": san_tag(g[1]),
                                            "band": "n" + g[8]}, fl))
    return out


def parse_cainfo(lines):
    """AT+QCAINFO -> one cell_ca per component carrier. LTE:
    type,earfcn,bw (resource blocks),band,state,pci,rsrp,rsrq,rssi,sinr[,...];
    NR (as an SCC of EN-DC): type,arfcn,bw code,band,pci[,...], whose later
    values are kept in "tail" until a connected answer shows their order."""
    out = []
    for seq, line in enumerate(lines, 1):
        f = fields_of(line)
        if len(f) < 4:
            continue
        rat, band = band_tag(f[3])
        bw = num(f[2])
        tags = {"rat": rat, "cc_type": san_tag(f[0]), "cc_seq": str(seq), "band": band}
        if rat == "lte":
            g = f + ["-"] * 10
            fl = {"arfcn": num(g[1]), "bw_code": bw, "bw_mhz": LTE_BW_MHZ.get(bw), "cell_state": num(g[4]),
                  "pci": num(g[5]), "rsrp": num(g[6]), "rsrq": num(g[7]), "rssi": num(g[8]), "sinr": num(g[9])}
        else:
            fl = {"arfcn": num(f[1]), "bw_code": bw, "bw_mhz": NR_BW_MHZ.get(bw),
                  "pci": num(f[4]) if len(f) > 4 else None}
            if len(f) > 5:
                fl["tail"] = ",".join(f[5:])
        out.append(("cell_ca", tags, fl))
    return out


def parse_nwinfo(lines):
    """AT+QNWINFO -> cell_nwinfo per line: rat and band as tags (san_tag),
    arfcn and plmn as fields."""
    out = []
    for line in lines:
        f = fields_of(line)
        if len(f) < 4:
            continue
        out.append(("cell_nwinfo", {"rat": san_tag(f[0]), "band": san_tag(f[2])},
                    {"arfcn": num(f[3]), "plmn": f[1]}))
    return out


def esc_tag(v):
    return str(v).replace("\\", "\\\\").replace(",", "\\,").replace("=", "\\=").replace(" ", "\\ ")


def lp_line(meas, tags, fields, t_ns):
    """One line protocol line; None fields are left out, ints get "i",
    strings quotes. None when no field is left."""
    fs = []
    for k, v in fields.items():
        if v is None:
            continue
        if isinstance(v, bool):
            fs.append(f"{k}={'true' if v else 'false'}")
        elif isinstance(v, int):
            fs.append(f"{k}={v}i")
        elif isinstance(v, float):
            fs.append(f"{k}={v}")
        else:
            fs.append(f'{k}="' + str(v).replace("\\", "\\\\").replace('"', '\\"') + '"')
    if not fs:
        return None
    t = ",".join(f"{k}={esc_tag(v)}" for k, v in tags.items() if v not in (None, ""))
    return f"{meas},{t} {','.join(fs)} {t_ns}"


PARSERS = {CMDS[0]: (parse_servingcell, "+QENG:"), CMDS[1]: (parse_cainfo, "+QCAINFO:"),
           CMDS[2]: (parse_nwinfo, "+QNWINFO:")}


def points(answers):
    """{cmd: [lines]} -> [(meas, tags, fields)] of every parser."""
    out = []
    for cmd, lines in answers.items():
        parse, prefix = PARSERS[cmd]
        out += parse([l for l in lines if l.startswith(prefix)])
    return out


class Modem:
    """One modem: its spec, its ssh+socat, and a query that reads an answer
    up to OK or ERROR."""

    def __init__(self, spec, site):
        parts = spec.split(":")
        if len(parts) < 3 or not all(parts[:3]):
            raise ValueError(f"modem '{spec}': expected HOST:DEVICE:LEG[:CARRIER[:MODEM_ID]]")
        self.host, self.dev, self.leg = parts[0], parts[1], parts[2]
        self.carrier = parts[3] if len(parts) > 3 and parts[3] else self.leg
        self.modem_id = parts[4] if len(parts) > 4 and parts[4] else self.leg
        self.name = self.leg
        self.tags = {"site": san_tag(site), "cpe_id": san_tag(self.host), "host": san_tag(self.host),
                     "modem_id": san_tag(self.modem_id), "leg": san_tag(self.leg), "carrier": san_tag(self.carrier)}
        self.proc, self.buf = None, b""

    def connect(self):
        self.close()
        self.proc = subprocess.Popen(
            ["ssh", "-o", "BatchMode=yes", "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2",
             self.host, f"exec socat - {self.dev},raw,echo=0,crnl"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
        self.buf = b""

    def close(self):
        if self.proc:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=2)
            except Exception:
                self.proc.kill()
            self.proc = None

    def query(self, cmd, timeout=3.0):
        """The lines of the answer to cmd (without echo, OK, blanks), or None
        when it did not end in OK within timeout (or the ssh died)."""
        if not self.proc or self.proc.poll() is not None:
            return None
        try:
            self.proc.stdin.write((cmd + "\r").encode())
        except (BrokenPipeError, OSError):
            return None
        lines, end = [], time.monotonic() + timeout
        fd = self.proc.stdout.fileno()
        while True:
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip()
                if not line or line == cmd or line.startswith("AT"):
                    continue
                if line == "OK":
                    return lines
                if line == "ERROR" or line.startswith("+CME ERROR"):
                    return lines + [line]
                lines.append(line)
            left = end - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([fd], [], [], left)
            if not r:
                return None
            chunk = os.read(fd, 4096)
            if not chunk:
                return None
            self.buf += chunk


class Writer:
    def __init__(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        self.lp_path = os.path.join(outdir, f"radio-{stamp}.lp")
        self.lp = open(self.lp_path, "a", encoding="utf-8")
        self.raw = open(os.path.join(outdir, f"radio-{stamp}.raw.jsonl"), "a", encoding="utf-8")
        self.lock = threading.Lock()
        self.lines = 0

    def put(self, modem, t_ns, answers, up):
        pts = points(answers) if up else []
        out = [l for l in (lp_line(m, {**modem.tags, **t}, f, t_ns) for m, t, f in pts) if l]
        out.append(lp_line("cell_monitor", modem.tags, {"up": 1 if up else 0, "lp_lines": len(out)}, t_ns))
        with self.lock:
            self.lp.write("".join(l + "\n" for l in out))
            for cmd, lines in answers.items():
                self.raw.write(json.dumps({"t_ms": t_ns // 1_000_000, "modem": modem.name, "cmd": cmd,
                                           "lines": lines}) + "\n")
            self.lp.flush()
            self.raw.flush()
            self.lines += len(out)
        return pts

    def close(self):
        self.lp.close()
        self.raw.close()


def run_modem(modem, writer, interval, stop, quiet):
    backoff, nxt = 2.0, time.monotonic()
    modem.connect()
    while not stop.is_set():
        answers, ok = {}, True
        for cmd in CMDS:
            a = modem.query(cmd)
            if a is None:
                ok = False
                break
            answers[cmd] = a
        t_ns = time.time_ns()
        if ok:
            backoff = 2.0
            pts = writer.put(modem, t_ns, answers, True)
            if not quiet:
                lte = next((f for m, t, f in pts if m == "cell_serving_lte"), {})
                nr = next((f for m, t, f in pts if m == "cell_serving_nr"), None)
                ncc = sum(1 for m, t, f in pts if m == "cell_ca")
                print(f"{time.strftime('%H:%M:%S')} {modem.name:9} rsrp {lte.get('rsrp')} sinr {lte.get('sinr')} "
                      f"cqi {lte.get('cqi')} tx {lte.get('tx_power')} cc {ncc}"
                      + (f" | nr rsrp {nr.get('rsrp')} sinr {nr.get('sinr')}" if nr else ""), file=sys.stderr)
            nxt += interval
            stop.wait(max(0.0, nxt - time.monotonic()))
            if time.monotonic() - nxt > interval:
                nxt = time.monotonic()  # fell behind (a slow answer): start the beat again
        else:
            writer.put(modem, t_ns, answers, False)
            print(f"{time.strftime('%H:%M:%S')} {modem.name}: no answer, reconnecting in {backoff:.0f} s",
                  file=sys.stderr)
            if stop.wait(backoff):
                break
            backoff = min(backoff * 2, 30.0)
            modem.connect()
            nxt = time.monotonic()
    modem.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-m", "--modem", action="append", required=True, help="HOST:DEVICE:LEG[:CARRIER[:MODEM_ID]]")
    ap.add_argument("-o", "--outdir", default="radio")
    ap.add_argument("-i", "--interval", type=float, default=1.0, help="seconds between polls of each modem (1)")
    ap.add_argument("-s", "--seconds", type=float, default=0, help="stop after this long (0: until SIGINT)")
    ap.add_argument("--site", default="banco")
    ap.add_argument("-q", "--quiet", action="store_true", help="no line per poll on stderr")
    args = ap.parse_args(argv)
    try:
        modems = [Modem(s, args.site) for s in args.modem]
    except ValueError as e:
        ap.error(str(e))
    if len({(m.host, m.dev) for m in modems}) != len(modems):
        ap.error("two modems on the same HOST:DEVICE")
    writer = Writer(args.outdir)
    stop = threading.Event()
    for s in (signal.SIGINT, signal.SIGTERM):
        signal.signal(s, lambda *_: stop.set())
    threads = [threading.Thread(target=run_modem, args=(m, writer, args.interval, stop, args.quiet), daemon=True)
               for m in modems]
    print(f"radiorec: {len(modems)} modems every {args.interval:g} s into {writer.lp_path}", file=sys.stderr)
    for t in threads:
        t.start()
    t_end = time.monotonic() + args.seconds if args.seconds else None
    while not stop.is_set():
        if t_end and time.monotonic() >= t_end:
            stop.set()
            break
        stop.wait(0.5)
    for t in threads:
        t.join(timeout=5)
    writer.close()
    print(f"radiorec: {writer.lines} lines in {writer.lp_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
