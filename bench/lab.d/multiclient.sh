# shellcheck shell=bash disable=SC2034 # LAB_CI, ENGINE and NLINKS are read by lab.sh
# Several routers on one server: "bench/lab.sh multiclient".
#
# Three client engines in the cli namespace, each a router with links of its
# own: alpha (l1, l2), bravo (l3, l4) and charlie (l5), whose key has the
# same client hint as alpha's, so the server tries both keys on its probes
# (engine/src/clients.h). One server with a [client NAME] section each, and
# a fake WireGuard (udpgen) per router on srv. Checks:
# - every router live on all its links within MC_LIVE_S (3) seconds, with
#   one session each, and alpha's and charlie's hints the same;
# - 1000 pps up, then down, for the three at once: each router's packets
#   reach its own WireGuard exactly once, none another's;
# - IP pass and the forward table: alpha asks and holds the whole range,
#   bravo asks too and waits, alpha lets it go and bravo takes it; alpha's
#   forward rules are in the table all along;
# - reloads that leave alpha alone (its session, and 1000 pps up with
#   nothing lost): bravo disabled (refused: vps_refusing) and enabled again,
#   charlie removed (its session closed, its probes no longer verify);
# - "cengarde ctl links" with a CLIENT column.
LAB_CI=1

MC_LIVE_S=${MC_LIVE_S:-3}
MC_PPS=${MC_PPS:-1000}
# alpha's key is the lab's; charlie's (sha256 of "cengarde lab charlie 182")
# was picked for its client hint, alpha's (e2).
MC_KEYS="alpha=$KEY bravo=ZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5/gIGCg4Q= charlie=AlbCIRHlyMZZ7TuRdu05c8OkgeGanpqawJ15fifXF0o="
MC_ROUTERS="alpha bravo charlie"

# mc_n NAME: the router's number: 1, 2 or 3.
mc_n() { case $1 in alpha) echo 1 ;; bravo) echo 2 ;; *) echo 3 ;; esac; }
mc_key() { echo "$MC_KEYS" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
mc_links() { case $1 in alpha) echo "1 2" ;; bravo) echo "3 4" ;; *) echo 5 ;; esac; }

# mc_conf NAME: router NAME's configuration: its links, its WireGuard side
# on 127.0.0.1:594N1.
mc_conf() {
	local name=$1 n i
	n=$(mc_n "$name")
	cat <<-EOF
		mode = client
		key = $(mc_key "$name")
		listen = 127.0.0.1:594${n}1
		server = 10.0.1.2:59402
		interfaces = none
		status_file = $RUN/$name.json
		control_socket = $RUN/$name.sock
		status_interval_ms = 100
	EOF
	for i in 1 2 3 4 5; do
		printf '[link l%s]\nserver = 10.0.%s.2:59402\nenabled = %s\n' "$i" "$i" \
			"$(case " $(mc_links "$name") " in *" $i "*) echo yes ;; *) echo no ;; esac)"
	done
}

mc_server_conf() {
	cat <<-EOF
		mode = server
		listen = *:59402
		status_file = $RUN/server.json
		control_socket = $RUN/server.sock
		status_interval_ms = 100
		forward_file = $RUN/forward
		[client alpha]
		key = $(mc_key alpha)
		wireguard = 127.0.0.1:59301
		forward = tcp:9000=22 udp:5000-5010
		[client bravo]
		key = $(mc_key bravo)
		wireguard = 127.0.0.1:59302
		[client charlie]
		key = $(mc_key charlie)
		wireguard = 127.0.0.1:59303
	EOF
}

mc_start() {
	local name
	ip netns exec srv "$CENGARDE_BIN" -c "$RUN/server.conf" >"$RUN/server.log" 2>&1 &
	echo $! >"$RUN/server.pid"
	for name in $MC_ROUTERS; do
		ip netns exec cli "$CENGARDE_BIN" -c "$RUN/$name.conf" >"$RUN/$name.log" 2>&1 &
		echo $! >"$RUN/$name.pid"
	done
}

mc_stop() {
	local name
	for name in $MC_ROUTERS; do
		[ -f "$RUN/$name.pid" ] && kill "$(cat "$RUN/$name.pid")" 2>/dev/null
		rm -f "$RUN/$name.pid"
	done
	stop
}

mc_teardown() {
	mc_stop
	teardown
}

# mc_py CHECK [ARGS]: checks on the status of every end, read from the
# control sockets. Exit 0 when the check holds.
#   live LIMIT ROUTERS     each of ROUTERS live on all its links, on both
#                          ends, with one session, within LIMIT s;
#   refused LIMIT NAME     the server refuses NAME (vps_refusing) and holds
#                          no session of it, within LIMIT s;
#   session NAME           NAME's session id on the server;
#   hints                  alpha's and charlie's hints, which must be equal;
#   table LIMIT TEXT       the forward table is TEXT ("\n" for newlines)
#                          within LIMIT s;
#   told LIMIT NAME PASS   NAME's replies say IP pass PASS (on, off) within
#                          LIMIT s: the next reply after a change does.
mc_py() {
	python3 - "$RUN" "$@" <<'EOF'
import json, socket, sys, time

run, check, args = sys.argv[1], sys.argv[2], sys.argv[3:]
LINKS = {"alpha": ["l1", "l2"], "bravo": ["l3", "l4"], "charlie": ["l5"]}


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


def client(srv, name):
    return next((c for c in (srv or {}).get("clients", []) if c["name"] == name), None)


def problems(names):
    srv, out = status("server"), []
    if not srv:
        return ["no status from the server"]
    for name in names:
        cli = status(name)
        if not cli:
            out.append(f"no status from {name}")
            continue
        out += [f"{name} {l['name']} {l['state']}" for l in cli["links"]
                if l["name"] in LINKS[name] and l["state"] != "live"]
        sess = [s for s in srv["sessions"] if s["client"] == name]
        if len(sess) != 1:
            out.append(f"{name} has {len(sess)} sessions on the server")
            continue
        ids = {"l" + p["address"].split(":")[0].split(".")[2] for p in sess[0]["links"] if p["state"] == "live"}
        out += [f"server: {name} {l} not live" for l in LINKS[name] if l not in ids]
    return out


def wait(limit, fn):
    t0, last = time.time(), "?"
    while True:
        last = fn()
        if not last or time.time() - t0 > limit:
            return time.time() - t0, last
        time.sleep(0.1)


if check == "live":
    took, late = wait(float(args[0]), lambda: problems(args[1:]))
    if late:
        print(f"not live after {took:.1f} s: {', '.join(late)}")
        sys.exit(1)
    print(f"{' '.join(args[1:])} live on both ends after {took:.1f} s")
elif check == "refused":
    name = args[1]

    def why():
        cli, c = status(name) or {}, client(status("server"), name)
        out = []
        if not cli.get("vps_refusing"):
            out.append(f"{name} does not see a refusal")
        if not c or c["sessions"] or c["enabled"]:
            out.append(f"server: {name} {c and ('enabled' if c['enabled'] else 'disabled')}, "
                       f"{c and c['sessions']} sessions")
        return out
    took, late = wait(float(args[0]), why)
    print(f"after {took:.1f} s: " + (", ".join(late) if late else f"{name} refused, no session"))
    sys.exit(1 if late else 0)
elif check == "session":
    srv = status("server") or {}
    print(next((s["id"] for s in srv.get("sessions", []) if s["client"] == args[0]), "none"))
elif check == "hints":
    srv = status("server")
    a, c = client(srv, "alpha"), client(srv, "charlie")
    print(f"hints: alpha {a and a['hint']}, charlie {c and c['hint']}")
    sys.exit(0 if a and c and a["hint"] == c["hint"] else 1)
elif check == "told":
    name, want = args[1], args[2]
    took, late = wait(float(args[0]), lambda: [] if ((status(name) or {}).get("passthrough") or {}).get("server")
                      == want else ["not yet"])
    print(f"{name} told IP pass {want}" + (" not" if late else "") + f" after {took:.1f} s")
    sys.exit(1 if late else 0)
elif check == "table":
    want = args[1].replace("\\n", "\n")

    def differs():
        try:
            return [] if open(f"{run}/forward").read() == want else ["differs"]
        except OSError:
            return ["missing"]
    took, late = wait(float(args[0]), differs)
    try:
        got = open(f"{run}/forward").read()
    except OSError:
        got = "(none)\n"
    print(f"after {took:.1f} s:\n" + "".join("     " + l + "\n" for l in got.splitlines()), end="")
    sys.exit(1 if late else 0)
EOF
}

# mc_traffic DIR [ROUTERS]: MC_PPS for 3 s up or down for ROUTERS (all) at
# once; each router's packets reach its own end exactly once.
mc_traffic() {
	local dir=$1 names=${2:-$MC_ROUTERS} name n fail=0 sent uniq dup pids=""
	for name in $names; do
		n=$(mc_n "$name")
		if [ "$dir" = up ]; then
			ip netns exec srv "$BIN/udpgen" -b "127.0.0.1:5930$n" -d 3 -g 3 >"$RUN/rx-$name.out" &
		else
			ip netns exec cli "$BIN/udpgen" -b "127.0.0.1:5000$n" -p "127.0.0.1:594${n}1" -r 20 -s 64 -d 5 -g 2 \
				>"$RUN/rx-$name.out" &
		fi
		pids="$pids $!"
	done
	sleep 1
	for name in $names; do
		n=$(mc_n "$name")
		if [ "$dir" = up ]; then
			ip netns exec cli "$BIN/udpgen" -b "127.0.0.1:5000$n" -p "127.0.0.1:594${n}1" -r "$MC_PPS" -s "$SIZE" \
				-d 3 -g 1 >"$RUN/tx-$name.out" &
		else
			ip netns exec srv "$BIN/udpgen" -b "127.0.0.1:5930$n" -l -r "$MC_PPS" -s "$SIZE" -d 3 -g 1 \
				>"$RUN/tx-$name.out" &
		fi
		pids="$pids $!"
	done
	# shellcheck disable=SC2086 # one pid each
	wait $pids
	for name in $names; do
		sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx-$name.out")
		uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx-$name.out")
		dup=$(sed -n 's/.*dup=\([0-9]*\).*/\1/p' "$RUN/rx-$name.out")
		echo "   $dir $name: sent=$sent uniq=$uniq dup=$dup"
		# Another router's packets would show as duplicates (every udpgen
		# counts from 0) or as more than were sent.
		if [ -z "$sent" ] || [ -z "$uniq" ] || [ $((uniq * 1000)) -lt $((sent * 995)) ] || [ "$uniq" -gt "$sent" ] ||
			[ "$dup" != 0 ]; then
			echo "FAIL: $dir $name"
			fail=1
		fi
	done
	return "$fail"
}

# mc_hup FILE: reload the engine of FILE.conf (SIGHUP).
mc_hup() { kill -HUP "$(cat "$RUN/$1.pid")"; }

multiclient() {
	local fail=0 name out sess traffic sent uniq
	ENGINE=c NLINKS=5
	trap mc_teardown EXIT
	trap 'mc_teardown; exit 130' INT TERM
	setup || return 1
	rm -f "$RUN/forward"
	mc_server_conf >"$RUN/server.conf"
	for name in $MC_ROUTERS; do
		mc_conf "$name" >"$RUN/$name.conf"
	done
	sed -i '1a passthrough = yes' "$RUN/alpha.conf"
	mc_start

	out=$(mc_py live "$MC_LIVE_S" alpha bravo charlie) || fail=1
	echo "   $out"
	out=$(mc_py hints) || { echo "FAIL: alpha's and charlie's hints differ: the lab's keys changed?"; fail=1; }
	echo "   $out"
	if [ "$fail" != 0 ]; then
		echo "--- server log"
		cat "$RUN/server.log"
		mc_teardown
		echo "multiclient: FAILED"
		return 1
	fi
	"$CENGARDE_BIN" ctl -s "$RUN/server.sock" links >"$RUN/links.txt"
	sed 's/^/   /' "$RUN/links.txt"
	head -n 1 "$RUN/links.txt" | grep -q '^CLIENT ' || { echo "FAIL: no CLIENT column in ctl links"; fail=1; }
	for name in $MC_ROUTERS; do
		grep -q "^$name " "$RUN/links.txt" || { echo "FAIL: $name not in ctl links"; fail=1; }
	done

	mc_traffic up || fail=1
	mc_traffic down || fail=1

	# IP pass: alpha holds it; bravo asks and waits; alpha lets it go.
	local rules='rule alpha tcp 9000 9000 22\nrule alpha udp 5000 5010 5000\n'
	local head='# cengarde forward table 1, written by the engine: data only\n'
	out=$(mc_py table 3 "${head}pass alpha\n$rules") || { echo "FAIL: alpha does not hold IP pass"; fail=1; }
	echo "   alpha asks: table $out"
	sed -i '1a passthrough = yes' "$RUN/bravo.conf"
	mc_hup bravo
	sleep 2
	out=$(mc_py table 0 "${head}pass alpha\n$rules") || { echo "FAIL: bravo took IP pass from alpha"; fail=1; }
	grep -q "bravo: asks for IP pass on, which alpha holds" "$RUN/server.log" ||
		{ echo "FAIL: the server log does not say bravo waits"; fail=1; }
	echo "   bravo asks too: unchanged"
	sed -i 's/^passthrough = yes$/passthrough = no/' "$RUN/alpha.conf"
	mc_hup alpha
	out=$(mc_py table 3 "${head}pass bravo\n$rules") || { echo "FAIL: bravo did not get IP pass"; fail=1; }
	echo "   alpha lets it go: table $out"
	out=$(mc_py told 3 bravo on) || fail=1
	echo "   $out"
	out=$(mc_py told 3 alpha off) || fail=1
	echo "   $out"

	# Server reloads under alpha's traffic.
	sess=$(mc_py session alpha)
	ip netns exec srv "$BIN/udpgen" -b 127.0.0.1:59301 -d 9 -g 3 >"$RUN/rx-alpha.out" &
	traffic=$!
	sleep 0.3
	ip netns exec cli "$BIN/udpgen" -b 127.0.0.1:50001 -p 127.0.0.1:59411 -r "$MC_PPS" -s "$SIZE" -d 9 -g 1 \
		>"$RUN/tx-alpha.out" &
	sleep 1
	sed -i '/^\[client bravo\]$/a enabled = no' "$RUN/server.conf"
	mc_hup server
	out=$(mc_py refused 3 bravo) || { echo "FAIL: bravo not refused once disabled"; fail=1; }
	echo "   bravo disabled: $out"
	sed -i '/^enabled = no$/d' "$RUN/server.conf"
	mc_hup server
	out=$(mc_py live 5 bravo) || { echo "FAIL: bravo not back once enabled"; fail=1; }
	echo "   bravo enabled again: $out"
	sed -i '/^\[client charlie\]$/,$d' "$RUN/server.conf"
	mc_hup server
	sleep 1
	grep -q "charlie: session [0-9a-f]* closed: the client was removed" "$RUN/server.log" ||
		{ echo "FAIL: charlie's session not closed when it was removed"; fail=1; }
	wait "$traffic"
	sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx-alpha.out")
	uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx-alpha.out")
	echo "   alpha through the reloads: sent=$sent uniq=$uniq"
	if [ -z "$sent" ] || [ -z "$uniq" ] || [ $((uniq * 1000)) -lt $((sent * 999)) ]; then
		echo "FAIL: alpha lost packets in the reloads"
		fail=1
	fi
	[ "$(mc_py session alpha)" = "$sess" ] || { echo "FAIL: alpha's session changed"; fail=1; }
	! grep -q "restarting" "$RUN/server.log" || { echo "FAIL: a reload restarted the server"; fail=1; }
	grep -hE "client hint|disabled|removed|closed|IP pass|forward table|refuses" \
		"$RUN/server.log" "$RUN/bravo.log" | sed 's/^/   /'
	mc_teardown
	if [ "$fail" = 0 ]; then
		echo "multiclient: ok"
	else
		echo "multiclient: FAILED"
	fi
	return "$fail"
}
