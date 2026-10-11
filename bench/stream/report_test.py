#!/usr/bin/env python3
"""Tests for bench/stream/report.py on made-up runs whose answers are known.

The sender run (TCP, 60 s, BBR, target 6 Mbit/s) holds one block: from second
20.05 to 23.0 the sender is stuck. The output time stands still and catches up
at 23.0, the TCP socket collapses (cwnd 100 to 10 at 20.1, back to 100 by 23.3),
the smoothed RTT goes from 30 to 250 ms until 23.5, the send buffer limits it
and 400 kB wait unsent, 40 segments are retransmitted at 23.0, 5 more at 45;
BBR's ProbeRTT dips cwnd to 4 for 0.2 s at 40. The receiver run sees the same
block as an arrival gap of 2.967 s followed by a burst of late frames, a
start-up burst of 10 packets, a last pair of frames that come 1.5 s late (the
stream ending: not a stall) and a jump of 15 frames in pts at second 1 and
another at second 32 (arrival gaps of 0.533 s: the other two stalls; the first
is right after the start-up, where nothing is skipped). The
fieldrec export has an outage of l2 around the block and two events that
nothing falls near, a mute that only the window reaches (2.5 s before the
second stall) and one event outside the run. A second sender run (SRT, with
the real header of srt-live-transmit's statistics) has 50 packets lost, 48
retransmitted and 6 dropped. Run: python3 bench/stream/report_test.py
"""

import contextlib
import csv
import importlib.util
import io
import json
import os
import sys
import tempfile

sys.dont_write_bytecode = True  # no bench/stream/__pycache__ in the tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report  # noqa: E402

T0 = 1791500000.0
DUR = 60.0
LAG = 1.0          # wall time minus output time of the sender
STALL = (20.05, 23.0)

failures = []


def check(name, ok, detail=""):
    if ok:
        print("ok   %s" % name)
    else:
        print("FAIL %s %s" % (name, detail))
        failures.append(name)


def near(x, want, tol):
    return x is not None and abs(x - want) <= tol


# ---- the sender run --------------------------------------------------------------------------

def out_time(t):
    """The sender's output time (seconds) at second t of the run: stuck during the block."""
    if t < STALL[0]:
        return t - LAG
    if t < STALL[1]:
        return STALL[0] - LAG
    return t - LAG


def make_sender(d):
    os.makedirs(d)
    meta = {"kind": "sender", "tool": "send.sh", "run_id": "synthetic", "start_epoch": T0, "end_epoch": T0 + DUR,
            "target_bitrate_bps": 6000000, "stopped_by": "duration",
            "dest": {"scheme": "rtmp", "host": "203.0.113.5", "port": 1935, "transport": "tcp"},
            "encoder": {"used": "h264_nvenc", "container": "flv"},
            "video": {"size": "1920x1080", "fps": 30, "gop_s": 2},
            "clock": {"burn_origin_epoch": T0 + 0.5},
            "kernel": {"tcp_congestion_control": "bbr"}}
    json.dump(meta, open(os.path.join(d, "meta.json"), "w"))
    # progress: a block every 0.2 s from second 1.0
    with open(os.path.join(d, "progress.log"), "w") as fh:
        k = 0
        while True:
            t = 1.0 + 0.2 * k
            if t > DUR:
                break
            o = out_time(t)
            size = int(750000 * o)  # 6 Mbit/s of output time
            drop = 4 if t >= STALL[1] else 0
            for key, val in (("frame", int(o * 30)), ("fps", "0.0"), ("total_size", size), ("out_time_us", int(o * 1e6)),
                             ("dup_frames", 0), ("drop_frames", drop), ("speed", "1.00x")):
                fh.write("%.6f %s=%s\n" % (T0 + t, key, val))
            fh.write("%.6f progress=continue\n" % (T0 + t))
            k += 1
    # tcp.csv every 0.1 s from second 1.0
    cols = ("t,state,local,peer,cc,cwnd,ssthresh,rtt_ms,rttvar_ms,minrtt_ms,rto_ms,mss,pmtu,unacked,retrans_cur,retrans_total,lost,sacked,"
            "reordering,notsent,sendq,bytes_sent,bytes_acked,bytes_retrans,delivered,delivery_rate_bps,pacing_rate_bps,send_bps,busy_ms,"
            "rwnd_limited_ms,sndbuf_limited_ms,app_limited,segs_out,data_segs_out,snd_wnd,bytes_received,segs_in,rcv_ooopack,dsack_dups,"
            "rcv_rtt_ms,rcv_space").split(",")
    with open(os.path.join(d, "tcp.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        retrans = 0
        limited = 0
        segs = 0
        for k in range(int((DUR - 1.0) / 0.1)):
            t = round(1.0 + 0.1 * k, 3)
            segs += 5  # 50 segments a second
            cw = 100
            if 20.1 <= t < 23.1:
                cw = 10
            elif abs(t - 23.1) < 1e-6:
                cw = 40
            elif abs(t - 23.2) < 1e-6:
                cw = 80
            if 40.0 <= t < 40.2:
                cw = 4
            rtt = 250.0 if 20.1 <= t < 23.5 else (80.0 if 45.0 <= t < 45.5 else 30.0)
            if abs(t - 23.0) < 1e-6:
                retrans += 40
            if abs(t - 45.0) < 1e-6:
                retrans += 5
            stuck = 20.1 <= t <= 23.0
            if stuck:
                limited += 100
            row = {"t": "%.6f" % (T0 + t), "state": "ESTAB", "local": "10.0.0.2:40000", "peer": "203.0.113.5:1935", "cc": "bbr",
                   "cwnd": cw, "rtt_ms": rtt, "rttvar_ms": 5, "minrtt_ms": 30, "retrans_total": retrans, "notsent": 400000 if stuck else 0,
                   "sendq": 400000 if stuck else 0, "delivery_rate_bps": 6000000, "sndbuf_limited_ms": limited, "rwnd_limited_ms": 0,
                   "busy_ms": 0, "app_limited": 0, "data_segs_out": segs if k else "", "bytes_retrans": retrans * 1400,
                   "bytes_sent": segs * 1400 if k else ""}  # the first row is a socket still connecting: no counters yet
            if k == 0:
                row["state"] = "SYN-SENT"
            w.writerow([row.get(c, "") for c in cols])
    return meta


# ---- the receiver run ------------------------------------------------------------------------

def make_receiver(d):
    os.makedirs(d)
    json.dump({"kind": "receiver", "tool": "recv.sh", "run_id": "synthetic-rx", "start_epoch": T0 + 0.9, "end_epoch": T0 + DUR},
              open(os.path.join(d, "meta.json"), "w"))
    rows = []
    k = 0
    skipped = 0
    prev = 0.0
    while True:
        pts = k / 30.0
        if k >= 32 * 30:  # a jump of 15 frames at second 1 and another at second 32
            pts = (k + 30) / 30.0
        elif k >= 1 * 30:
            pts = (k + 15) / 30.0
        g = T0 + LAG + pts  # when the sender made it
        if pts > DUR - 3:
            break
        a = g + 0.5
        if STALL[0] <= g - T0 < STALL[1]:
            a = T0 + STALL[1] + 0.5 + 0.0005 * skipped
            skipped += 1
        if k < 10:
            a = T0 + 1.7  # the first packets come out of ffmpeg's probing in one lump
        a = max(a, prev + 0.0001)  # TCP delivers in order
        prev = a
        size = 25000 if k % 60 == 0 else 3000
        rows.append((a, pts, size, "K__" if k % 60 == 0 else "___"))
        k += 1
    rows[-2] = (rows[-2][0] + 1.5,) + rows[-2][1:]  # the stream ends with a flush: the last two frames come 1.5 s late
    rows[-1] = (rows[-1][0] + 1.5,) + rows[-1][1:]
    with open(os.path.join(d, "packets.csv"), "w") as fh:
        fh.write("t,pts,dts,size,flags\n")
        for a, pts, size, fl in rows:
            fh.write("%.6f,%.6f,%.6f,%d,%s\n" % (a, pts, pts, size, fl))
    with open(os.path.join(d, "tcp.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        cols = "t,state,local,peer,cc,cwnd,rtt_ms,segs_in,rcv_ooopack,dsack_dups".split(",")
        w.writerow(cols)
        for i in range(50):
            w.writerow(["%.6f" % (T0 + 2 + i), "ESTAB", "x", "y", "bbr", 10, 1.0, 100 * i, 7 if i >= 25 else 0, 0])
    # the receiver's own SRT statistics (srt-live-transmit -f on its side): 30 lost, 28 retransmitted, 4 dropped, 2 belated
    import datetime
    with open(os.path.join(d, "srt-stats.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(SRT_HEAD)
        for k in range(100):  # every 0.5 s
            t = T0 + 0.5 * (k + 1)
            row = {c: 0 for c in SRT_HEAD}
            row["Timepoint"] = datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f+0000")
            row.update({"pktRecvUnique": 100 * (k + 1), "pktRecv": 100 * (k + 1), "pktRcvLoss": 30 if k >= 40 else 0,
                        "pktRcvRetrans": 28 if k >= 41 else 0, "pktRcvDrop": 4 if k >= 70 else 0, "pktRcvBelated": 2 if k >= 71 else 0,
                        "msRcvBuf": 150, "msRcvTsbPdDelay": 200, "msRTT": 25})
            w.writerow([row[c] for c in SRT_HEAD])
    return rows


# ---- the fieldrec export and the SRT run -----------------------------------------------------

def make_fieldrec(d):
    os.makedirs(d)
    head = "kind,link,label,segment,start_utc,start_epoch,end_utc,end_epoch,duration_s,ends".split(",")
    with open(os.path.join(d, "events.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(head)
        for kind, link, s, e in (("outage", "l2", 19.0, 25.0), ("upload_mute", "l3", 40.0, 41.5), ("outage", "l1", 52.0, 53.0),
                                 ("download_mute", "l1", 30.0, 31.0), ("download_mute", "l2", 100.0, 101.0), ("restart", "", 5.0, 5.0)):
            w.writerow([kind, link, link, 0, "", "%.3f" % (T0 + s), "", "%.3f" % (T0 + e), "%.3f" % (e - s), "recovered"])
    with open(os.path.join(d, "tunnel.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["segment", "time_utc", "time_epoch", "uptime_ms", "clock_synced", "interval_ms", "down_packets_delta", "lost_all_delta"])
        for i in range(120):
            t = T0 + 0.5 * (i + 1)
            w.writerow([0, "", "%.3f" % t, i * 500, 1, 500, 100, 38 if abs(t - (T0 + 23.0)) < 0.01 else 0])


SRT_HEAD = ("Timepoint,Time,SocketID,Time,pktFlowWindow,pktCongestionWindow,pktFlightSize,msRTT,mbpsBandwidth,mbpsMaxBW,pktSent,"
            "pktSentUnique,pktSndLoss,pktSndDrop,pktRetrans,pktSndFilterExtra,byteSent,byteSentUnique,byteSndDrop,byteAvailSndBuf,"
            "msSndBuf,mbpsSendRate,usPktSndPeriod,pktRecv,pktRecvUnique,pktRcvLoss,pktRcvDrop,pktRcvRetrans,pktRcvBelated,"
            "pktRcvFilterExtra,pktRcvFilterSupply,pktRcvFilterLoss,byteRecv,byteRecvUnique,byteRcvLoss,byteRcvDrop,byteAvailRcvBuf,"
            "msRcvBuf,mbpsRecvRate,msRcvTsbPdDelay").split(",")


def make_srt(d):
    os.makedirs(d)
    json.dump({"kind": "sender", "run_id": "synthetic-srt", "start_epoch": T0, "end_epoch": T0 + 30, "target_bitrate_bps": 6000000,
               "dest": {"scheme": "srt", "host": "203.0.113.5", "port": 9000, "transport": "udp"}},
              open(os.path.join(d, "meta.json"), "w"))
    import datetime
    with open(os.path.join(d, "srt-stats.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(SRT_HEAD)
        sent = loss = drop = retr = 0
        for k in range(150):  # every 0.2 s
            t = T0 + 0.2 * (k + 1)
            sent += 100
            if 49 <= k <= 53:  # seconds 10.0 to 10.8: five reports, 50 packets lost, 48 retransmitted
                loss += 10
                retr += 8 if k == 53 else 10
            if k == 99:  # second 20.0
                drop += 6
            rtt = 120.0 if 49 <= k <= 53 else 20.0
            row = {c: 0 for c in SRT_HEAD}
            row["Timepoint"] = datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f+0000")
            row.update({"msRTT": rtt, "pktSent": sent, "pktSentUnique": sent, "pktSndLoss": loss, "pktSndDrop": drop, "pktRetrans": retr,
                        "msSndBuf": 40, "mbpsBandwidth": 80})
            w.writerow([row[c] for c in SRT_HEAD])


# ---- the tests -------------------------------------------------------------------------------

def run_report(argv):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = report.main(argv)
    return rc, buf.getvalue()


def main():
    tmp = tempfile.mkdtemp(prefix="stream-report-test-")
    snd, rxd, frd, srtd = (os.path.join(tmp, n) for n in ("sender", "receiver", "fieldrec", "srt"))
    make_sender(snd)
    make_receiver(rxd)
    make_fieldrec(frd)
    make_srt(srtd)
    jpath = os.path.join(tmp, "out.json")

    # pure helpers
    check("pct interpolates", report.pct([1, 2, 3, 4], 50) == 2.5 and report.pct([], 50) is None and report.pct([5], 99) == 5)
    check("merge_intervals merges near ones", report.merge_intervals([(0, 1, 2), (1.5, 2, 3), (5, 6, 1)], 1.0) == [(0, 2, 5), (5, 6, 1)])
    eps = report.episodes([0, 1, 2, 3, 4], [False, True, True, False, True], 0.5)
    check("episodes", eps == [(1, 2.5, 1, 2), (4, 4.5, 4, 4)], str(eps))
    check("covered unions the padded intervals", abs(report.covered([(10, 12), (13, 14)], 0, 100, 1.0) - 6.0) < 1e-9)
    dup = os.path.join(tmp, "dup.csv")
    open(dup, "w").write("Timepoint,Time,Time,x\n2026-10-11T20:00:00.000000+0000,1,2,3\n")
    tb = report.Table(dup)
    check("Table: the first of two columns with the same name wins", tb.num["Time"] == [1.0] and tb.raw["x"] == ["3"])
    check("startup_burst finds a lump and leaves a clean start alone",
          report.startup_burst([0.0, 0.001, 0.002, 0.003, 0.5], [0.0, 0.1, 0.2, 0.3, 0.4]) == 3
          and report.startup_burst([0.0, 0.033, 0.066, 0.1], [0.0, 0.033, 0.066, 0.1]) == 0)

    rc, text = run_report([snd, "--rx", rxd, "--fieldrec", frd, "--json", jpath, "--no-plots"])
    r = json.load(open(jpath))
    check("exit code 0", rc == 0)

    # sender
    s = r["sender"]
    check("sender: mean bitrate is the target (it catches up)", near(s["mean_mbps"], 6.0, 0.05), str(s.get("mean_mbps")))
    check("sender: two seconds under half the target", s["bins_below_half_target"] == 2, str(s.get("bins_below_half_target")))
    check("sender: speed under 0.95x in two seconds, the slowest is 0", s["bins_speed_below_0_95"] == 2 and near(s["speed_min_1s"], 0.0, 0.01),
          "%s %s" % (s.get("bins_speed_below_0_95"), s.get("speed_min_1s")))
    check("sender: dropped frames", s["drop_frames"] == 4 and s["dup_frames"] == 0)
    check("sender: one held-back episode of 2750 ms", s["holds"] == 1 and near(s["hold_peak_ms"], 2750, 60), "%s %s" % (s.get("holds"), s.get("hold_peak_ms")))
    check("sender: the burned clock is 0.5 s behind", near(s["clock_offset_s"], 0.5, 0.05), str(s.get("clock_offset_s")))
    check("sender: bitrate p50 is the target", near(s["bitrate_p50_mbps"], 6.0, 0.1))
    check("sender: the first 2 s are left out of the bins", s["bins"] in (56, 57), str(s.get("bins")))

    # tcp
    t = r["tcp"]
    check("tcp: 45 segments retransmitted in 2 bursts", t["retrans_segments"] == 45 and t["retrans_bursts"] == 2 and t["retrans_burst_max"] == 40,
          "%s %s %s" % (t.get("retrans_segments"), t.get("retrans_bursts"), t.get("retrans_burst_max")))
    want_pct = 100.0 * 45 / (5 * (int((DUR - 1.0) / 0.1) - 2))  # 5 segments per sample, counted from the second row
    check("tcp: retransmission rate", near(t["retrans_pct"], want_pct, 0.01), "%s vs %s" % (t.get("retrans_pct"), want_pct))
    check("tcp: minimum RTT and the inflation", near(t["rtt_min_ms"], 30, 0.01) and near(t["rtt_infl_max_ms"], 220, 0.01))
    check("tcp: 3.4 s over +100 ms in one spike", near(t["rtt_over_100ms_s"], 3.4, 0.11) and t["rtt_spikes"] == 1,
          "%s %s" % (t.get("rtt_over_100ms_s"), t.get("rtt_spikes")))
    check("tcp: two collapses, one of them a ProbeRTT dip", t["collapses"] == 2 and t["collapses_probe_rtt_like"] == 1,
          "%s %s" % (t.get("collapses"), t.get("collapses_probe_rtt_like")))
    c = [x for x in t["collapse_list"] if not x["probe_rtt_like"]][0]
    check("tcp: the collapse 100 -> 10 at 20.1", c["from"] == 100 and c["to"] == 10 and near(c["start"], T0 + 20.1, 0.001), str(c))
    check("tcp: the collapse lasts 3.1 s (back at 80 % of the peak)", near(c["end"] - c["start"], 3.1, 0.05), str(c))
    check("tcp: 3.0 s limited by the send buffer", near(t["sndbuf_limited_s"], 3.0, 0.01), str(t.get("sndbuf_limited_s")))
    check("tcp: 3.0 s with more than 64 KiB unsent", near(t["backlog_over_64k_s"], 3.0, 0.11), str(t.get("backlog_over_64k_s")))
    check("tcp: the sampling interval", near(t["interval_p50_ms"], 100, 1))
    check("tcp: delivery rate", near(t["delivery_rate_p50_mbps"], 6.0, 0.01))
    check("tcp: the server's socket saw 7 out-of-order segments", r["rx_tcp"].get("rcv_ooopack") == 7, str(r["rx_tcp"].get("rcv_ooopack")))

    # receiver
    x = r["receiver"]
    check("receiver: start-up burst of 9 left out", x["startup_burst_packets"] == 9, str(x.get("startup_burst_packets")))
    ms = [v["ms"] for v in x.get("stall_list", [])]
    check("receiver: three stalls in time order, 533, 2967 and 533 ms; the first right after the start-up",
          x["stalls"] == 3 and near(ms[0], 533, 10) and near(ms[1], 2967, 10) and near(ms[2], 533, 10), str([round(v) for v in ms]))
    check("receiver: the big stall starts at 20.533 and the first at 2.467",
          near(x["stall_list"][1]["start"], T0 + 20.533, 0.01) and near(x["stall_list"][0]["start"], T0 + 2.467, 0.05))
    check("receiver: stall percentiles", near(x["stall_total_s"], 4.033, 0.03) and near(x["stall_p50_ms"], 533, 10) and near(x["stall_max_ms"], 2967, 10))
    check("receiver: one late episode, peak about 2.933 s (the first late frame)", x["late_episodes"] == 1 and near(x["delay_max_ms"], 2933, 20),
          "%s %s" % (x.get("late_episodes"), x.get("delay_max_ms")))
    check("receiver: two jumps in pts, 30 frames missing", x["pts_jumps"] == 2 and x["frames_missing"] == 30, "%s %s" % (x.get("pts_jumps"), x.get("frames_missing")))
    check("receiver: the frame interval", near(x["frame_interval_ms"], 33.3, 0.2))
    check("receiver: median delay over the floor is small", x["delay_p50_ms"] < 20, str(x.get("delay_p50_ms")))

    # fieldrec
    f = r["fieldrec"]
    check("fieldrec: four events overlap the run (not the one at 100 s, not the restart)", f["events"] == 4, str(f.get("events")))
    rows = {(w["kind"], w["link"]): w for w in f["rows"]}
    check("fieldrec: the outage of l2 has an incident near, the others have none; the mute is reached by the window only",
          rows[("outage", "l2")]["events_with_incident"] == 1 and rows[("outage", "l1")]["events_with_incident"] == 0
          and rows[("upload_mute", "l3")]["events_with_incident"] == 0 and rows[("outage", "all")]["events_with_incident"] == 1
          and rows[("download_mute", "l1")]["events_with_incident"] == 1)
    bt = rows[("outage", "all")]["by_type"]
    check("fieldrec: the first stall, the collapse, the RTT spike and the hold are near an outage; the second stall is not",
          bt["rx_stall"]["near"] == 1 and bt["rx_stall"]["total"] == 3 and bt["tcp_collapse"]["near"] == 1 and bt["tcp_rtt_spike"]["near"] == 1 and bt["sender_hold"]["near"] == 1,
          str({k: v["near"] for k, v in bt.items()}))
    check("fieldrec: the retransmission at 45 s is not near", bt["tcp_retrans"]["near"] == 1 and bt["tcp_retrans"]["total"] == 2, str(bt["tcp_retrans"]))
    # windows [16, 28] and [49, 56] of a 60 s run: 19 s, so 0.3167 of a stall by chance, for each of the three
    check("fieldrec: chance is the covered share of the run", near(bt["rx_stall"]["expected"], 3 * 19.0 / 60.0, 0.01), str(bt["rx_stall"]["expected"]))
    check("fieldrec: lost_all and the retransmission burst that it explains",
          f["lost_all_packets"] == 38 and f["retrans_bursts"] == 2 and f["retrans_bursts_with_lost_all"] == 1,
          "%s %s %s" % (f.get("lost_all_packets"), f.get("retrans_bursts"), f.get("retrans_bursts_with_lost_all")))
    inc = [i for i in f["incidents"] if i["type"] == "rx_stall" and "for 29" in i["detail"]][0]
    check("fieldrec: the stall is 'during' the outage of l2", inc["near"] and inc["near"][0]["link"] == "l2" and inc["near"][0]["where"] == "during it", str(inc["near"]))
    # a wider and a narrower window, and a clock offset
    _, _ = run_report([snd, "--rx", rxd, "--fieldrec", frd, "--json", jpath, "--no-plots", "--window", "0.1"])
    r2 = json.load(open(jpath))
    check("fieldrec: --window 0.1 still finds the stall at the outage of l2, no longer the one 2.5 s after the mute",
          {w["link"]: w for w in r2["fieldrec"]["rows"] if w["kind"] == "outage"}["l2"]["by_type"]["rx_stall"]["near"] == 1
          and {(w["kind"], w["link"]): w for w in r2["fieldrec"]["rows"]}[("download_mute", "l1")]["events_with_incident"] == 0)
    _, _ = run_report([snd, "--rx", rxd, "--fieldrec", frd, "--json", jpath, "--no-plots", "--fieldrec-offset", "30"])
    r3 = json.load(open(jpath))
    row = [w for w in r3["fieldrec"]["rows"] if w["kind"] == "outage" and w["link"] == "l2"]
    check("fieldrec: --fieldrec-offset moves the events away from the stall", row and row[0]["by_type"]["rx_stall"]["near"] == 0, str(row))

    _, _ = run_report([snd, "--rx", rxd, "--json", jpath, "--no-plots", "--skip-end", "0"])
    check("receiver: with --skip-end 0 the stream's last flush counts as a fourth stall",
          json.load(open(jpath))["receiver"]["stalls"] == 4, str(json.load(open(jpath))["receiver"]["stalls"]))

    sx = r["srt_rx"]
    check("srt receiver: 30 lost, 28 retransmitted, 4 dropped, 2 belated, as shares of the 9900 received",
          sx["pktRcvLoss"] == 30 and sx["pktRcvRetrans"] == 28 and sx["pktRcvDrop"] == 4 and sx["pktRcvBelated"] == 2
          and near(sx["rcv_loss_pct"], 100.0 * 30 / 9900, 1e-6) and sx["packets_recv_unique"] == 9900,
          "%s %s %s" % (sx.get("pktRcvLoss"), sx.get("pktRcvDrop"), sx.get("packets_recv_unique")))
    types = {i["type"] for i in r["incidents"]}
    check("srt receiver: its losses are srt_rx_ incidents, apart from the sender's", {"srt_rx_loss", "srt_rx_drop"} <= types and "srt_loss" not in types, str(types))
    check("srt receiver: the report has a receiver line", "- receiver: 9900 packets received; lost 30" in text and "SRT latency 200 ms" in text)

    # text
    check("text: names the stall and the collapse", "stalls (no video packet for 200 ms or more, the last 2 s left out): 3" in text and "collapses under 50 %" in text)
    check("text: has the link events table", "| outage | l2 |" in text)

    # srt
    rc, text2 = run_report([srtd, "--json", jpath, "--no-plots"])
    q = json.load(open(jpath))["srt"]
    check("srt: 50 lost, 48 retransmitted, 6 dropped", q["pktSndLoss"] == 50 and q["pktRetrans"] == 48 and q["pktSndDrop"] == 6,
          "%s %s %s" % (q.get("pktSndLoss"), q.get("pktRetrans"), q.get("pktSndDrop")))
    check("srt: loss as a share of the packets", near(q["loss_pct"], 100.0 * 50 / q["packets_sent_unique"], 1e-6) and near(q["packets_sent_unique"], 14900, 1))
    check("srt: RTT min and max", near(q["rtt_min_ms"], 20, 0.01) and near(q["rtt_max_ms"], 120, 0.01))
    check("srt: a loss incident and a drop incident", [i["type"] for i in json.load(open(jpath))["incidents"]].count("srt_loss") == 1
          and [i["type"] for i in json.load(open(jpath))["incidents"]].count("srt_drop") == 1)
    check("srt: the report says so", "## SRT (srt-live-transmit statistics" in text2 and "lost 50" in text2)

    # a sender without SRT statistics says how to get them
    nosrt = os.path.join(tmp, "nosrt")
    make_srt(nosrt)
    os.remove(os.path.join(nosrt, "srt-stats.csv"))
    with open(os.path.join(nosrt, "progress.log"), "w") as fh:
        for k in range(10):
            fh.write("%.6f total_size=%d\n%.6f out_time_us=%d\n%.6f progress=continue\n" % (T0 + k, k * 1000, T0 + k, k * 1000000, T0 + k))
    _, text3 = run_report([nosrt, "--no-plots"])
    check("srt: without statistics it points to -S", "no SRT statistics" in text3)

    # a plot, when matplotlib is there
    if importlib.util.find_spec("matplotlib"):
        png = os.path.join(tmp, "plots")
        _, text4 = run_report([snd, "--rx", rxd, "--fieldrec", frd, "--plots", png])
        p = os.path.join(png, "report.png")
        check("plot: report.png is written", os.path.isfile(p) and os.path.getsize(p) > 10000, text4[-200:])
        _, _ = run_report([srtd, "--plots", png])
        check("plot: an SRT run draws too", os.path.getsize(os.path.join(png, "report.png")) > 10000)
    else:
        print("skip plots (matplotlib is not installed)")

    # a receiver directory alone
    rc, text5 = run_report([rxd, "--no-plots"])
    check("receiver alone: reported", rc == 0 and "## Receiver" in text5 and "## Sender" not in text5)

    if failures:
        print("%d FAILED: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("report_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
