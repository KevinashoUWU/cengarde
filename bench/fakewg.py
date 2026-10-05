#!/usr/bin/env python3
"""Two fake WireGuard ends for bench/lab.d/wgpoke.sh, with just the handshake
behaviour that scenario needs (no crypto: messages only have WireGuard's
shapes, type byte and length).

  fakewg.py vps BIND POKE [DELAY_FILE]
                              the VPS's WireGuard behind the server engine,
                              ignoring every initiation (as WireGuard does
                              with a timestamp older than the last one it
                              took). Its endpoint is where the last data or
                              response came from; a datagram to POKE makes
                              it send an initiation there, after the seconds
                              DELAY_FILE holds (0 without it): WireGuard
                              with a session still valid waits 15 s.
  fakewg.py router BIND PEER MODE
                              the router's WireGuard in front of the client
                              engine at PEER. MODE data sends data every
                              100 ms; MODE knock sends an initiation every
                              500 ms. Either way it answers an initiation
                              with a response.

Each prints one line per event, flushed: "endpoint PORT", "ignored PORT",
"poked", "initiation-to PORT" (vps); "got-initiation", "sent-response"
(router).
"""
import select
import socket
import sys
import time

INIT, RESP, DATA = 1, 2, 4
SIZE = {INIT: 148, RESP: 92, DATA: 200}


def msg(t):
    return bytes([t, 0, 0, 0]) + b"\xab" * (SIZE[t] - 4)


def kind(b):
    if len(b) >= 4 and not any(b[1:4]):
        if b[0] == INIT and len(b) == 148:
            return INIT
        if b[0] == RESP and len(b) == 92:
            return RESP
        if b[0] == DATA and len(b) >= 32:
            return DATA
    return 0


def addr(s):
    host, port = s.rsplit(":", 1)
    return host, int(port)


def say(*words):
    print(*words, flush=True)


def delay(path):
    try:
        with open(path) as f:
            return float(f.read().strip() or 0)
    except (OSError, ValueError):
        return 0.0


def vps(bind, poke, delay_file=None):
    wg = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    wg.bind(addr(bind))
    pk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    pk.bind(addr(poke))
    endpoint, due = None, None
    while True:
        wait = None if due is None else max(0.0, due - time.monotonic())
        ready = select.select([wg, pk], [], [], wait)[0]
        if due is not None and time.monotonic() >= due:
            due = None
            if endpoint:
                wg.sendto(msg(INIT), endpoint)
                say("initiation-to", endpoint[1])
        for s in ready:
            b, src = s.recvfrom(65536)
            if s is pk:
                say("poked")
                if due is None:
                    due = time.monotonic() + (delay(delay_file) if delay_file else 0.0)
                continue
            k = kind(b)
            if k == INIT:
                say("ignored", src[1])
            elif k in (RESP, DATA) and src != endpoint:
                endpoint = src
                say("endpoint", src[1])


def router(bind, peer, mode):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(addr(bind))
    peer = addr(peer)
    every = 0.1 if mode == "data" else 0.5
    nxt = 0.0
    while True:
        now = time.monotonic()
        if now >= nxt:
            s.sendto(msg(DATA if mode == "data" else INIT), peer)
            nxt = now + every
        if select.select([s], [], [], max(0.0, nxt - now))[0]:
            b, _ = s.recvfrom(65536)
            if kind(b) == INIT:
                say("got-initiation")
                s.sendto(msg(RESP), peer)
                say("sent-response")


if __name__ == "__main__":
    if sys.argv[1:2] == ["vps"] and len(sys.argv) in (4, 5):
        vps(*sys.argv[2:])
    elif sys.argv[1:2] == ["router"] and len(sys.argv) == 5 and sys.argv[4] in ("data", "knock"):
        router(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)
