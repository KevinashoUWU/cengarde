#!/usr/bin/env python3
"""quicperf: a bulk-transfer QUIC server and client for the bonding lab.

The bonding lab (bench/lab.d/bond.sh, story 012) puts real TCP (iperf3) inside
cengarde's tunnel. This is the QUIC next to it: one
stream of bulk data for N seconds, up (the client sends) or down (the server
sends), over aioquic, so that the same links also see a QUIC sender (what
browsers and phones use, though not with this library). Its loss detection is
the point: aioquic declares a packet lost after 3 later packets are
acknowledged, or after 9/8 of an RTT, and never adapts either threshold, so a
path that reorders by more than that makes it count packets as lost that were
only late.

    python3 -I bench/quicperf.py server [--bind ADDR] [--port 4433] [--cc cubic]
    python3 -I bench/quicperf.py client HOST[:PORT] [--dir up|down] [--secs 10]
                                        [--cc cubic] [--mtu 1280]

Needs aioquic (pip install aioquic==1.2.0, which also brings cryptography) and
nothing else outside the standard library. The server makes a self-signed
certificate when it starts, in memory: there is nothing to generate or commit.
The client does not verify it, and speaks IPv4 only (aioquic's own connect()
needs an IPv6 socket, which a router or a container may lack). Give both ends the same --cc and --mtu: each end
sets them for what it sends. --cc is "cubic" or "reno", the two controllers
aioquic has.

The client prints one JSON object (exit status 0 when the transfer completed):

    dir        "up" or "down", from the client's side
    cc         the congestion controller of the sender
    secs_req   how long the sender writes (--secs)
    bytes      what the receiver got, and secs the time from its first byte to
               its last one; mbps = bytes * 8 / secs / 1e6, the goodput
    sent       ack-eliciting packets the sender sent (the ones that can be lost)
    lost       packets the sender declared lost
    spurious   of those, the ones acknowledged afterwards, so they were only
               late; this one is counted here, aioquic does not keep it
    srtt_ms, min_rtt_ms   the sender's smoothed and minimum RTT
    cwnd_bytes the sender's congestion window at the end
    mtu, aioquic   the datagram size and the library version

The protocol is the minimum: stream 0 (client, bidirectional) carries JSON
lines. The client asks ("start", direction, seconds); the bulk data goes on a
unidirectional stream of the sender and ends with a FIN; the receiver's end
answers on stream 0 with what it measured (the server, for "up", once the data
ended; for "down", the sender's statistics once the client says "done").

It is pure Python, one core: expect its own CPU to limit it somewhere around
100 to 200 Mbit/s, below what iperf3 reaches. It uses a few aioquic internals
(the loss recovery object and the stream sender), which is why the version is
pinned.
"""

import argparse
import asyncio
import datetime
import json
import ssl
import sys
import time
from bisect import bisect_left, insort

try:
    import aioquic
    from aioquic.asyncio import QuicConnectionProtocol, serve
    from aioquic.quic.configuration import QuicConfiguration
    from aioquic.quic.connection import QuicConnection
    from aioquic.quic.events import ConnectionTerminated, StreamDataReceived
    from aioquic.quic.packet import QuicPacketType
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID
except ImportError as e:
    sys.exit(f"quicperf: needs aioquic (pip install aioquic==1.2.0): {e}")

ALPN = "quicperf"
CONTROL = 0  # stream 0: the client's bidirectional stream, JSON lines
CHUNK = 64 * 1024  # what is written to the stream at a time
BACKLOG = 128 * 1024  # written but not yet sent: enough to keep the sender busy
WINDOW = 16 * 1024 * 1024  # flow control: never the limit
PORT = 4433


def self_signed():
    """A certificate and its key, valid for a day, made here and kept in memory."""
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "quicperf")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=1))
        .sign(key, hashes.SHA256())
    )
    return cert, key


def configuration(args, is_client):
    cfg = QuicConfiguration(
        is_client=is_client,
        alpn_protocols=[ALPN],
        congestion_control_algorithm=args.cc,
        max_datagram_size=args.mtu,
        max_data=2 * WINDOW,
        max_stream_data=WINDOW,
        idle_timeout=30.0,
    )
    if is_client:
        cfg.verify_mode = ssl.CERT_NONE
    else:
        cfg.certificate, cfg.private_key = self_signed()
    return cfg


class LossCounters:
    """Packets sent, declared lost and found spurious, by one end.

    aioquic keeps no such counters, so three methods of its loss recovery are
    wrapped. Only 1-RTT packets count (the handshake is not the transfer), and
    "sent" is only the ack-eliciting ones: a bare ACK is never declared lost.
    A packet is spurious when an ACK arrives for it after it was declared lost:
    aioquic forgets such a packet and ignores that ACK, so it is checked here
    against the sorted list of the numbers declared lost.
    """

    def __init__(self, recovery):
        self.recovery = recovery
        self.sent = self.lost = self.spurious = 0
        self._lost_pns = []  # sorted packet numbers declared lost, not yet acked
        self._space = None  # the 1-RTT packet number space, once it sends
        sent, lost, acked = recovery.on_packet_sent, recovery._on_packets_lost, recovery.on_ack_received

        def on_packet_sent(*, packet, space):
            if packet.packet_type == QuicPacketType.ONE_RTT:
                self._space = space
                if packet.is_ack_eliciting:
                    self.sent += 1
            return sent(packet=packet, space=space)

        def on_packets_lost(*, now, packets, space):
            packets = list(packets)
            for p in packets:
                if p.packet_type == QuicPacketType.ONE_RTT and p.is_ack_eliciting:
                    self.lost += 1
                    insort(self._lost_pns, p.packet_number)
            return lost(now=now, packets=packets, space=space)

        def on_ack_received(*, ack_rangeset, ack_delay, now, space):
            if space is self._space and self._lost_pns:
                for r in ack_rangeset:
                    i, j = bisect_left(self._lost_pns, r.start), bisect_left(self._lost_pns, r.stop)
                    if j > i:
                        self.spurious += j - i
                        del self._lost_pns[i:j]
            return acked(ack_rangeset=ack_rangeset, ack_delay=ack_delay, now=now, space=space)

        recovery.on_packet_sent = on_packet_sent
        recovery._on_packets_lost = on_packets_lost
        recovery.on_ack_received = on_ack_received

    def snapshot(self, cc):
        r = self.recovery
        ms = lambda s: None if s in (0, float("inf")) else round(s * 1000, 2)
        return {
            "cc": cc,
            "sent": self.sent,
            "lost": self.lost,
            "spurious": self.spurious,
            "srtt_ms": ms(r._rtt_smoothed),
            "min_rtt_ms": ms(r._rtt_min),
            "cwnd_bytes": r.congestion_window,
        }


class Sink:
    """The receiving side of the bulk stream: bytes, and when the first and last came."""

    def __init__(self):
        self.bytes = 0
        self.first = self.last = None

    def add(self, n):
        self.last = time.monotonic()
        if self.first is None:
            self.first = self.last
        self.bytes += n

    def result(self):
        secs = (self.last - self.first) if self.first is not None else 0.0
        mbps = self.bytes * 8 / secs / 1e6 if secs > 0 else 0.0
        return {"bytes": self.bytes, "secs": round(secs, 3), "mbps": round(mbps, 2)}


class Peer(QuicConnectionProtocol):
    """What both ends share: counters, control lines and the bulk sender."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.counters = LossCounters(self._quic._loss)
        self.cc = self._quic.configuration.congestion_control_algorithm
        self.sink = Sink()
        self.control_buf = b""

    def send_line(self, msg, end=False):
        self._quic.send_stream_data(CONTROL, json.dumps(msg).encode() + b"\n", end_stream=end)
        self.transmit()

    async def send_bulk(self, stream_id, secs):
        """Write zeros for secs seconds, only as fast as the connection sends them."""
        zeros, written = bytes(CHUNK), CHUNK
        end = time.monotonic() + secs
        self._quic.send_stream_data(stream_id, zeros)  # this creates the stream
        sender = self._quic._streams[stream_id].sender
        while time.monotonic() < end:
            if written - sender.highest_offset < BACKLOG:
                self._quic.send_stream_data(stream_id, zeros)
                written += CHUNK
                self.transmit()
            else:
                await asyncio.sleep(0.001)
        self._quic.send_stream_data(stream_id, b"", end_stream=True)
        self.transmit()

    def control_lines(self, data):
        self.control_buf += data
        while b"\n" in self.control_buf:
            line, self.control_buf = self.control_buf.split(b"\n", 1)
            yield json.loads(line)


class ServerPeer(Peer):
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.replied = False

    def quic_event_received(self, event):
        if not isinstance(event, StreamDataReceived):
            return
        if event.stream_id != CONTROL:  # the client's bulk data: "up"
            self.sink.add(len(event.data))
            if event.end_stream:
                self.reply()
            return
        for msg in self.control_lines(event.data):
            if msg["op"] == "start" and msg["dir"] == "down":
                sid = self._quic.get_next_available_stream_id(is_unidirectional=True)
                asyncio.ensure_future(self.send_bulk(sid, msg["secs"]))
            elif msg["op"] == "done":  # "down": the client has all of it
                self.reply()

    def reply(self):
        if not self.replied:
            self.replied = True
            self.send_line({"rx": self.sink.result(), "tx": self.counters.snapshot(self.cc)}, end=True)


class ClientPeer(Peer):
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        loop = asyncio.get_event_loop()
        self.data_done = loop.create_future()  # the server's bulk stream ended ("down")
        self.answer = loop.create_future()  # the server's reply on the control stream

    def quic_event_received(self, event):
        if isinstance(event, ConnectionTerminated):
            for fut in (self.data_done, self.answer):
                if not fut.done():
                    fut.set_exception(RuntimeError(f"connection closed: {event.reason_phrase or event.error_code}"))
                    fut.exception()  # nobody may be waiting on this one: do not warn
        elif isinstance(event, StreamDataReceived):
            if event.stream_id != CONTROL:  # the server's bulk data: "down"
                self.sink.add(len(event.data))
                if event.end_stream and not self.data_done.done():
                    self.data_done.set_result(None)
            else:
                for msg in self.control_lines(event.data):
                    if not self.answer.done():
                        self.answer.set_result(msg)

    async def run(self, direction, secs):
        self.send_line({"op": "start", "dir": direction, "secs": secs})
        if direction == "up":
            sid = self._quic.get_next_available_stream_id(is_unidirectional=True)
            await self.send_bulk(sid, secs)
        else:
            await self.data_done
            self.send_line({"op": "done"}, end=True)
        reply = await self.answer
        rx, tx = (reply["rx"], self.counters.snapshot(self.cc)) if direction == "up" else (self.sink.result(), reply["tx"])
        return {
            "dir": direction,
            "cc": tx["cc"],
            "secs_req": secs,
            "bytes": rx["bytes"],
            "secs": rx["secs"],
            "mbps": rx["mbps"],
            **{k: tx[k] for k in ("sent", "lost", "spurious", "srtt_ms", "min_rtt_ms", "cwnd_bytes")},
        }


async def run_server(args):
    await serve(args.bind, args.port, configuration=configuration(args, False), create_protocol=ServerPeer)
    print(f"quicperf: listening on {args.bind}:{args.port} (cc {args.cc}, mtu {args.mtu})", file=sys.stderr, flush=True)
    await asyncio.Event().wait()


async def run_client(args):
    # Not aioquic.asyncio.connect(): it always binds an IPv6 dual-stack socket,
    # and the lab (like many routers and containers) can be IPv4 only.
    host, _, port = args.target.partition(":")
    loop = asyncio.get_running_loop()
    quic = QuicConnection(configuration=configuration(args, True))
    transport, peer = await loop.create_datagram_endpoint(lambda: ClientPeer(quic), local_addr=("0.0.0.0", 0))
    try:
        peer.connect((host, int(port or PORT)))
        await peer.wait_connected()
        result = await peer.run(args.dir, args.secs)
        result.update(mtu=args.mtu, aioquic=aioquic.__version__)
        peer.close()
        try:
            await asyncio.wait_for(peer.wait_closed(), 1)
        except asyncio.TimeoutError:
            pass
    finally:
        transport.close()
    print(json.dumps(result))


def main():
    ap = argparse.ArgumentParser(description="bulk QUIC transfer for the bonding lab (see the top of this file)")
    sub = ap.add_subparsers(dest="role", required=True)
    for role in ("server", "client"):
        p = sub.add_parser(role)
        p.add_argument("--cc", choices=("cubic", "reno"), default="cubic", help="congestion controller (cubic)")
        p.add_argument("--mtu", type=int, default=1280, help="QUIC datagram size in bytes (1280)")
    sub.choices["server"].add_argument("--bind", default="0.0.0.0", help="address to listen on (0.0.0.0)")
    sub.choices["server"].add_argument("--port", type=int, default=PORT, help=f"UDP port ({PORT})")
    c = sub.choices["client"]
    c.add_argument("target", help=f"HOST[:PORT] ({PORT})")
    c.add_argument("--dir", choices=("up", "down"), default="up", help="up: the client sends (up)")
    c.add_argument("--secs", type=int, default=10, help="seconds the sender writes (10)")
    c.add_argument("--timeout", type=int, default=20, help="extra seconds to wait for the end (20)")
    args = ap.parse_args()
    try:
        asyncio.run(run_server(args) if args.role == "server" else asyncio.wait_for(run_client(args), args.secs + args.timeout))
    except KeyboardInterrupt:
        pass
    except (asyncio.TimeoutError, RuntimeError, OSError) as e:
        sys.exit(f"quicperf: {type(e).__name__}: {e}")


if __name__ == "__main__":
    main()
