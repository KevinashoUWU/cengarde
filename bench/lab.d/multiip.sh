# shellcheck shell=bash disable=SC2034 # LAB_CI is read by lab.sh
# Several server addresses: "bench/lab.sh multiip".
#
# The server listens on a wildcard and replies from the address each packet
# arrived at (engine/src/pktinfo.h). srv gets a secondary address on s1,
# 10.0.1.20/24, and 198.51.100.7/32 on lo, which cli reaches through l2:
# l1 sends to the secondary, l2 to the /32 and l3 to 10.0.3.2 as always.
# Replies from the address the route picks (10.0.1.2 and 10.0.2.2) would
# never get into the client's link sockets, which are connected to the
# address they send to.
#
# With listen = 0.0.0.0:59402 and then *:59402 (dual-stack, IPv4 arrivals as
# v4-mapped addresses, where the kernel has IPv6; IPv4 otherwise):
# - every link live on both ends within 3 s of the start, and each server
#   path's links[].local the address its link sends to (also in the LOCAL
#   column of "cengarde ctl links");
# - 2000 pps down and up exactly once (dup 0, loss within 0.5 %, as smoke),
#   every link carrying at least 90 % of the packets;
# - the /32 deleted while 2000 pps flow down: only l2's local_errors grow,
#   its path stays, and the tunnel loses nothing through l1 and l3; with the
#   /32 back, l2 is live again within 5 s.
# Then, with the client's address list: l2 with "server = 198.51.100.7:59402
# 10.0.2.2:59402" and server_failover_ms = 3000 is back on the second
# address within 4 s of the /32's deletion, and the server's path follows it
# there (links[].local 10.0.2.2).
LAB_CI=1

# multiip_py CHECK [ARGS]: the checks that read the status of both ends from
# their control sockets. Server paths are named after the client link they
# come from (10.0.N.1 is lN). Exit 0 when the check holds.
#   live T0 LIMIT    every link live on both ends (and the locals right)
#                    within LIMIT s of T0 (epoch seconds);
#   counts           packets received per link so far, as JSON;
#   spread FILE DIR SENT   each link received 90 % of SENT since FILE;
#   errors           only l2 has local_errors, and its path is still there;
#   moved T0 LIMIT   l2 live on 10.0.2.2 on both ends within LIMIT s of T0.
multiip_py() {
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
    """The server's paths of the newest session, by client link name."""
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
    for name in LOCAL:
        if name not in p:
            out.append(f"server has no path from {name}")
        elif p[name]["state"] != "live":
            out.append(f"server {name} {p[name]['state']}")
    return out


def counts():
    cli, srv = status("client") or {}, paths(status("server"))
    down = {l["name"]: l["rx_first"] + l["rx_duplicate"] for l in cli.get("links", [])}
    up = {n: p["rx_first"] + p["rx_duplicate"] for n, p in srv.items()}
    return {"down": down, "up": up}


if check == "live":
    t0, limit = float(args[0]), float(args[1])
    late = ["?"]
    while time.time() - t0 < limit + 2:
        late = problems()
        if not late:
            break
        time.sleep(0.1)
    took = time.time() - t0
    if late:
        print(f"not live after {took:.1f} s: {', '.join(late)}")
        sys.exit(1)
    p, wrong = paths(status("server")), []
    for name, want in LOCAL.items():
        if p[name]["local"] != want:
            wrong.append(f"{name} local {p[name]['local']}, not {want}")
    print(f"every link live on both ends after {took:.1f} s; locals: " +
          " ".join(f"{n}={p[n]['local']}" for n in sorted(p)))
    if wrong:
        print("FAIL: " + "; ".join(wrong))
    sys.exit(1 if wrong or took > limit else 0)
elif check == "counts":
    print(json.dumps(counts()))
elif check == "spread":
    before, d, sent = json.load(open(args[0]))[args[1]], args[1], int(args[2])
    now = counts()[d]
    got = {n: now.get(n, 0) - before.get(n, 0) for n in LOCAL}
    print(f"{d} per link: " + " ".join(f"{n}={got[n]}" for n in sorted(got)))
    sys.exit(0 if all(v * 10 >= sent * 9 for v in got.values()) else 1)
elif check == "moved":
    t0, limit, want = float(args[0]), float(args[1]), "10.0.2.2:59402"
    seen = "?"
    while time.time() - t0 < limit + 2:
        cli, p = status("client") or {}, paths(status("server"))
        l2 = next((l for l in cli.get("links", []) if l["name"] == "l2"), {})
        seen = (f"client {l2.get('state')} to {l2.get('remote')} (candidate {l2.get('candidate')}, "
                f"{l2.get('failovers')} failovers), server local {p.get('l2', {}).get('local')}")
        if l2.get("state") == "live" and l2.get("remote") == want and p.get("l2", {}).get("local") == want:
            break
        time.sleep(0.1)
    took = time.time() - t0
    print(f"after {took:.1f} s: {seen}")
    sys.exit(0 if took <= limit else 1)
elif check == "errors":
    p = paths(status("server"))
    errs = {n: p[n]["local_errors"] if n in p else None for n in LOCAL}
    print("local_errors: " + " ".join(f"{n}={errs[n]}" for n in sorted(errs)))
    sys.exit(0 if errs["l2"] and errs["l1"] == 0 and errs["l3"] == 0 else 1)
EOF
}

# multiip_traffic DIR: 2000 pps for 3 s, delivered exactly once and spread
# over every link.
multiip_traffic() {
	local dir=$1 sent uniq dup out fail=0
	multiip_py counts >"$RUN/counts.json"
	"$dir" 2000 3 >"$RUN/traffic.txt"
	echo "   $dir: $(cat "$RUN/traffic.txt")"
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
	uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	dup=$(sed -n 's/.*dup=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	if [ -z "$sent" ] || [ $((uniq * 1000)) -lt $((sent * 995)) ] || [ "$dup" != 0 ]; then
		echo "FAIL: $dir: sent=$sent uniq=$uniq dup=$dup"
		fail=1
	fi
	out=$(multiip_py spread "$RUN/counts.json" "$dir" "${sent:-0}") || { echo "FAIL: $dir not over every link"; fail=1; }
	echo "   $out"
	return "$fail"
}

# multiip_run LISTEN: one pass, with the server on LISTEN.
multiip_run() {
	local listen=$1 fail=0 t0 out sent uniq dup traffic
	echo "   listen = $listen"
	setup || return 1
	ip -n srv addr add 10.0.1.20/24 dev s1
	ip -n srv addr add 198.51.100.7/32 dev lo
	ip -n cli route add 198.51.100.7/32 via 10.0.2.2 dev l2
	sed -i -e 's/10\.0\.1\.2:59402/10.0.1.20:59402/' -e 's/10\.0\.2\.2:59402/198.51.100.7:59402/' "$RUN/client.conf"
	sed -i "s/^listen = .*/listen = $listen/" "$RUN/server.conf"
	t0=$(date +%s.%N)
	start
	if ! out=$(multiip_py live "$t0" 3); then
		echo "   $out"
		echo "FAIL: not every link live with its own local address within 3 s"
		echo "--- server log"
		cat "$RUN/server.log"
		stop
		return 1
	fi
	echo "   $out"
	"$CENGARDE_BIN" ctl -s "$RUN/server.sock" status >"$RUN/server-now.json"
	[ "$(jget "$RUN/server-now.json" 'd["reply_from_arrival"]')" = True ] ||
		{ echo "FAIL: reply_from_arrival is not true"; fail=1; }
	echo "   bound $(jget "$RUN/server-now.json" 'd["listen"]')"
	"$CENGARDE_BIN" ctl -s "$RUN/server.sock" links >"$RUN/links.txt"
	sed 's/^/   /' "$RUN/links.txt"
	grep -Eq ' 198\.51\.100\.7:59402$' "$RUN/links.txt" || { echo "FAIL: no LOCAL 198.51.100.7 in ctl links"; fail=1; }
	multiip_traffic down || fail=1
	multiip_traffic up || fail=1

	# The /32 goes away under traffic.
	down 2000 6 >"$RUN/traffic.txt" &
	traffic=$!
	sleep 2.5
	ip -n srv addr del 198.51.100.7/32 dev lo
	wait "$traffic"
	echo "   down, /32 deleted after 1.5 s: $(cat "$RUN/traffic.txt")"
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
	uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	dup=$(sed -n 's/.*dup=\([0-9]*\).*/\1/p' "$RUN/rx.out")
	if [ -z "$sent" ] || [ $((uniq * 1000)) -lt $((sent * 995)) ] || [ "$dup" != 0 ]; then
		echo "FAIL: the tunnel lost packets when the /32 went away: sent=$sent uniq=$uniq dup=$dup"
		fail=1
	fi
	out=$(multiip_py errors) || { echo "FAIL: not only l2's local_errors grew, or l2's path is gone"; fail=1; }
	echo "   $out"
	grep -h "address removed or no route" "$RUN/server.log" | sed 's/^/   /'
	ip -n srv addr add 198.51.100.7/32 dev lo
	out=$(multiip_py live "$(date +%s.%N)" 5) || { echo "FAIL: l2 not back with the /32"; fail=1; }
	echo "   /32 back: $out"
	stop
	return "$fail"
}

# multiip_failover: l2 lists the /32 and then 10.0.2.2, and moves to the
# second when the first goes away.
multiip_failover() {
	local fail=0 out
	echo "   failover: l2 lists 198.51.100.7 and then 10.0.2.2"
	setup || return 1
	ip -n srv addr add 10.0.1.20/24 dev s1
	ip -n srv addr add 198.51.100.7/32 dev lo
	ip -n cli route add 198.51.100.7/32 via 10.0.2.2 dev l2
	sed -i -e 's/10\.0\.1\.2:59402/10.0.1.20:59402/' \
		-e 's/10\.0\.2\.2:59402/198.51.100.7:59402 10.0.2.2:59402/' "$RUN/client.conf"
	sed -i '1a server_failover_ms = 3000' "$RUN/client.conf"
	sed -i "s/^listen = .*/listen = *:59402/" "$RUN/server.conf"
	start
	out=$(multiip_py live "$(date +%s.%N)" 3) || fail=1
	echo "   $out"
	ip -n srv addr del 198.51.100.7/32 dev lo
	out=$(multiip_py moved "$(date +%s.%N)" 4) || { echo "FAIL: l2 not on 10.0.2.2 within 4 s"; fail=1; }
	echo "   /32 deleted: $out"
	grep -h "no reply from 198.51.100.7" "$RUN/client.log" | sed 's/^/   /'
	stop
	return "$fail"
}

multiip() {
	local fail=0 listen
	if [ "$(printf %s "$RUN/client.sock" | wc -c)" -gt 107 ]; then
		echo "FAIL: $RUN/client.sock is longer than 107 bytes (sun_path): use a shorter RUN"
		echo "multiip: FAILED"
		return 1
	fi
	ENGINE=c NLINKS=3
	CLIENT_EXTRA="control_socket = $RUN/client.sock${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	SERVER_EXTRA="control_socket = $RUN/server.sock${SERVER_EXTRA:+;$SERVER_EXTRA}"
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	for listen in "0.0.0.0:59402" "*:59402"; do
		multiip_run "$listen" || fail=1
		teardown
	done
	multiip_failover || fail=1
	teardown
	[ "$fail" = 0 ] && echo "multiip: ok" || echo "multiip: FAILED"
	return "$fail"
}
