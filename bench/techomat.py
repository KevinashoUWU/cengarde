#!/usr/bin/env python3
"""The matrix of a ceiling capture run (bench/techo.sh, story 029): every
record of every phase with when it left, on which link, how many times, when
it first came to the other end's NIC and when TCP or GRIT delivered it, and
what a reorderer would have had to hold to give the links' records back in
the order they left.

    python3 bench/techomat.py RUN_DIR [-p PHASE ...] [--vps 64.176.14.91]

Per phase (tcp-up, tcp-down, grit-up, grit-down, udp-up, udp-down) it writes
in RUN_DIR/matrix/:

    PHASE.md            per link: records, Mbit/s, sends per record, wire
                        loss, one-way delay, TCP's head-of-line wait; for all
                        links merged in the order they left (the bonding
                        ideal): the hold a reorderer needs and the share in
                        order within each wait D
    PHASE-records.csv.gz one line per record: link, number, global order,
                        first send, sends, first arrival, arrival link,
                        delivery (ns since the epoch; empty when it never
                        came)
    PHASE-bins.csv      per link and 100 ms: Mbit/s that arrived and the
                        median one-way delay

How a record is found in a packet:
    tcp   the TCP sequence (both ends fix the MSS to the record: number =
          (seq - ISN - 1) / size - 1, the hello taking the first slot), and
          the record's own header when the packet starts with one
    udp   the record's header (one per datagram)
    grit  every "CGT L" header inside a tunnel DATA packet (GRIT cuts the
          stream its own way; one cut in two is not counted where its header
          is not whole); the link is the tunnel header's link byte

The one-way delay mixes two clocks (the PC's or the NUC's, and the VPS's),
each on chrony with the SHOA servers: the run's clock-*.txt say how far off
each one was.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import csv
import glob
import gzip
import mmap
import os
import re
import struct
import sys

import numpy as np

HDR = re.compile(rb'CG([TUH]) L(\d) Q(\d{12}) T(\d{19}) ')
MARK = b'CGT L'
CG_HDR_LEN = 28
CG_T_DATA = 1
DEADLINES_MS = (0, 5, 10, 20, 50, 100, 200, 300, 500, 1000)


# ---- pcap

def packets(path):
    """(t_ns, ip_bytes) of every IPv4 packet of a classic pcap file."""
    with open(path, 'rb') as f:
        if os.fstat(f.fileno()).st_size < 24:
            return
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    magic = m[:4]
    if magic in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1'):
        end = '<'
    elif magic in (b'\xa1\xb2\xc3\xd4', b'\xa1\xb2\x3c\x4d'):
        end = '>'
    else:
        raise ValueError(f'{path}: not a pcap file (pcapng is not read)')
    nano = magic in (b'\x4d\x3c\xb2\xa1', b'\xa1\xb2\x3c\x4d')
    dlt = struct.unpack_from(end + 'I', m, 20)[0]
    l2 = {1: 14, 113: 16, 276: 20, 101: 0, 12: 0}.get(dlt)
    if l2 is None:
        raise ValueError(f'{path}: link type {dlt}')
    rec = struct.Struct(end + 'IIII')
    off, size = 24, len(m)
    while off + 16 <= size:
        sec, frac, incl, _ = rec.unpack_from(m, off)
        off += 16
        data = m[off:off + incl]
        off += incl
        p = l2
        if dlt == 1:
            etype = data[12:14]
            while etype == b'\x81\x00' and len(data) >= p + 4: # a VLAN tag left on
                p += 4
                etype = data[p - 2:p]
            if etype != b'\x08\x00':
                continue
        if len(data) > p and data[p] >> 4 == 4:
            yield sec * 1_000_000_000 + (frac if nano else frac * 1000), data[p:]


def l4(ip):
    """proto, src, sport, dst, dport, tcp seq, tcp flags, payload."""
    ihl = (ip[0] & 15) * 4
    proto = ip[9]
    src, dst = ip[12:16], ip[16:20]
    t = ip[ihl:]
    if proto == 6 and len(t) >= 20:
        sport, dport, seq = struct.unpack_from('>HHI', t, 0)
        doff = (t[12] >> 4) * 4
        return 6, src, sport, dst, dport, seq, t[13], t[doff:]
    if proto == 17 and len(t) >= 8:
        sport, dport = struct.unpack_from('>HH', t, 0)
        return 17, src, sport, dst, dport, 0, 0, t[8:]
    return None


def ip4(s):
    return bytes(int(x) for x in s.split('.'))


# ---- the logs

LOG = np.dtype([('num', '<u8'), ('t_rec', '<i8'), ('t_here', '<i8'), ('link', 'u1'), ('kind', 'u1'),
                ('pad', '<u2'), ('bytes', '<u4')])


def read_log(path):
    return np.fromfile(path, dtype=LOG) if os.path.exists(path) else np.zeros(0, dtype=LOG)


# ---- what happened to each record

class Book:
    """Per (link, number): first send and its link, sends, first arrival and
    its link, delivery."""

    def __init__(self):
        self.sent = {}
        self.arr = {}
        self.deliv = {}
        self.pk_sent = {} # grit: (tunnel link, tunnel seq) -> (time, bytes)
        self.pk_arr = {}

    def send(self, link, num, t, via=None):
        k = (link, num)
        s = self.sent.get(k)
        if s is None:
            self.sent[k] = [t, 1, link if via is None else via]
        else:
            s[1] += 1
            if t < s[0]:
                s[0] = t
                s[2] = link if via is None else via

    def arrive(self, link, num, t, via=None):
        k = (link, num)
        a = self.arr.get(k)
        if a is None or t < a[0]:
            self.arr[k] = (t, via if via is not None else link)

    def deliver(self, link, num, t):
        k = (link, num)
        if k not in self.deliv or t < self.deliv[k]:
            self.deliv[k] = t


def tcp_records(path, book, sender, forward, link_of, rec):
    """The records of the TCP segments going the data's way (forward(src)) in
    one capture: sends at the sender's end, arrivals at the other.
    link_of(flow) gives a flow's link (None: from the records it carries)."""
    isn, flow_link = {}, {}
    for t, ip in packets(path):
        x = l4(ip)
        if not x or x[0] != 6:
            continue
        _, src, sport, dst, dport, seq, flags, pl = x
        flow = (src, sport, dst, dport)
        if flags & 0x02: # SYN: the data starts at ISN + 1
            isn[flow] = seq
            continue
        if not pl or flow not in isn or not forward(src):
            continue
        m = HDR.match(bytes(pl[:42]))
        if m:
            flow_link.setdefault(flow, int(m.group(2)))
        link = link_of(flow) if link_of else None
        if link is None:
            link = flow_link.get(flow)
        if link is None:
            continue
        rel = (seq - isn[flow] - 1) % (1 << 32)
        if rel < rec: # the hello
            continue
        num = rel // rec - 1
        if sender:
            book.send(link, num, t)
        else:
            book.arrive(link, num, t)


def udp_records(path, book, sender, forward):
    for t, ip in packets(path):
        x = l4(ip)
        if not x or x[0] != 17 or not forward(x[1]):
            continue
        m = HDR.match(bytes(x[7][:42]))
        if not m or m.group(1) != b'U':
            continue
        link, num = int(m.group(2)), int(m.group(3))
        if sender:
            book.send(link, num, t)
        else:
            book.arrive(link, num, t)


def grit_records(path, book, sender, forward, link_names, dev=None):
    """Every record header inside the tunnel's DATA packets of one capture;
    the tunnel link (the header's link byte) is what the book keys on as
    'via', and the stream is link 0."""
    for t, ip in packets(path):
        x = l4(ip)
        if not x or x[0] != 17 or not forward(x[1]):
            continue
        pl = bytes(x[7])
        if len(pl) < CG_HDR_LEN or pl[0] >> 4 < 12 or pl[0] & 15 != CG_T_DATA:
            continue
        via = pl[3]
        seq = struct.unpack_from('>I', pl, 8)[0]
        pk = book.pk_sent if sender else book.pk_arr
        if (via, seq) not in pk:
            pk[(via, seq)] = (t, len(pl))
        if dev:
            link_names.setdefault(via, {}).setdefault(dev, 0)
            link_names[via][dev] += 1
        body = pl[CG_HDR_LEN:]
        i = body.find(MARK)
        while i >= 0:
            m = HDR.match(body, i)
            if m:
                num = int(m.group(3))
                if sender:
                    book.send(0, num, t, via)
                else:
                    book.arrive(0, num, t, via)
            i = body.find(MARK, i + 1)


# ---- the matrix

def pct(a, q):
    return float(np.percentile(a, q)) if len(a) else float('nan')


def ms(ns):
    return ns / 1e6


def table(book, rec, secs):
    """Arrays per record, in the order they first left."""
    keys = sorted(book.sent, key=lambda k: book.sent[k][0])
    n = len(keys)
    link = np.array([k[0] for k in keys], dtype=np.int16)
    num = np.array([k[1] for k in keys], dtype=np.int64)
    t_send = np.array([book.sent[k][0] for k in keys], dtype=np.int64)
    sends = np.array([book.sent[k][1] for k in keys], dtype=np.int32)
    send_via = np.array([book.sent[k][2] for k in keys], dtype=np.int16)
    t_arr = np.full(n, -1, dtype=np.int64)
    via = np.full(n, -1, dtype=np.int16)
    t_del = np.full(n, -1, dtype=np.int64)
    for i, k in enumerate(keys):
        a = book.arr.get(k)
        if a:
            t_arr[i], via[i] = a
        d = book.deliv.get(k)
        if d is not None:
            t_del[i] = d
    return dict(link=link, num=num, t_send=t_send, sends=sends, send_via=send_via, t_arr=t_arr, via=via, t_del=t_del)


def hold(t_arr):
    """What a reorderer giving the records back in the order they left holds
    each one: the latest arrival of it and all before it, minus its own.
    Records that never came are left out (a reorderer gives up on them)."""
    a = t_arr[t_arr >= 0]
    return np.maximum.accumulate(a) - a


def links_by_packet(book, names):
    """GRIT's links, from the tunnel's packets (link byte and sequence): what
    each carried, what it lost and its one-way delay."""
    lines = ['', 'The tunnel\'s packets, per link', '',
             '| link | packets | Mbit/s | wire loss | OWD p50 | p95 | p99 | max |', '| --- | --- | --- | --- | --- | --- | --- | --- |']
    for via in sorted({k[0] for k in book.pk_sent}):
        ks = [k for k in book.pk_sent if k[0] == via]
        ts = np.array([book.pk_sent[k][0] for k in ks])
        span = (ts.max() - ts.min()) / 1e9 or 1
        nbytes = sum(book.pk_sent[k][1] for k in ks)
        owd = np.array([ms(book.pk_arr[k][0] - book.pk_sent[k][0]) for k in ks if k in book.pk_arr])
        lost = 1 - len(owd) / len(ks)
        lines.append(f'| {names.get(via, via)} | {len(ks)} | {nbytes * 8 / span / 1e6:.1f} | {100 * lost:.2f} % | {pct(owd, 50):.1f} '
                     f'| {pct(owd, 95):.1f} | {pct(owd, 99):.1f} | {owd.max() if len(owd) else float("nan"):.1f} |')
    return lines


def summary(name, tb, rec, names, book=None):
    lines = [f'# {name}', '']
    lines.append('| link | records | Mbit/s | sends/record | wire loss | OWD p50 | p95 | p99 | max | HoL p50 | p95 | p99 |')
    lines.append('| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |')
    groups = [('all', np.ones(len(tb['link']), bool))]
    key = 'send_via' if name.startswith('grit') else 'link'
    for lk in sorted(set(tb[key].tolist()) - {-1}):
        groups.append((names.get(lk, str(lk)) + (' (first sent)' if key == 'send_via' else ''), tb[key] == lk))
    for label, sel in groups:
        arr = sel & (tb['t_arr'] >= 0)
        n = int(sel.sum())
        if not n:
            continue
        span = (tb['t_send'][sel].max() - tb['t_send'][sel].min()) / 1e9 or 1
        owd = ms(tb['t_arr'][arr] - tb['t_send'][arr])
        dl = arr & (tb['t_del'] >= 0)
        hol = ms(tb['t_del'][dl] - tb['t_arr'][dl])
        lost = 1 - arr.sum() / n
        lines.append(f'| {label} | {n} | {n * rec * 8 / span / 1e6:.1f} | {tb["sends"][sel].mean():.3f} | {100 * lost:.2f} % '
                     f'| {pct(owd, 50):.1f} | {pct(owd, 95):.1f} | {pct(owd, 99):.1f} | {owd.max() if len(owd) else float("nan"):.1f} '
                     f'| {pct(hol, 50):.1f} | {pct(hol, 95):.1f} | {pct(hol, 99):.1f} |')
    if book is not None and book.pk_sent:
        lines += links_by_packet(book, names)
    h = ms(hold(tb['t_arr']))
    lines += ['', 'Every link merged in the order the records left (the bonding ideal): what a reorderer holds, ms', '',
              f'p50 {pct(h, 50):.1f}, p90 {pct(h, 90):.1f}, p99 {pct(h, 99):.1f}, p99.9 {pct(h, 99.9):.1f}, max '
              f'{h.max() if len(h) else float("nan"):.1f}', '',
              '| wait D (ms) | ' + ' | '.join(str(d) for d in DEADLINES_MS) + ' |',
              '| --- |' + ' --- |' * len(DEADLINES_MS),
              '| in order within D | ' + ' | '.join(f'{100 * (h <= d).mean():.2f} %' if len(h) else '-' for d in DEADLINES_MS) + ' |']
    # how far each record came from its place: arrival rank minus departure rank
    ok = tb['t_arr'] >= 0
    if ok.any():
        rank = np.empty(ok.sum(), dtype=np.int64)
        rank[np.argsort(tb['t_arr'][ok], kind='stable')] = np.arange(ok.sum())
        disp = np.abs(rank - np.arange(ok.sum()))
        lines += ['', f'Displacement (records between where it left and where it came): p50 {pct(disp, 50):.0f}, '
                  f'p99 {pct(disp, 99):.0f}, max {disp.max()}']
    return '\n'.join(lines) + '\n'


def write_records(path, tb):
    with gzip.open(path, 'wt', newline='') as f:
        w = csv.writer(f)
        w.writerow(['link', 'num', 'order', 't_send', 'sends', 'send_via', 't_arr', 'via', 't_del'])
        for i in range(len(tb['link'])):
            w.writerow([tb['link'][i], tb['num'][i], i, tb['t_send'][i], tb['sends'][i], tb['send_via'][i],
                        tb['t_arr'][i] if tb['t_arr'][i] >= 0 else '', tb['via'][i] if tb['via'][i] >= 0 else '',
                        tb['t_del'][i] if tb['t_del'][i] >= 0 else ''])


def write_bins(path, tb, rec, names, key):
    ok = tb['t_arr'] >= 0
    if not ok.any():
        return
    t0 = tb['t_arr'][ok].min()
    with open(path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['link', 't_ms', 'mbits', 'owd_p50_ms'])
        for lk in sorted(set(tb[key][ok].tolist())):
            sel = ok & (tb[key] == lk)
            b = (tb['t_arr'][sel] - t0) // 100_000_000
            owd = ms(tb['t_arr'][sel] - tb['t_send'][sel])
            for x in np.unique(b):
                s = b == x
                w.writerow([names.get(lk, lk), int(x) * 100, f'{s.sum() * rec * 8 / 0.1 / 1e6:.2f}',
                            f'{np.median(owd[s]):.2f}'])


def phase(run, ph, vps, rec):
    pdir = os.path.join(run, ph)
    vdir = os.path.join(run, 'vps', ph)
    kind, way = ph.split('-')
    book = Book()
    vps_ip = ip4(vps)
    names = {1: 'ENTEL', 2: 'WOM', 3: 'MOVISTAR', 4: 'CLARO'}
    # a record leaves the PC (or the NUC) going up, the VPS going down: the
    # PC's and the NUC's captures see it leave going up, the VPS's going down
    up = way == 'up'

    def forward(src):
        return (src != vps_ip) if up else (src == vps_ip)
    if kind == 'tcp':
        devs = {'vlan10': 1, 'vlan20': 2, 'vlan30': 3, 'vlan40': 4}
        for p in sorted(glob.glob(os.path.join(pdir, 'pc-vlan*.pcap'))):
            lk = devs.get(os.path.basename(p)[3:-5])
            tcp_records(p, book, up, forward, lambda flow, lk=lk: lk, rec)
        for p in glob.glob(os.path.join(vdir, 'vps.pcap')):
            tcp_records(p, book, not up, forward, None, rec)
        logs = [os.path.join(vdir, '..', 'srv', 'tcp.log')] if up else glob.glob(os.path.join(pdir, 'pc-L*.log'))
    elif kind == 'udp':
        for p in sorted(glob.glob(os.path.join(pdir, 'pc-vlan*.pcap'))):
            udp_records(p, book, up, forward)
        for p in glob.glob(os.path.join(vdir, 'vps.pcap')):
            udp_records(p, book, not up, forward)
        logs = []
    else:
        link_names = {}
        for p in sorted(glob.glob(os.path.join(pdir, 'nuc-*.pcap'))):
            grit_records(p, book, up, forward, link_names, os.path.basename(p)[4:-5])
        for p in glob.glob(os.path.join(vdir, 'vps.pcap')):
            grit_records(p, book, not up, forward, link_names)
        for via, devs in link_names.items():
            names[via] = max(devs, key=devs.get)
        logs = [os.path.join(vdir, '..', 'srv', 'tcp.log')] if up else glob.glob(os.path.join(pdir, 'pc-L*.log'))
    if not book.sent:
        return None
    lo, hi = min(v[0] for v in book.sent.values()), max(v[0] for v in book.sent.values())
    for path in logs:
        lg = read_log(path)
        lg = lg[(lg['kind'] == 2) & (lg['t_here'] >= lo) & (lg['t_here'] <= hi + 30_000_000_000)]
        for r in lg:
            book.deliver(int(r['link']), int(r['num']), int(r['t_here']))
    tb = table(book, rec, (hi - lo) / 1e9)
    out = os.path.join(run, 'matrix')
    os.makedirs(out, exist_ok=True)
    text = summary(ph, tb, rec, names, book if kind == 'grit' else None)
    with open(os.path.join(out, f'{ph}.md'), 'w') as f:
        f.write(text)
    write_records(os.path.join(out, f'{ph}-records.csv.gz'), tb)
    write_bins(os.path.join(out, f'{ph}-bins.csv'), tb, rec, names, 'via' if kind == 'grit' else 'link')
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('run')
    ap.add_argument('-p', '--phase', action='append')
    ap.add_argument('--vps', default='64.176.14.91')
    ap.add_argument('-r', '--rec', type=int, default=1300)
    a = ap.parse_args()
    phases = a.phase or sorted(os.path.basename(d) for d in glob.glob(os.path.join(a.run, '*-up')) +
                               glob.glob(os.path.join(a.run, '*-down')))
    for ph in phases:
        text = phase(a.run, ph, a.vps, a.rec)
        print(text if text else f'{ph}: no records\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
