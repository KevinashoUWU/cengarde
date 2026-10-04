#!/usr/bin/env python3
"""Measurement driver for the lab (bench/lab.sh): what the lab's report line
lacks, per run.

It wraps bench/lab.sh (setup, start, teardown) and replays its down/up
traffic, adding:
- CPU per packet in ns from /proc/PID/task/*/schedstat (user plus kernel in
  the process's context, as lab.sh's us/pkt, with ns resolution), the
  user/sys split, and the same per thread, by thread name;
- the fake WireGuard processes' own CPU, whole-system CPU, ksoftirqd and
  steal;
- per-netns UDP counters (RcvbufErrors, SndbufErrors) and per-socket drops
  from /proc/net/udp, labelled by role: client links l1.., the client's
  WireGuard socket c_wg; the server's listen sockets lfd (all of them) and
  lfd0.. (by creation order: the lanes, then the junk socket), its
  per-session WireGuard sockets sess_wg; the generators gen;
- the engines' own counters through their control sockets, the server's
  lanes included (rx and kernel drops per lane, from SO_MEMINFO).

Every run is one JSON line in RESULTS (default ./mt.jsonl).

Commands (each run under the lab's lock, e.g.
  RUN=/tmp/cg-mt flock /tmp/cengarde-netns.lock python3 bench/mt.py ...):
  sweep LABEL DIRS RATES SECS [NLINKS SIZE REPS]
  slow LABEL DIR PPS SECS WHERE(none|cli|srv) RATE LIMIT [CLIENT_EXTRA] [SERVER_EXTRA]
  perf LABEL DIR PPS SECS SIDE OUT [NLINKS SIZE]
  multi LABEL DIRS N TOTAL_PPS_LIST SECS [REPS]
  multirates LABEL DIR RATES SECS
  s1 [REPS RATES LANES SECS NLINKS]   gate S1 of the threading design: upload,
       server lanes interleaved (default 1 and 8), default server buffers,
       the router's at 32 MiB so that only the server can drop
  table FILE...                       the S1 table from such runs

Environment: RUN (default /tmp/cg-mt), RESULTS, CENGARDE_BIN, PERF (perf
binary), CX and SX (extra client and server settings, "k = v;k = v"), PIN
(CPU pinning, "srv=1,cli=2,gtx=0,grx=3").
"""
import glob
import json
import os
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LAB = HERE
BIN = LAB + '/bin'
RUN = os.environ.get('RUN', '/tmp/cg-mt')
CG = os.environ.get('CENGARDE_BIN', BIN + '/cengarde')
HZ = os.sysconf('SC_CLK_TCK')
PERF = os.environ.get('PERF', 'perf')
RESULTS = os.environ.get('RESULTS', 'mt.jsonl')


def lab(fn, env=None):
    e = dict(os.environ)
    e['RUN'] = RUN
    e.update(env or {})
    r = subprocess.run([LAB + '/lab.sh'] + fn.split(), env=e, capture_output=True, text=True)
    return r.stdout + r.stderr


def nsx(ns, *cmd):
    return subprocess.run(['ip', 'netns', 'exec', ns] + list(cmd), capture_output=True, text=True).stdout


def pid_of(side):
    try:
        return int(open(f'{RUN}/{side}.pid').read())
    except OSError:
        return None


def proc_ticks(pid):
    try:
        f = open(f'/proc/{pid}/stat').read().rsplit(')', 1)[1].split()
        return int(f[11]), int(f[12])
    except OSError:
        return 0, 0


def proc_ns(pid):
    t = 0
    for p in glob.glob(f'/proc/{pid}/task/*/schedstat'):
        try:
            t += int(open(p).read().split()[0])
        except OSError:
            pass
    return t


def thread_ns(pid):
    """CPU ns per thread, by thread name (threads of one name summed)."""
    out = {}
    for d in glob.glob(f'/proc/{pid}/task/*'):
        try:
            name = open(d + '/comm').read().strip()
            out[name] = out.get(name, 0) + int(open(d + '/schedstat').read().split()[0])
        except OSError:
            pass
    return out


def sysstat():
    v = [int(x) for x in open('/proc/stat').readline().split()[1:]]
    # user nice system idle iowait irq softirq steal
    idle = v[3] + v[4]
    return sum(v[:8]), idle, v[6], v[7]


KSOFT = None


def ksoftirqd_ticks():
    global KSOFT
    if KSOFT is None:
        KSOFT = []
        for p in glob.glob('/proc/[0-9]*/comm'):
            try:
                if open(p).read().startswith('ksoftirqd'):
                    KSOFT.append(int(p.split('/')[2]))
            except OSError:
                pass
    return sum(sum(proc_ticks(p)) for p in KSOFT)


def snmp(ns):
    lines = [l.split() for l in nsx(ns, 'cat', '/proc/net/snmp').splitlines() if l.startswith('Udp:')]
    if len(lines) < 2:
        return {}
    return {k: int(v) for k, v in zip(lines[0][1:], lines[1][1:])}


def hexaddr(h):
    ip, port = h.split(':')
    b = bytes.fromhex(ip)[::-1]
    return '.'.join(str(x) for x in b) + ':' + str(int(port, 16))


def udp_drops(ns):
    """Per-socket drops from /proc/net/udp, labelled by role."""
    out = {}
    for l in nsx(ns, 'cat', '/proc/net/udp').splitlines()[1:]:
        f = l.split()
        if len(f) < 13:
            continue
        loc, rem, drops = hexaddr(f[1]), hexaddr(f[2]), int(f[12])
        if ns == 'cli':
            if loc.startswith('10.0.'):
                k = 'l' + loc.split('.')[2]
            elif loc.endswith(':50000') or loc.startswith('127.0.0.1:5000'):
                k = 'gen'
            elif loc.startswith('127.0.0.1:594'):
                k = 'c_wg'
            else:
                k = 'other'
        else:
            if loc.endswith(':59402'):
                k = 'lfd@' + f[9]
            elif rem == '127.0.0.1:59301':
                k = 'sess_wg'
            elif loc.endswith(':59301'):
                k = 'gen'
            else:
                k = 'other'
        out[k] = out.get(k, 0) + drops
    lanes = sorted((int(k[4:]), k) for k in out if k.startswith('lfd@'))
    for i, (_, k) in enumerate(lanes):
        out['lfd' + str(i)] = out.pop(k)
    if lanes:
        out['lfd'] = sum(out['lfd' + str(i)] for i in range(len(lanes)))
    return out


def softnet():
    proc = drop = 0
    for l in open('/proc/net/softnet_stat'):
        f = l.split()
        proc += int(f[0], 16)
        drop += int(f[1], 16)
    return proc, drop


def ctl_status(side):
    sock = f'{RUN}/{side}.sock'
    if not os.path.exists(sock):
        return None
    out = subprocess.run([CG, 'ctl', '-s', sock, 'status'], capture_output=True, text=True).stdout
    try:
        return json.loads(out)
    except ValueError:
        return None


def engine_counters(side, st):
    """Drops the engine counts, summed."""
    if not st:
        return {}
    if side == 'client':
        L = st['links']
        return {'c_tx_drops': sum(l['tx_drops'] for l in L), 'c_tx_errors': sum(l['tx_errors'] for l in L),
                'c_wg_drops': st['download']['wireguard_drops'], 'c_too_old': st['download']['too_old'],
                'c_mutes': sum(l['upload_mutes'] for l in L),
                'c_link_tx': [l['tx_packets'] for l in L], 'c_link_drops': [l['tx_drops'] for l in L],
                'c_rx_first': [l['rx_first'] for l in L], 'c_rx_dup': [l['rx_duplicate'] for l in L],
                'g_c_rx_lag_ms': [l['rx_lag_ms'] for l in L], 'g_c_upload': [l['upload'] for l in L],
                'g_c_rtt_ms': [l['rtt_ms'] for l in L],
                'g_c_srv_view_lag_ms': [l.get('server_view', {}).get('lag_ms') for l in L]}
    S = st.get('sessions', [])
    return {'s_tx_drops': sum(p['tx_drops'] for s in S for p in s['links']),
            's_wg_drops': sum(s['wireguard_drops'] for s in S),
            's_mutes': sum(p['download_mutes'] for s in S for p in s['links']),
            's_link_drops': [p['tx_drops'] for s in S for p in s['links']],
            's_link_tx': [p['tx_packets'] for s in S for p in s['links']],
            's_rx_first': [p['rx_first'] for s in S for p in s['links']],
            'g_s_rx_lag_ms': [p['rx_lag_ms'] for s in S for p in s['links']],
            'g_s_download': [p['download'] for s in S for p in s['links']],
            's_too_old': st['rx']['too_old'],
            's_lane_rx': [l['rx'] for l in st.get('lanes', [])],
            's_lane_drops': [l['drops'] or 0 for l in st.get('lanes', [])],
            's_junk_rx': (st.get('junk') or {}).get('rx', 0),
            'g_s_lanes': len(st.get('lanes', [])),
            'g_s_rcvbuf': st.get('rcvbuf')}


def snap():
    s = {'t': time.monotonic(), 'sys': sysstat(), 'ksoft': ksoftirqd_ticks(), 'softnet': softnet(),
         'snmp_cli': snmp('cli'), 'snmp_srv': snmp('srv'), 'drops_cli': udp_drops('cli'),
         'drops_srv': udp_drops('srv')}
    for side in ('client', 'server'):
        p = pid_of(side)
        s[side] = (proc_ticks(p), proc_ns(p)) if p else ((0, 0), 0)
        s[side + '_threads'] = thread_ns(p) if p else {}
        s[side + '_st'] = engine_counters(side, ctl_status(side))
    return s


def diff_counters(a, b):
    out = {}
    for k, v in b.items():
        if k.startswith('g_'):
            out[k] = v
        elif isinstance(v, list):
            av = a.get(k, [0] * len(v))
            out[k] = [x - (av[i] if i < len(av) else 0) for i, x in enumerate(v)]
        else:
            out[k] = v - a.get(k, 0)
    return out


def kv(line):
    d = {}
    for tok in line.split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            try:
                d[k] = int(v)
            except ValueError:
                d[k] = v
    return d


PIN = os.environ.get('PIN', '')  # e.g. "srv=1,cli=2,gtx=0,grx=3"
PINS = dict(x.split('=') for x in PIN.split(',')) if PIN else {}


def pinned(cmd, role):
    # cmd = ['ip','netns','exec',NS, prog, ...] -> insert taskset after the netns
    if role in PINS:
        return cmd[:4] + ['taskset', '-c', PINS[role]] + cmd[4:]
    return cmd


def run_child(cmd, role=None):
    return subprocess.Popen(pinned(cmd, role) if role else cmd, stdout=subprocess.PIPE, text=True)


def reap(p):
    out = p.stdout.read()
    _, status, ru = os.wait4(p.pid, 0)
    p.returncode = status
    return out, ru.ru_utime + ru.ru_stime


def traffic(direction, pps, secs, size, label='', perf=None, extra=None):
    """One down/up run like bench/lab.sh, with the extra counters."""
    if direction == 'down':
        rx = run_child(['ip', 'netns', 'exec', 'cli', BIN + '/udpgen', '-b', '127.0.0.1:50000', '-p',
                        '127.0.0.1:59401', '-r', '20', '-s', '64', '-d', str(secs + 2), '-g', '2'], 'grx')
        time.sleep(1)
        txcmd = ['ip', 'netns', 'exec', 'srv', BIN + '/udpgen', '-b', '127.0.0.1:59301', '-l', '-r', str(pps),
                 '-s', str(size), '-d', str(secs), '-g', '1']
    else:
        rx = run_child(['ip', 'netns', 'exec', 'srv', BIN + '/udpgen', '-b', '127.0.0.1:59301', '-d', str(secs),
                        '-g', '3'], 'grx')
        time.sleep(0.3)
        txcmd = ['ip', 'netns', 'exec', 'cli', BIN + '/udpgen', '-b', '127.0.0.1:50000', '-p', '127.0.0.1:59401',
                 '-r', str(pps), '-s', str(size), '-d', str(secs), '-g', '1']
    perfp = None
    if perf:
        side, path = perf
        perfp = subprocess.Popen([PERF, 'record', '-q', '-e', 'cpu-clock', '-F', '4999', '-g', '-p',
                                  str(pid_of(side)), '-o', path], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
        time.sleep(0.5)
    s0 = snap()
    tx = run_child(txcmd, 'gtx')
    txo, txcpu = reap(tx)
    s1 = snap()
    if perfp:
        perfp.send_signal(signal.SIGINT)
        perfp.wait()
    rxo, rxcpu = reap(rx)
    t = kv(txo)
    r = kv(rxo)
    sent = t.get('sent', 0)
    uniq = r.get('uniq', 0)
    wall = s1['t'] - s0['t']
    tot = s1['sys'][0] - s0['sys'][0]
    idle = s1['sys'][1] - s0['sys'][1]
    rec = {'label': label, 'dir': direction, 'pps': pps, 'secs': secs, 'size': size, 'sent': sent,
           'send_err': t.get('send_err', 0), 'uniq': uniq, 'dup': r.get('dup', 0), 'rx': r.get('rx', 0),
           'loss_pct': 100.0 * (sent - uniq) / sent if sent else 0.0,
           'p50': r.get('p50_us'), 'p99': r.get('p99_us'), 'p999': r.get('p999_us'),
           'over200ms': r.get('over200ms'), 'wall': wall,
           'sys_busy_cpus': (tot - idle) / tot * os.cpu_count() if tot else 0,
           'softirq_cpus': (s1['sys'][2] - s0['sys'][2]) / tot * os.cpu_count() if tot else 0,
           'ksoftirqd_ticks': s1['ksoft'] - s0['ksoft'], 'steal_ticks': s1['sys'][3] - s0['sys'][3],
           'softnet_drops': s1['softnet'][1] - s0['softnet'][1],
           'gen_tx_us_pkt': 1e6 * txcpu / sent if sent else 0,
           'gen_rx_us_pkt': 1e6 * rxcpu / sent if sent else 0}
    for side in ('client', 'server'):
        (u0, k0), n0 = s0[side]
        (u1, k1), n1 = s1[side]
        rec[side + '_user_us'] = 1e6 * (u1 - u0) / HZ / sent if sent else 0
        rec[side + '_sys_us'] = 1e6 * (k1 - k0) / HZ / sent if sent else 0
        rec[side + '_us'] = (n1 - n0) / 1000 / sent if sent else 0
        rec[side + '_cpu_pct'] = 100 * (n1 - n0) / 1e9 / secs
        t0, t1 = s0[side + '_threads'], s1[side + '_threads']
        rec[side + '_threads_us'] = {k: (t1[k] - t0.get(k, 0)) / 1000 / sent if sent else 0 for k in t1}
        rec.update(diff_counters(s0[side + '_st'], s1[side + '_st']))
    for ns in ('cli', 'srv'):
        a, b = s0['snmp_' + ns], s1['snmp_' + ns]
        for k in ('RcvbufErrors', 'SndbufErrors', 'InErrors', 'InDatagrams', 'OutDatagrams'):
            rec[f'{ns}_{k}'] = b.get(k, 0) - a.get(k, 0)
    for ns in ('cli', 'srv'):
        a, b = s0['drops_' + ns], s1['drops_' + ns]
        rec['sockdrops_' + ns] = {k: b[k] - a.get(k, 0) for k in b if b[k] - a.get(k, 0)}
    if extra:
        rec.update(extra)
    print(fmt(rec), flush=True)
    with open(RESULTS, 'a') as f:
        f.write(json.dumps(rec) + '\n')
    return rec


def fmt(r):
    return (f"{r['label']:<14} {r['dir']:<4} pps={r['pps']:<6} size={r['size']:<4} sent={r['sent']:<7} "
            f"loss={r['loss_pct']:6.3f}% p50={r['p50']}us p99={r['p99']}us p99.9={r['p999']}us | "
            f"us/pkt cli={r['client_us']:5.2f} (u{r['client_user_us']:.1f}/s{r['client_sys_us']:.1f}) "
            f"srv={r['server_us']:5.2f} (u{r['server_user_us']:.1f}/s{r['server_sys_us']:.1f}) "
            f"cpu cli={r['client_cpu_pct']:.0f}% srv={r['server_cpu_pct']:.0f}% "
            f"sys={r['sys_busy_cpus']:.2f}cpu ksoft={r['ksoftirqd_ticks']} | "
            f"rcvbuf cli={r['cli_RcvbufErrors']} srv={r['srv_RcvbufErrors']} "
            f"sndbuf cli={r['cli_SndbufErrors']} srv={r['srv_SndbufErrors']} softnet={r['softnet_drops']} "
            f"eng: c_txdrop={r.get('c_tx_drops')} c_wgdrop={r.get('c_wg_drops')} s_txdrop={r.get('s_tx_drops')} "
            f"s_wgdrop={r.get('s_wg_drops')} mutes={r.get('c_mutes')}/{r.get('s_mutes')} "
            f"gen tx={r['gen_tx_us_pkt']:.1f} rx={r['gen_rx_us_pkt']:.1f} senderr={r['send_err']} "
            f"sockdrops cli={r['sockdrops_cli']} srv={r['sockdrops_srv']}")


def start(nlinks=3, size=1400, client_extra='', server_extra=''):
    if os.environ.get('CX'):
        client_extra = os.environ['CX'] + (';' + client_extra if client_extra else '')
    if os.environ.get('SX'):
        server_extra = os.environ['SX'] + (';' + server_extra if server_extra else '')
    if 'cli' in PINS:
        client_extra = f"cpu = {PINS['cli']}" + (';' + client_extra if client_extra else '')
    if 'srv' in PINS:
        server_extra = f"cpu = {PINS['srv']}" + (';' + server_extra if server_extra else '')
    env = {'NLINKS': str(nlinks), 'SIZE': str(size),
           'CLIENT_EXTRA': f'control_socket = {RUN}/client.sock' + (';' + client_extra if client_extra else ''),
           'SERVER_EXTRA': f'control_socket = {RUN}/server.sock' + (';' + server_extra if server_extra else '')}
    out = lab('setup', env) + lab('start', env)
    if out.strip():
        print(out)
    time.sleep(0.5)


def teardown():
    lab('teardown')


def cmd_sweep(argv):
    """sweep LABEL DIRS RATES SECS [NLINKS SIZE REPS]"""
    label, dirs, rates, secs = argv[0], argv[1].split(','), [int(x) for x in argv[2].split(',')], int(argv[3])
    nlinks = int(argv[4]) if len(argv) > 4 else 3
    size = int(argv[5]) if len(argv) > 5 else 1400
    reps = int(argv[6]) if len(argv) > 6 else 1
    try:
        for rep in range(reps):
            start(nlinks, size)
            for d in dirs:
                for pps in rates:
                    traffic(d, pps, secs, size, label=f'{label}#{rep}')
            teardown()
    finally:
        teardown()


def cmd_slow(argv):
    """slow LABEL DIR PPS SECS WHERE(none|cli|srv) RATE LIMIT [CLIENT_EXTRA] [SERVER_EXTRA]"""
    label, d, pps, secs, where, rate, limit = argv[0], argv[1], int(argv[2]), int(argv[3]), argv[4], argv[5], argv[6]
    cx = argv[7] if len(argv) > 7 else ''
    sx = argv[8] if len(argv) > 8 else ''
    try:
        start(3, 1400, cx, sx)
        if where != 'none':
            ns, dev = ('cli', 'l3') if where == 'cli' else ('srv', 's3')
            lim = ['latency', limit.split(':', 1)[1]] if limit.startswith('latency:') else ['limit', limit]
            subprocess.run(['ip', 'netns', 'exec', ns, 'tc', 'qdisc', 'replace', 'dev', dev, 'root', 'tbf', 'rate', rate,
                            'burst', '32kb'] + lim, check=True)
        r = traffic(d, pps, secs, 1400, label=label)
        for k in ('c_link_tx', 'c_link_drops', 'c_rx_first', 'g_c_rx_lag_ms', 'g_c_upload', 'g_c_rtt_ms',
                  'g_c_srv_view_lag_ms', 's_link_tx', 's_link_drops', 's_rx_first', 'g_s_rx_lag_ms', 'g_s_download'):
            print(f'    {k}={r.get(k)}')
        for f in ('client', 'server'):
            out = subprocess.run(['grep', '-h', 'mute', f'{RUN}/{f}.log'], capture_output=True, text=True).stdout
            for l in out.splitlines()[:6]:
                print('    ' + f + ': ' + l)
    finally:
        teardown()


def cmd_perf(argv):
    """perf LABEL DIR PPS SECS SIDE OUT [NLINKS SIZE]"""
    label, d, pps, secs, side, out = argv[0], argv[1], int(argv[2]), int(argv[3]), argv[4], argv[5]
    nlinks = int(argv[6]) if len(argv) > 6 else 3
    size = int(argv[7]) if len(argv) > 7 else 1400
    try:
        start(nlinks, size)
        traffic(d, pps, secs, size, label=label, perf=(side, out))
    finally:
        teardown()


# ---- several sessions on one server ----

def start_multi(n, server_extra='', client_extra=''):
    env = {'NLINKS': '3', 'SERVER_EXTRA': f'control_socket = {RUN}/server.sock' + (';' + server_extra if server_extra else '')}
    lab('setup', env)
    base = open(f'{RUN}/client.conf').read()
    srv = subprocess.Popen(f'exec ip netns exec srv {CG} -c {RUN}/server.conf >{RUN}/server.log 2>&1', shell=True)
    open(f'{RUN}/server.pid', 'w').write(str(srv.pid))
    pids = []
    for k in range(n):
        conf = base.replace('listen = 127.0.0.1:59401', f'listen = 127.0.0.1:{59401 + k}')
        conf = conf.replace(f'status_file = {RUN}/client.json', f'status_file = {RUN}/client{k}.json')
        if client_extra:
            conf = conf.replace('interfaces = none', 'interfaces = none\n' + client_extra.replace(';', '\n'))
        open(f'{RUN}/client{k}.conf', 'w').write(conf)
        p = subprocess.Popen(f'exec ip netns exec cli {CG} -c {RUN}/client{k}.conf >{RUN}/client{k}.log 2>&1',
                             shell=True)
        pids.append(p.pid)
    open(f'{RUN}/clients.pids', 'w').write(' '.join(map(str, pids)))
    time.sleep(2)
    return pids


def stop_multi():
    try:
        for p in open(f'{RUN}/clients.pids').read().split():
            try:
                os.kill(int(p), signal.SIGTERM)
            except OSError:
                pass
        os.unlink(f'{RUN}/clients.pids')
    except OSError:
        pass
    subprocess.run(['pkill', '-f', BIN + '/mgen'], capture_output=True)
    lab('teardown')


def msnap(cpids):
    return {'t': time.monotonic(), 'sys': sysstat(), 'ksoft': ksoftirqd_ticks(),
            'srv': proc_ns(pid_of('server')), 'srv_t': proc_ticks(pid_of('server')),
            'cli': sum(proc_ns(p) for p in cpids), 'drops_cli': udp_drops('cli'), 'drops_srv': udp_drops('srv')}


def multi_traffic(direction, rates, secs, size, label='', cpids=(), perf=None):
    n = len(rates)
    kas, txs = [], []
    if direction == 'down':
        mg = run_child(['ip', 'netns', 'exec', 'srv', BIN + '/mgen', '-b', '127.0.0.1:59301', '-l', '-n', str(n),
                        '-R', ','.join(map(str, rates)), '-s', str(size), '-d', str(secs), '-g', '1'])
        time.sleep(0.3)
        for k in range(n):
            kas.append(run_child(['ip', 'netns', 'exec', 'cli', BIN + '/udpgen', '-b', f'127.0.0.1:{50000 + k}', '-p',
                                  f'127.0.0.1:{59401 + k}', '-r', '20', '-s', '64', '-d', str(secs + 3), '-g', '2']))
            time.sleep(0.25)
    else:
        mg = run_child(['ip', 'netns', 'exec', 'srv', BIN + '/mgen', '-b', '127.0.0.1:59301', '-d', str(secs + 1),
                        '-g', '3'])
        time.sleep(0.3)
    perfp = None
    if perf:
        perfp = subprocess.Popen([PERF, 'record', '-q', '-e', 'cpu-clock', '-F', '4999', '-g', '-p',
                                  str(pid_of('server')), '-o', perf], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
        time.sleep(0.5)
    s0 = msnap(cpids)
    if direction == 'up':
        for k in range(n):
            txs.append(run_child(['ip', 'netns', 'exec', 'cli', BIN + '/udpgen', '-b', f'127.0.0.1:{50000 + k}', '-p',
                                  f'127.0.0.1:{59401 + k}', '-r', str(rates[k]), '-s', str(size), '-d', str(secs),
                                  '-g', '1']))
        outs = [reap(t)[0] for t in txs]
        s1 = msnap(cpids)
        if perfp:
            perfp.send_signal(signal.SIGINT)
            perfp.wait()
        mo = reap(mg)[0]
        sent = [kv(o).get('sent', 0) for o in outs]
        flows = [kv(l) for l in mo.splitlines() if l.startswith('flow') and not l.startswith('flows=')]
        tot = kv([l for l in mo.splitlines() if l.startswith('flows=')][0])
        # match flows to senders by count (heavy vs light), largest to largest
        fl = sorted([f for f in flows], key=lambda f: -f.get('uniq', 0))
        order = sorted(range(n), key=lambda k: -sent[k])
        per = []
        for i, k in enumerate(order):
            f = fl[i] if i < len(fl) else {}
            per.append((k, sent[k], f.get('uniq', 0), f.get('p50_us'), f.get('p99_us'), f.get('p999_us')))
        per.sort()
    else:
        mo = reap(mg)[0]
        s1 = msnap(cpids)
        if perfp:
            perfp.send_signal(signal.SIGINT)
            perfp.wait()
        kouts = [reap(k)[0] for k in kas]
        first = kv(mo.splitlines()[0])
        sent = [first.get(f'sent{k}', 0) for k in range(n)]
        per = []
        for k in range(n):
            r = kv(kouts[k])
            per.append((k, sent[k], r.get('uniq', 0), r.get('p50_us'), r.get('p99_us'), r.get('p999_us')))
        tot = {'uniq': sum(p[2] for p in per)}
    S = sum(sent)
    U = sum(p[2] for p in per)
    t_all = s1['sys'][0] - s0['sys'][0]
    idle = s1['sys'][1] - s0['sys'][1]
    rec = {'label': label, 'dir': direction, 'n': n, 'rates': rates, 'secs': secs, 'size': size, 'sent': S, 'uniq': U,
           'loss_pct': 100.0 * (S - U) / S if S else 0,
           'server_us': (s1['srv'] - s0['srv']) / 1000 / S if S else 0,
           'server_cpu_pct': 100 * (s1['srv'] - s0['srv']) / 1e9 / secs,
           'server_user_us': 1e6 * (s1['srv_t'][0] - s0['srv_t'][0]) / HZ / S if S else 0,
           'server_sys_us': 1e6 * (s1['srv_t'][1] - s0['srv_t'][1]) / HZ / S if S else 0,
           'clients_us': (s1['cli'] - s0['cli']) / 1000 / S if S else 0,
           'clients_cpu_pct': 100 * (s1['cli'] - s0['cli']) / 1e9 / secs,
           'sys_busy_cpus': (t_all - idle) / t_all * os.cpu_count() if t_all else 0,
           'steal_ticks': s1['sys'][3] - s0['sys'][3],
           'per': [{'k': p[0], 'sent': p[1], 'uniq': p[2], 'loss_pct': 100.0 * (p[1] - p[2]) / p[1] if p[1] else 0,
                    'p50': p[3], 'p99': p[4], 'p999': p[5]} for p in per]}
    for ns in ('cli', 'srv'):
        a, b = s0['drops_' + ns], s1['drops_' + ns]
        rec['sockdrops_' + ns] = {k: b[k] - a.get(k, 0) for k in b if b[k] - a.get(k, 0)}
    print(f"{label:<12} {direction:<4} n={n:<2} rates={','.join(map(str, rates))[:40]:<40} sent={S:<7} "
          f"loss={rec['loss_pct']:6.3f}% | srv us/pkt={rec['server_us']:5.2f} (u{rec['server_user_us']:.1f}/s{rec['server_sys_us']:.1f}) "
          f"cpu={rec['server_cpu_pct']:.0f}% | clients us/pkt={rec['clients_us']:5.2f} cpu={rec['clients_cpu_pct']:.0f}% "
          f"sys={rec['sys_busy_cpus']:.2f} | drops cli={rec['sockdrops_cli']} srv={rec['sockdrops_srv']}", flush=True)
    for p in rec['per']:
        if n <= 4 or p['loss_pct'] > 0.1:
            print(f"     session {p['k']}: sent={p['sent']} loss={p['loss_pct']:.3f}% p50={p['p50']}us p99={p['p99']}us p99.9={p['p999']}us")
    with open(RESULTS, 'a') as f:
        f.write(json.dumps(rec) + '\n')
    return rec


def cmd_multi(argv):
    """multi LABEL DIRS N TOTAL_PPS_LIST SECS [REPS] : sessions share the total evenly"""
    label, dirs, n, totals, secs = argv[0], argv[1].split(','), int(argv[2]), [int(x) for x in argv[3].split(',')], int(argv[4])
    reps = int(argv[5]) if len(argv) > 5 else 1
    try:
        for rep in range(reps):
            cpids = start_multi(n)
            for d in dirs:
                for tot in totals:
                    multi_traffic(d, [tot // n] * n, secs, 1400, label=f'{label}#{rep}', cpids=cpids)
            stop_multi()
    finally:
        stop_multi()


def cmd_multirates(argv):
    """multirates LABEL DIR RATES(r0,r1,...) SECS : one run with given per-session rates"""
    label, d, rates, secs = argv[0], argv[1], [int(x) for x in argv[2].split(',')], int(argv[3])
    try:
        cpids = start_multi(len(rates))
        multi_traffic(d, rates, secs, 1400, label=label, cpids=cpids)
    finally:
        stop_multi()


# ---- gate S1: server lanes against one listen socket ----

def cmd_s1(argv):
    """s1 [REPS RATES LANES SECS NLINKS DIRS]: upload by default, the lane
    counts interleaved run by run, the router's buffers at 32 MiB."""
    reps = int(argv[0]) if len(argv) > 0 else 4
    rates = [int(x) for x in (argv[1] if len(argv) > 1 else '80000,90000,100000,110000').split(',')]
    lanes = [int(x) for x in (argv[2] if len(argv) > 2 else '1,8').split(',')]
    secs = int(argv[3]) if len(argv) > 3 else 5
    nlinks = int(argv[4]) if len(argv) > 4 else 3
    dirs = (argv[5] if len(argv) > 5 else 'up').split(',')
    try:
        for rep in range(reps):
            for L in lanes:
                start(nlinks, 1400, 'rcvbuf = 33554432', f'lanes = {L}')
                for d in dirs:
                    for pps in rates:
                        traffic(d, pps, secs, 1400, label=f's1-L{L}-n{nlinks}-r{rep}')
                teardown()
    finally:
        teardown()


def whole_lost(r):
    """Packets lost with every copy at the server's listen sockets
    (upload), estimated as the loss nothing else explains: what the
    client dropped before duplication and what the server dropped after
    dedup are taken out. 0 when no listen socket dropped anything."""
    lanes = sum(r.get('s_lane_drops') or []) or r.get('sockdrops_srv', {}).get('lfd', 0)
    if r['dir'] != 'up' or not lanes:
        return 0
    cli, srv = r.get('sockdrops_cli', {}), r.get('sockdrops_srv', {})
    before = cli.get('c_wg', 0) + cli.get('gen', 0)
    after = srv.get('gen', 0) + srv.get('sess_wg', 0) + (r.get('s_wg_drops') or 0)
    return max(0, r['sent'] - r['uniq'] - before - after)


def cmd_table(argv):
    """table FILE...: S1 rows by direction, lanes and rate."""
    rows = {}
    for f in argv:
        for line in open(f):
            r = json.loads(line)
            if not r.get('label', '').startswith('s1-'):
                continue
            L = int(r['label'].split('-')[1][1:])
            n = r['label'].split('-')[2]
            rows.setdefault((r['dir'], n, r['pps'], L), []).append(r)

    def rng(v, f='%.2f'):
        return (f % min(v)) + ('–' + f % max(v) if max(v) != min(v) else '') if v else '-'

    def mean(v):
        return sum(v) / len(v) if v else 0

    print('| dir | links | kpps | lanes | runs | loss % | runs ≤ 0.1 % | server µs/pkt (mean) | client µs/pkt (mean) '
          '| server CPU % | p50 µs | copies dropped at the listen sockets | whole packets lost there (est.) |')
    print('| --- | --- | ---: | ---: | ---: | --- | ---: | --- | --- | --- | --- | --- | --- |')
    for k in sorted(rows):
        rs = rows[k]
        d, n, pps, L = k
        drops = [sum(r.get('s_lane_drops') or []) or r.get('sockdrops_srv', {}).get('lfd', 0) for r in rs]
        print(f"| {d} | {n[1:]} | {pps // 1000} | {L} | {len(rs)} | {rng([r['loss_pct'] for r in rs], '%.3f')} "
              f"| {sum(1 for r in rs if r['loss_pct'] <= 0.1)}/{len(rs)} "
              f"| {rng([r['server_us'] for r in rs])} ({mean([r['server_us'] for r in rs]):.2f}) "
              f"| {rng([r['client_us'] for r in rs])} ({mean([r['client_us'] for r in rs]):.2f}) "
              f"| {rng([r['server_cpu_pct'] for r in rs], '%.0f')} | {rng([r['p50'] for r in rs if r['p50'] is not None], '%d')} "
              f"| {', '.join(map(str, drops))} | {', '.join(str(whole_lost(r)) for r in rs)} |")


if __name__ == '__main__':
    globals()['cmd_' + sys.argv[1]](sys.argv[2:])
