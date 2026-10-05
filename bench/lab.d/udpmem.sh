# shellcheck shell=bash disable=SC2034 # LAB_CI is read by lab.sh
# The server's receive budget against net.ipv4.udp_mem: "bench/lab.sh
# udpmem" (it runs only with LAB_UDPMEM=1, as the CI does).
#
# Every UDP socket of the machine shares udp_mem: past its first value
# the kernel leaves each UDP socket one queued datagram, past its last
# none. The server's lanes multiply its receive buffers, so they share
# half of the pressure threshold (engine/src/rcvbudget.h). Here udp_mem is lowered to UDPMEM
# pages (8192 12288 16384: 64 MiB at most for the whole machine) and put
# back by a trap, the server is stopped with SIGSTOP so that nothing
# drains, and a flood of protocol 3 datagrams on every link id fills every
# lane, while a separate UDP pair exchanges 100 pps. Its receiver reads
# every 250 ms, so it holds a backlog like any busy UDP socket of the
# machine. With 8 lanes at the default 4 MiB (8 MiB once doubled) the
# lanes alone would pass both values:
# - the server reports rcvbuf_capped, with the per-socket value of the
#   budget (24 MiB for 10 sockets);
# - every lane fills, and all the lanes together hold no more than the
#   budget (their receive queues in /proc/net/udp and /proc/net/udp6: with
#   *:59402 the lanes are dual-stack AF_INET6 sockets where the kernel has
#   IPv6, and the kernel lists those in udp6 only);
# - the pair loses nothing;
# - after SIGCONT the tunnel works again: smoke's traffic both ways.
#
# udp_mem is global and visible only in the first network namespace, so a
# server in srv would estimate it from RAM; the server is shown the lowered
# values through a tmpfs over /proc/sys/net/ipv4 in its own mount
# namespace, as it would read them on a VPS. UDPMEM_SHOW shows it other
# values: with the real ones it does not cap, the lanes stop filling at
# udp_mem's first value and the pair loses most of its packets (measured:
# 163 of 599 arrived), which is what the budget prevents.
LAB_CI=1

UDPMEM=${UDPMEM:-8192 12288 16384}
UDPMEM_SHOW=${UDPMEM_SHOW:-$UDPMEM}
UDPMEM_SAVED=

udpmem_restore() {
	[ -n "$UDPMEM_SAVED" ] || return 0
	echo "$UDPMEM_SAVED" >/proc/sys/net/ipv4/udp_mem
	UDPMEM_SAVED=
}

# A stopped server would not see teardown's SIGTERM.
udpmem_cont() {
	[ -f "$RUN/server.pid" ] && kill -CONT "$(cat "$RUN/server.pid")" 2>/dev/null
	return 0
}

# udpmem_py CHECK [ARGS]:
#   capped           the server's status: rcvbuf_capped and its values;
#   full UDP         the server's /proc/net/udp and udp6, one after the
#                    other: every lane full (its receive queue at 90 % of
#                    the effective rcvbuf or more), and all of them within
#                    the budget;
#   live LIMIT       every client link live within LIMIT s.
udpmem_py() {
	python3 - "$RUN" "$@" <<'EOF'
import json, socket, sys, time

run, check, args = sys.argv[1], sys.argv[2], sys.argv[3:]


def status(end):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(1)
    try:
        s.connect(f"{run}/{end}.sock")
        s.sendall(b"status\n")
        buf = b""
        while chunk := s.recv(65536):
            buf += chunk
        return json.loads(buf)
    except (OSError, ValueError):
        return None
    finally:
        s.close()


if check == "capped":
    d = status("server") or {}
    rb = d.get("rcvbuf") or {}
    json.dump(rb, open(f"{run}/rcvbuf.json", "w"))
    print(f"rcvbuf {rb}, rcvbuf_capped {d.get('rcvbuf_capped')}")
    sys.exit(0 if d.get("rcvbuf_capped") and rb.get("budget") else 1)
elif check == "full":
    rb = json.load(open(f"{run}/rcvbuf.json"))
    socks = []  # (receive queue, drops) of every socket on port 59402
    # Both tables have the same columns, and each starts with a header line.
    for line in open(args[0]).read().splitlines():
        f = line.split()
        if len(f) > 12 and f[0] != "sl" and f[1].endswith(f":{59402:04X}"):
            socks.append((int(f[4].split(":")[1], 16), int(f[12])))
    if not socks:
        print("no socket on port 59402 in /proc/net/udp or /proc/net/udp6")
        sys.exit(1)
    held = sum(r for r, _ in socks)
    full = sum(1 for r, _ in socks if r >= rb["effective"] * 0.9)
    print(f"{len(socks)} listen sockets hold {held} bytes (budget {rb['budget']}), {full} of them full "
          f"(effective rcvbuf {rb['effective']}); queues {[r for r, _ in socks]}, drops {[d for _, d in socks]}")
    sys.exit(0 if full >= 8 and held <= rb["budget"] else 1)
elif check == "live":
    t0, limit, late = time.time(), float(args[0]), ["?"]
    while time.time() - t0 < limit:
        d = status("client")
        late = [l["name"] for l in (d or {}).get("links", []) if l["state"] != "live"] if d else ["no status"]
        if not late:
            break
        time.sleep(0.1)
    print(f"after {time.time() - t0:.1f} s: " + (", ".join(late) + " not live" if late else "every link live"))
    sys.exit(1 if late else 0)
EOF
}

# udpmem_pair_rx SECS: receives on 127.0.0.1:59501 in srv for SECS, reading
# every 250 ms; prints "rx=N".
udpmem_pair_rx() {
	ip netns exec srv python3 - "$1" <<'EOF2'
import socket, sys, time

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", 59501))
s.setblocking(False)
n, end = 0, time.time() + float(sys.argv[1])
while time.time() < end:
    time.sleep(0.25)
    while True:
        try:
            s.recv(2048)
            n += 1
        except BlockingIOError:
            break
print(f"rx={n}")
EOF2
}

# udpmem_flood N: N protocol 3 datagrams (1400 bytes) on each link id 0-15,
# from cli to the server's port: every lane gets 2 N.
udpmem_flood() {
	ip netns exec cli python3 - "$1" <<'EOF'
import socket, sys

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
for link in range(16):
    pkt = bytes([0x31, 0, 0, link]) + bytes(1396)
    for _ in range(int(sys.argv[1])):
        try:
            s.sendto(pkt, ("10.0.1.2", 59402))
        except BlockingIOError:
            pass
EOF
}

udpmem() {
	local fail=0 out sp pair_rx sent uniq dir
	if [ "${LAB_UDPMEM:-}" != 1 ]; then
		echo "udpmem: skipped (it lowers net.ipv4.udp_mem for the whole machine; LAB_UDPMEM=1 runs it)"
		return 0
	fi
	if [ "$(printf %s "$RUN/client.sock" | wc -c)" -gt 107 ]; then
		echo "FAIL: $RUN/client.sock is longer than 107 bytes (sun_path): use a shorter RUN"
		echo "udpmem: FAILED"
		return 1
	fi
	ENGINE=c NLINKS=3
	CLIENT_EXTRA="control_socket = $RUN/client.sock${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	SERVER_EXTRA="control_socket = $RUN/server.sock${SERVER_EXTRA:+;$SERVER_EXTRA};lanes = 8"
	trap 'udpmem_restore; udpmem_cont; teardown' EXIT
	trap 'udpmem_restore; udpmem_cont; teardown; exit 130' INT TERM
	setup || return 1
	UDPMEM_SAVED=$(cat /proc/sys/net/ipv4/udp_mem) || return 1
	echo "$UDPMEM" >/proc/sys/net/ipv4/udp_mem || { echo "FAIL: cannot lower net.ipv4.udp_mem"; return 1; }
	echo "   net.ipv4.udp_mem: $UDPMEM_SAVED -> $(cat /proc/sys/net/ipv4/udp_mem)"
	# The server, shown udp_mem in its own mount namespace (ip netns exec
	# makes one); the client as start() runs it.
	# shellcheck disable=SC2016 # expanded by that sh, from its arguments
	ip netns exec srv sh -c 'mount -t tmpfs -o size=16k cg-udpmem /proc/sys/net/ipv4 &&
		echo "$2" >/proc/sys/net/ipv4/udp_mem && exec "$0" -c "$1"' \
		"$CENGARDE_BIN" "$RUN/server.conf" "$UDPMEM_SHOW" >"$RUN/server.log" 2>&1 &
	echo $! >"$RUN/server.pid"
	ip netns exec cli "$CENGARDE_BIN" -c "$RUN/client.conf" >"$RUN/client.log" 2>&1 &
	echo $! >"$RUN/client.pid"
	sleep 1.5
	sp=$(cat "$RUN/server.pid")
	out=$(udpmem_py capped) || { echo "FAIL: the server did not cap its receive buffers"; fail=1; }
	echo "   $out"
	grep -h "rcvbuf:" "$RUN/server.log" | sed 's/^/   /'

	# The pair, 100 pps for 6 s, across the stop and the flood.
	udpmem_pair_rx 8 >"$RUN/pair-rx.out" &
	pair_rx=$!
	sleep 0.5
	ip netns exec srv "$BIN/udpgen" -b 127.0.0.1:59502 -p 127.0.0.1:59501 -r 100 -s 1400 -d 6 -g 1 \
		>"$RUN/pair-tx.out" &
	sleep 1
	kill -STOP "$sp"
	udpmem_flood 1500
	sleep 0.5
	# A kernel without IPv6 has no udp6; cat still prints udp.
	ip netns exec srv cat /proc/net/udp /proc/net/udp6 >"$RUN/udp.txt" 2>/dev/null
	out=$(udpmem_py full "$RUN/udp.txt") || { echo "FAIL: the lanes are not all full, or hold more than the budget"; fail=1; }
	echo "   $out"
	wait "$pair_rx"
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/pair-tx.out")
	uniq=$(sed -n 's/.*rx=\([0-9]*\).*/\1/p' "$RUN/pair-rx.out")
	echo "   the separate pair: sent=$sent received=$uniq"
	[ -n "$sent" ] && [ "$sent" -gt 0 ] && [ "$uniq" = "$sent" ] || { echo "FAIL: the separate UDP pair lost packets"; fail=1; }
	udpmem_restore
	echo "   net.ipv4.udp_mem back to $(cat /proc/sys/net/ipv4/udp_mem)"

	kill -CONT "$sp"
	out=$(udpmem_py live 10) || fail=1
	echo "   SIGCONT: $out"
	for dir in down up; do
		"$dir" 2000 3 >"$RUN/traffic.txt"
		echo "   $dir: $(cat "$RUN/traffic.txt")"
		sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
		uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
		if [ -z "$sent" ] || [ $((uniq * 1000)) -lt $((sent * 995)) ]; then
			echo "FAIL: $dir after SIGCONT: sent=$sent uniq=$uniq"
			fail=1
		fi
	done
	stop
	teardown
	[ "$fail" = 0 ] && echo "udpmem: ok" || echo "udpmem: FAILED"
	return "$fail"
}
