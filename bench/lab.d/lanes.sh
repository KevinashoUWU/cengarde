# shellcheck shell=bash disable=SC2034 # LAB_CI is read by lab.sh
# Server lanes: "bench/lab.sh lanes".
#
# The server listens on a SO_REUSEPORT group of 8 lanes and a junk socket,
# and a BPF program puts each datagram on the lane of its link id
# (engine/src/steer.h). With 3 links, the addresses of multiip (l1 to a
# secondary address, l2 to a /32 on lo, l3 to 10.0.3.2) and lanes = 8:
# - every link live on both ends within 3 s, and each server path's
#   links[].local the address its link sends to: replies leave from the
#   arrival address on every lane;
# - 2000 pps down and up exactly once (dup 0, loss within 0.5 %, as smoke);
# - lanes 0, 1 and 2 received, each from its own link only; lanes 3 to 7
#   and the junk socket received nothing;
# - a short datagram and a protocol 3 one (an older router) land in the junk socket
#   (rx.junk, rx.short, rx.bad_version), not in a lane;
# - a second server on the same port fails to start ("Address already in
#   use") instead of joining the group, and the first keeps its 8 lanes;
# Then lanes = 1: one socket and no junk socket, and smoke's traffic.
LAB_CI=1

# lanes_py CHECK [ARGS]: checks on both ends' status, from their control
# sockets. Exit 0 when the check holds.
#   live T0 LIMIT    every link live on both ends, the locals right, within
#                    LIMIT s of T0 (epoch seconds);
#   lanes JUNK       the lanes as above (8 lanes, 3 links), and JUNK
#                    datagrams in the junk socket;
#   junk BEFORE      rx.junk, short and bad_version grew by 2, 1 and 1 from
#                    BEFORE (a JSON file of the server's status), the lanes
#                    did not;
#   single           one lane, no junk socket.
lanes_py() {
	python3 - "$RUN" "$@" <<'EOF'
import json, socket, sys, time

run, check, args = sys.argv[1], sys.argv[2], sys.argv[3:]
LOCAL = {"l1": "10.0.1.20:59402", "l2": "198.51.100.7:59402", "l3": "10.0.3.2:59402"}


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


def paths(srv):
    for sess in (srv or {}).get("sessions", []):
        if sess["newest"]:
            return {"l" + p["address"].split(":")[0].split(".")[2]: p for p in sess["links"]}
    return {}


def problems():
    cli, srv = status("client"), status("server")
    if not cli or not srv:
        return ["no status from the " + ("client" if not cli else "server")]
    out = [f"client {l['name']} {l['state']}" for l in cli["links"] if l["state"] != "live"]
    p = paths(srv)
    for name, want in LOCAL.items():
        if name not in p:
            out.append(f"server has no path from {name}")
        elif p[name]["state"] != "live":
            out.append(f"server {name} {p[name]['state']}")
        elif p[name]["local"] != want:
            out.append(f"server {name} local {p[name]['local']}, not {want}")
    return out


srv = status("server") or {}
if check == "live":
    t0, limit = float(args[0]), float(args[1])
    late = ["?"]
    while time.time() - t0 < limit + 2:
        late = problems()
        if not late:
            break
        time.sleep(0.1)
    took = time.time() - t0
    print(f"after {took:.1f} s: " + (", ".join(late) if late else "every link live on both ends, locals right"))
    sys.exit(1 if late or took > limit else 0)
elif check == "lanes":
    lanes, junk, bad = srv.get("lanes", []), srv.get("junk"), []
    print("lanes: " + " ".join(f"{l['index']}:rx={l['rx']},links={l['links']},drops={l['drops']}" for l in lanes) +
          f" junk: {junk}")
    print(f"rcvbuf: {srv.get('rcvbuf')} capped={srv.get('rcvbuf_capped')} steering_error={srv.get('steering_error')!r}")
    if len(lanes) != 8:
        bad.append(f"{len(lanes)} lanes, not 8")
    for l in lanes:
        want = [l["index"]] if l["index"] < 3 else []
        if l["links"] != want:
            bad.append(f"lane {l['index']} holds links {l['links']}, not {want}")
        if (l["rx"] > 0) != (l["index"] < 3):
            bad.append(f"lane {l['index']} received {l['rx']}")
    if not junk or junk["rx"] != int(args[0]) or junk["index"] != 8:
        bad.append(f"junk socket {junk}")
    if bad:
        print("FAIL: " + "; ".join(bad))
    sys.exit(1 if bad else 0)
elif check == "junk":
    before, rx = json.load(open(args[0])), srv.get("rx", {})
    got = {k: rx.get(k, 0) - before["rx"].get(k, 0) for k in ("junk", "short", "bad_version")}
    lanes = sum(l["rx"] for l in srv.get("lanes", [])) - sum(l["rx"] for l in before["lanes"])
    print(f"junk socket: {got}, lanes meanwhile: {lanes} datagrams (probes)")
    sys.exit(0 if got == {"junk": 2, "short": 1, "bad_version": 1} and srv["junk"]["rx"] == 2 else 1)
elif check == "single":
    print(f"lanes: {srv.get('lanes')} junk: {srv.get('junk')}")
    sys.exit(0 if len(srv.get("lanes", [])) == 1 and srv.get("junk") is None else 1)
EOF
}

# lanes_traffic DIR: 2000 pps for 3 s, delivered exactly once.
lanes_traffic() {
	local dir=$1 sent uniq dup
	"$dir" 2000 3 >"$RUN/traffic.txt"
	echo "   $dir: $(cat "$RUN/traffic.txt")"
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
	uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	dup=$(sed -n 's/.*dup=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	if [ -z "$sent" ] || [ $((uniq * 1000)) -lt $((sent * 995)) ] || [ "$dup" != 0 ]; then
		echo "FAIL: $dir: sent=$sent uniq=$uniq dup=$dup"
		return 1
	fi
}

# lanes_send WHAT: one datagram from cli to the server's port: "short"
# (10 bytes) or "v3" (a protocol 3 header and payload: an older router).
lanes_send() {
	ip netns exec cli python3 - "$1" <<'EOF'
import socket, sys

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
data = b"x" * 10 if sys.argv[1] == "short" else bytes([0x31, 0, 0, 1]) + bytes(60)
s.sendto(data, ("10.0.3.2", 59402))
EOF
}

lanes() {
	local fail=0 t0 out pid2
	if [ "$(printf %s "$RUN/client.sock" | wc -c)" -gt 107 ]; then
		echo "FAIL: $RUN/client.sock is longer than 107 bytes (sun_path): use a shorter RUN"
		echo "lanes: FAILED"
		return 1
	fi
	ENGINE=c NLINKS=3
	# lanes = 8 last, so that it wins over a lanes in SERVER_EXTRA.
	CLIENT_EXTRA="control_socket = $RUN/client.sock${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	SERVER_EXTRA="control_socket = $RUN/server.sock${SERVER_EXTRA:+;$SERVER_EXTRA};lanes = 8"
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	setup || return 1
	ip -n srv addr add 10.0.1.20/24 dev s1
	ip -n srv addr add 198.51.100.7/32 dev lo
	ip -n cli route add 198.51.100.7/32 via 10.0.2.2 dev l2
	sed -i -e 's/10\.0\.1\.2:59402/10.0.1.20:59402/' -e 's/10\.0\.2\.2:59402/198.51.100.7:59402/' "$RUN/client.conf"
	t0=$(date +%s.%N)
	start
	out=$(lanes_py live "$t0" 3) || fail=1
	echo "   $out"
	[ "$fail" = 0 ] || { echo "FAIL: not every link live with its own local address within 3 s"; cat "$RUN/server.log"; }
	lanes_traffic down || fail=1
	lanes_traffic up || fail=1
	lanes_py lanes 0 >"$RUN/lanes.txt" || fail=1
	sed 's/^/   /' "$RUN/lanes.txt"

	# Junk: a short datagram and a protocol 4 one.
	"$CENGARDE_BIN" ctl -s "$RUN/server.sock" status >"$RUN/server-before.json"
	lanes_send short
	lanes_send v3
	sleep 0.5
	out=$(lanes_py junk "$RUN/server-before.json") || { echo "FAIL: the junk socket did not take them"; fail=1; }
	echo "   $out"
	grep -h "short packet\|protocol v3 packet" "$RUN/server.log" | sed 's/^/   /'

	# A second server on the same port: refused, not a member of the group.
	sed -e "s|$RUN/server.sock|$RUN/server2.sock|" -e "s|$RUN/server.json|$RUN/server2.json|" \
		"$RUN/server.conf" >"$RUN/server2.conf"
	ip netns exec srv "$CENGARDE_BIN" -c "$RUN/server2.conf" >"$RUN/server2.log" 2>&1 &
	pid2=$!
	sleep 1
	if kill -0 "$pid2" 2>/dev/null; then
		echo "FAIL: a second server on the same port is running"
		kill "$pid2"
		fail=1
	fi
	wait "$pid2" 2>/dev/null
	grep -q "Address already in use" "$RUN/server2.log" || { echo "FAIL: the second server did not say why"; fail=1; }
	echo "   second server: $(head -1 "$RUN/server2.log")"
	out=$(lanes_py lanes 2) || { echo "FAIL: the first server's lanes changed"; echo "$out"; fail=1; }
	lanes_traffic down || fail=1
	stop

	# lanes = 1: one socket, as before lanes.
	echo "   lanes = 1"
	SERVER_EXTRA="${SERVER_EXTRA%;lanes = 8};lanes = 1"
	setup && start || return 1
	out=$(lanes_py single) || { echo "FAIL: lanes = 1 is not one socket"; fail=1; }
	echo "   $out"
	lanes_traffic down || fail=1
	lanes_traffic up || fail=1
	stop
	teardown
	[ "$fail" = 0 ] && echo "lanes: ok" || echo "lanes: FAILED"
	return "$fail"
}
