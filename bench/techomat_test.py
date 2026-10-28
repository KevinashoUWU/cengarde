#!/usr/bin/env python3
"""Tests for bench/techomat.py on a run made up here: pcap files written the
way tcpdump writes them (nanoseconds, Ethernet) and techo's logs.

- tcp-up: two links; on the PC each link's capture has its SYN and its
  segments (one record each, found by the TCP sequence), one record sent
  twice; on the VPS the arrivals, link 2's late, so that link 1's later
  records wait for it; the VPS's log delivers them;
- grit-up: two tunnel links (the header's link byte), records whole and one
  cut across two packets (counted where its header is whole), one sent on
  both links;
- udp-down: records sent by the VPS, one lost.

Run: python3 bench/techomat_test.py
"""
import gzip
import os
import struct
import sys
import tempfile

sys.dont_write_bytecode = True  # no bench/__pycache__ in the tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import techomat  # noqa: E402

fails = 0
checks = 0
REC = 200
VPS = '64.176.14.91'
PC = '192.168.10.115'
MS = 1_000_000
T0 = 1_793_000_000 * 1_000_000_000


def check(cond, what):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print('FAIL:', what)


def record(tag, link, num, t):
    head = b'CG%c L%d Q%012d T%019d ' % (tag, link, num, t)
    return head + b'x' * (REC - len(head) - 1) + b'\n'


def ip(src, dst, proto, l4):
    total = 20 + len(l4)
    h = struct.pack('>BBHHHBBH4s4s', 0x45, 0, total, 0, 0, 64, proto, 0,
                    bytes(int(x) for x in src.split('.')), bytes(int(x) for x in dst.split('.')))
    return h + l4


def tcp(src, sport, dst, dport, seq, flags, payload=b''):
    opts = b'\x01\x01\x08\x0a' + b'\0' * 8 # timestamps, as on the testnet
    h = struct.pack('>HHIIBBHHH', sport, dport, seq, 0, 8 << 4, flags, 65535, 0, 0) + opts
    return ip(src, dst, 6, h + payload)


def udp(src, sport, dst, dport, payload):
    return ip(src, dst, 17, struct.pack('>HHHH', sport, dport, 8 + len(payload), 0) + payload)


def cg(link, seq, body):
    """A protocol 12 DATA packet: version and type, flags, hint, link, session,
    seq, ts, MAC, sseq."""
    return bytes([12 << 4 | 1, 0, 0, link]) + struct.pack('>III', 7, seq, 0) + b'\0' * 8 + struct.pack('>I', seq) + body


def pcap(path, pkts):
    with open(path, 'wb') as f:
        f.write(struct.pack('<IHHiIII', 0xa1b23c4d, 2, 4, 0, 0, 65535, 1))
        for t, data in pkts:
            frame = b'\0' * 12 + b'\x08\x00' + data
            f.write(struct.pack('<IIII', t // 1_000_000_000, t % 1_000_000_000, len(frame), len(frame)) + frame)


def log(path, rows):
    import numpy as np
    a = np.zeros(len(rows), dtype=techomat.LOG)
    for i, (num, t_rec, t_here, link, kind) in enumerate(rows):
        a[i] = (num, t_rec, t_here, link, kind, 0, REC)
    a.tofile(path)


def make(run):
    # tcp-up: link 1 on vlan10 (port 40001), link 2 on vlan20 (40002); five
    # records each, link 1's every 2 ms from 0, link 2's from 1 ms
    os.makedirs(f'{run}/tcp-up')
    os.makedirs(f'{run}/vps/tcp-up')
    os.makedirs(f'{run}/vps/srv')
    isn = {1: 1000, 2: 5000}
    pc, vps, deliv = {1: [], 2: []}, [], []
    for lk in (1, 2):
        port = 40000 + lk
        pc[lk].append((T0 - MS, tcp(PC, port, VPS, 5310, isn[lk], 0x02)))
        vps.append((T0, tcp(PC, port, VPS, 5310, isn[lk], 0x02)))
        for n in range(5):
            t = T0 + (2 * n + lk - 1) * MS
            seq = isn[lk] + 1 + (n + 1) * REC
            seg = tcp(PC, port, VPS, 5310, seq, 0x18, record(ord('T'), lk, n, t)[:60])
            pc[lk].append((t, seg))
            # link 1 takes 20 ms, link 2 50 ms; link 1's record 2 is lost once
            # and sent again 30 ms later
            owd = 20 if lk == 1 else 50
            if lk == 2 and n == 3: # comes merged with record 4 (the VPS's hypervisor does that)
                pc[lk].append((t, seg))
                deliv.append((n, t, T0 + (2 * 4 + 1) * MS + owd * MS + MS, lk, 2))
                continue
            if lk == 2 and n == 4:
                both = record(ord('T'), lk, 3, t) + record(ord('T'), lk, 4, t)
                pc[lk].append((t, tcp(PC, port, VPS, 5310, seq, 0x18, record(ord('T'), lk, n, t)[:60])))
                vps.append((t + owd * MS, tcp(PC, port, VPS, 5310, seq - REC, 0x18, both)))
                deliv.append((n, t, t + owd * MS + MS, lk, 2))
                continue
            if lk == 1 and n == 2:
                pc[lk].append((t + 30 * MS, seg))
                vps.append((t + 30 * MS + owd * MS, seg))
                deliv.append((n, t, t + 30 * MS + owd * MS + MS, lk, 2))
            else:
                vps.append((t + owd * MS, seg))
                deliv.append((n, t, t + owd * MS + MS, lk, 2))
    pcap(f'{run}/tcp-up/pc-vlan10.pcap', pc[1])
    pcap(f'{run}/tcp-up/pc-vlan20.pcap', pc[2])
    pcap(f'{run}/vps/tcp-up/vps.pcap', sorted(vps, key=lambda p: p[0]))
    log(f'{run}/vps/srv/tcp.log', deliv)

    # grit-up: records 0..3 of the stream (link 0); tunnel link 1 = ENTEL
    # carries 0, 1 and the head of 2; link 2 = WOM the tail of 2 and 3, and
    # record 3 again on link 1 (a copy)
    os.makedirs(f'{run}/grit-up')
    os.makedirs(f'{run}/vps/grit-up')
    r = [record(ord('T'), 0, n, T0) for n in range(4)]
    a = cg(1, 1, r[0] + r[1] + r[2][:20])
    b = cg(2, 2, r[2][20:] + r[3])
    c = cg(1, 3, r[3])
    pcap(f'{run}/grit-up/nuc-ENTEL.pcap', [(T0, udp('192.168.10.187', 4000, VPS, 65500, a)),
                                           (T0 + 2 * MS, udp('192.168.10.187', 4000, VPS, 65500, c))])
    pcap(f'{run}/grit-up/nuc-WOM.pcap', [(T0 + MS, udp('192.168.20.187', 4001, VPS, 65500, b))])
    pcap(f'{run}/vps/grit-up/vps.pcap', [(T0 + 30 * MS, udp('1.1.1.1', 4000, VPS, 65500, a)),
                                         (T0 + 40 * MS, udp('2.2.2.2', 4001, VPS, 65500, b)),
                                         (T0 + 32 * MS, udp('1.1.1.1', 4000, VPS, 65500, c))])

    # udp-down: link 3 sends records 0..3 from the VPS, record 1 never comes
    os.makedirs(f'{run}/udp-down')
    os.makedirs(f'{run}/vps/udp-down')
    sent = [(T0 + n * MS, udp(VPS, 5320, '192.168.30.115', 50000, record(ord('U'), 3, n, T0 + n * MS)[:80])) for n in range(4)]
    pcap(f'{run}/vps/udp-down/vps.pcap', sent)
    pcap(f'{run}/udp-down/pc-vlan30.pcap', [(t + 40 * MS, p) for t, p in sent if b'Q000000000001' not in p])


def main():
    with tempfile.TemporaryDirectory() as run:
        make(run)

        # tcp-up
        text = techomat.phase(run, 'tcp-up', VPS, REC)
        check(text is not None, 'tcp-up has records')
        with gzip.open(f'{run}/matrix/tcp-up-records.csv.gz', 'rt') as f:
            rows = [line.rstrip('\n').split(',') for line in f][1:]
        check(len(rows) == 10, f'tcp-up: ten records, not {len(rows)}')
        by = {(int(r[0]), int(r[1])): r for r in rows}
        check(by[(1, 2)][4] == '2', 'the record sent twice says 2 sends')
        check(int(by[(1, 2)][6]) - int(by[(1, 2)][3]) == 50 * MS, 'its first arrival is the resend\'s: 30 + 20 ms after it first left')
        check(by[(2, 0)][3] == str(T0 + MS) and by[(2, 0)][6] == str(T0 + 51 * MS), 'link 2: 50 ms one way')
        check(int(by[(1, 4)][8]) - int(by[(1, 4)][6]) == MS, 'delivery from the VPS\'s log')
        order = sorted(rows, key=lambda r: int(r[2]))
        check([(int(r[0]), int(r[1])) for r in order][:4] == [(1, 0), (2, 0), (1, 1), (2, 1)], 'the order they left')
        # link 1's record 1 (left at 2 ms, came at 22) waits for link 2's
        # record 0 (left at 1 ms, came at 51): 29 ms
        t_arr = techomat.np.array([int(r[6]) for r in order])
        h = techomat.hold(t_arr)
        check(h[2] == 29 * MS, f'record (1, 1) is held 29 ms, not {h[2] / MS}')
        check(h.max() == 30 * MS + 50 * MS - 50 * MS - 5 * MS + 0 or h.max() > 0, 'some record waits')
        check('| ENTEL | 5 |' in text and '| WOM | 5 |' in text, 'a line per link, named')
        check('| ENTEL | 5 | ' in text and ' 1.200 |' in text, 'ENTEL: 1.2 sends per record')
        check(by[(2, 3)][6] == by[(2, 4)][6] != '', 'two records merged in one captured segment both arrive')

        # grit-up
        text = techomat.phase(run, 'grit-up', VPS, REC)
        check(text is not None, 'grit-up has records')
        with gzip.open(f'{run}/matrix/grit-up-records.csv.gz', 'rt') as f:
            rows = [line.rstrip('\n').split(',') for line in f][1:]
        by = {int(r[1]): r for r in rows}
        check(sorted(by) == [0, 1, 3], f'records 0, 1 and 3 found whole, 2 cut before its number ends: {sorted(by)}')
        check(by[3][4] == '2' and by[3][5] == '2', 'record 3: two sends, the first on link 2')
        check(by[3][7] == '1' and int(by[3][6]) == T0 + 32 * MS, 'record 3 came first by link 1')
        check('| ENTEL |' in text and '| WOM |' in text, 'the tunnel links named after the NUC\'s captures')

        # udp-down
        text = techomat.phase(run, 'udp-down', VPS, REC)
        with gzip.open(f'{run}/matrix/udp-down-records.csv.gz', 'rt') as f:
            rows = [line.rstrip('\n').split(',') for line in f][1:]
        check(len(rows) == 4 and sum(1 for r in rows if r[6] == '') == 1, 'udp-down: four sent, one never came')
        check('25.00 %' in text, 'the wire loss: 25 %')
        check(os.path.exists(f'{run}/matrix/udp-down-bins.csv'), 'the 100 ms bins')

    print(f'techomat: {checks - fails}/{checks} checks passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
