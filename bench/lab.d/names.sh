# shellcheck shell=bash disable=SC2034 # LAB_CI is read by lab.sh
# The server as a name, looked up by the client: "bench/lab.sh names".
#
# The client's "server" is vps.lab:59402, with no per-link server, and the
# client resolves the name itself, through each link, asking that link's
# resolver first (engine/src/resolve.h, names.c). In the lab each link has
# "resolver = 10.0.N.2:5353" (the srv end of the link), the global
# "resolver =" is empty, and bench/fakedns.py runs in srv on 0.0.0.0:5353
# answering vps.lab from a zone file the scenario rewrites; it logs every
# question, so that the scenario can count the lookups.
#
# The server listens on a wildcard and is reached at a /32 on srv's lo,
# 198.51.100.7 and later .8, which cli reaches through every link: one route
# per link with its own metric (the link sockets are bound to their device,
# so each one takes the route of its own device).
#
# a. start: every link live on both ends within NAMES_START_LIMIT s (4); the
#    name is "ok" with the address, answered through one of the links' resolvers.
# b. IP change under 1000 pps both ways, NAMES_FLIPS times (2, .7 to .8 and
#    back, the second one soon after the first: new addresses end a silence,
#    so the next one is looked up at once and not 15 s after the last
#    lookup it asked for): the new /32 is added, the zone changed and the
#    old /32 deleted.
#    The cut is measured from the control sockets (25 ms): the gap in the
#    packets each end delivers to WireGuard around the deletion, and the time
#    until every link is live on the new address at both ends. At most
#    NAMES_CUT_LIMIT s (8); expected about 3: the links' stall detection, 2 s
#    of silence, then a lookup.
# c. DNS down while the server works: the name is DROPped for NAMES_DOWN_S s
#    (40) with a TTL of 30 s: the lookups fail, the tunnel loses nothing (as
#    smoke judges), the name keeps its address (ok, error "timeout").
# d. DNS back, with TTL 0, for NAMES_TTL0_S s (70): at most 3 lookups of each
#    type (the 30 s floor), at least 1 of A, and never two A lookups less than
#    29 s apart; the time the name takes to be asked again is reported.
# e. NXDOMAIN while the server answers: the address is kept and the tunnel
#    unaffected (1000 pps, no loss). Then the address is deleted too: the
#    name ends with no address and the links say "server name not resolved".
#    Then the record and the address come back and every link has to be live
#    within NAMES_RECOVER_S s (30; 0: off): a name left with no address is
#    asked again every 15 s (CG_RS_EMPTY_MS), not after 5 minutes.
LAB_CI=1

NAMES_NAME=vps.lab
NAMES_ADDRS=(198.51.100.7 198.51.100.8)
NAMES_START_LIMIT=${NAMES_START_LIMIT:-4}
NAMES_FLIPS=${NAMES_FLIPS:-2}
NAMES_CUT_LIMIT=${NAMES_CUT_LIMIT:-8}
NAMES_DOWN_S=${NAMES_DOWN_S:-40}
NAMES_TTL0_S=${NAMES_TTL0_S:-70}
NAMES_RECOVER_S=${NAMES_RECOVER_S:-30}

# names_py CHECK [ARGS]: the checks that read both ends from their control
# sockets (server paths are named after the client link they come from).
#   live T0 LIMIT [ADDR]   every link live on both ends (on ADDR) within
#                          LIMIT s of T0 (epoch seconds);
#   flip NAME OLD NEW LIMIT   moves the server from OLD to NEW and times it.
# Exit 0 when the check holds.
names_py() {
	python3 - "$RUN" "$@" <<'EOF'
import json, os, socket, subprocess, sys, time

run, check, args = sys.argv[1], sys.argv[2], sys.argv[3:]
LINKS, PORT = ("l1", "l2", "l3"), 59402


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


def sample():
    """Both ends now, or None. dl: packets the client delivered to WireGuard;
    ul: those the server delivered to its own."""
    cli, srv = status("client"), status("server")
    if not cli or not srv:
        return None
    sess = [s for s in srv.get("sessions", []) if s["newest"]]
    paths = {"l" + p["address"].split(":")[0].split(".")[2]: p for s in sess for p in s["links"]}
    return {"t": time.time(), "dl": cli["download"]["packets"],
            "ul": sum(s["upload_packets"] for s in srv.get("sessions", [])),
            "name": (cli.get("names") or [{}])[0],
            "cli": {l["name"]: l for l in cli["links"]}, "srv": paths}


def live_on(s, addr=None):
    """Every link live at both ends, and on addr:PORT when given."""
    for n in LINKS:
        c, p = s["cli"].get(n), s["srv"].get(n)
        if not c or not p or c["state"] != "live" or p["state"] != "live":
            return False
        if addr and (c["remote"] != f"{addr}:{PORT}" or p["local"] != f"{addr}:{PORT}"):
            return False
    return True


def ip(*a):
    subprocess.run(["ip", "-n", "srv", *a], check=True)


if check == "live":
    t0, limit, addr = float(args[0]), float(args[1]), (args[2] if len(args) > 2 else None)
    s = None
    while time.time() - t0 < limit + 2:
        s = sample()
        if s and live_on(s, addr):
            break
        s = None
        time.sleep(0.05)
    took = time.time() - t0
    print(f"every link live on both ends{' on ' + addr if addr else ''} after {took:.1f} s" if s
          else f"not live after {took:.1f} s")
    sys.exit(0 if s and took <= limit else 1)

elif check == "flip":
    name, old, new, limit = args[0], args[1], args[2], float(args[3])
    samples = []

    def take():
        s = sample()
        if s:
            samples.append(s)
        return s

    for _ in range(25):  # half a second of traffic before
        take()
        time.sleep(0.02)
    ip("addr", "add", new + "/32", "dev", "lo")
    with open(run + "/zone.tmp", "w") as f:
        f.write(f"{name} A {new} 30\n")
    os.replace(run + "/zone.tmp", run + "/zone")
    t_del = time.time()
    ip("addr", "del", old + "/32", "dev", "lo")
    stable = None
    while time.time() - t_del < limit + 5:
        s = take()
        if s and live_on(s, new):
            stable = stable or s["t"]
            if s["t"] - stable >= 1.0:  # a second of flow on the new address
                break
        else:
            stable = None
        time.sleep(0.02)

    def first(pred):
        """Seconds from the deletion to the first sample after it where pred holds."""
        return next((s["t"] - t_del for s in samples if s["t"] >= t_del and pred(s)), None)

    def cut(key):
        """(longest gap in the packets delivered across the deletion, seconds from the deletion to the next
        one), or None when they did not come back by the end of the samples."""
        inc = [b["t"] for a, b in zip(samples, samples[1:]) if b[key] > a[key]]
        if not inc or samples[-1]["t"] - inc[-1] > 0.5 or inc[-1] < t_del:
            return None
        gaps = [(a, b) for a, b in zip(inc, inc[1:]) if b >= t_del]
        a, b = max(gaps, key=lambda g: g[1] - g[0])
        return b - a, b - t_del

    def f(x):
        return "never" if x is None else f"{x:.2f} s"

    n0, n1 = samples[0]["name"], samples[-1]["name"]
    cd, cu = cut("dl"), cut("ul")
    t_stall = first(lambda s: any(s["cli"][n]["state"] != "live" for n in LINKS))
    t_lookup = first(lambda s: s["name"].get("lookups", 0) > n0.get("lookups", 0))
    t_addr = first(lambda s: s["name"].get("addresses") == [new])
    t_cli = first(lambda s: all(s["cli"][n]["state"] == "live" and s["cli"][n]["remote"] == f"{new}:{PORT}" for n in LINKS))
    t_srv = first(lambda s: all(s["srv"].get(n, {}).get("state") == "live" and
                                s["srv"][n]["local"] == f"{new}:{PORT}" for n in LINKS))
    both = None if t_cli is None or t_srv is None else max(t_cli, t_srv)
    print(f"   {old} -> {new}: cut down {f(cd and cd[0])}, up {f(cu and cu[0])} "
          f"(first packets again {f(cd and cd[1])} and {f(cu and cu[1])} after the deletion)")
    print(f"   after the deletion: a link not live {f(t_stall)}, first lookup {f(t_lookup)}, new address {f(t_addr)}, "
          f"live on it: client {f(t_cli)}, server {f(t_srv)}; lookups +{n1.get('lookups', 0) - n0.get('lookups', 0)}, "
          f"via {n1.get('via')}, {len(samples)} samples")
    print(f"LIVE {f(both)}")
    sys.exit(0 if cd and cu and both is not None and both <= limit and max(cd[1], cu[1]) <= limit else 1)
EOF
}

# names_zone LINE...: the zone fakedns serves, replaced in one step (the
# server reads it again when the file changes).
names_zone() {
	printf '%s\n' "$@" >"$RUN/zone.tmp" && mv "$RUN/zone.tmp" "$RUN/zone"
}

# names_stop: fakedns goes away (it runs in srv, which teardown only deletes).
names_stop() {
	[ -f "$RUN/fakedns.pid" ] && kill "$(cat "$RUN/fakedns.pid")" 2>/dev/null
	rm -f "$RUN/fakedns.pid"
	return 0
}

# names_get EXPR: a value of the client's first name (n, in Python) now.
names_get() {
	"$CENGARDE_BIN" ctl -s "$RUN/client.sock" status >"$RUN/client-now.json" 2>/dev/null &&
		jget "$RUN/client-now.json" "(lambda n: $1)(d['names'][0])"
}

# names_row: the first name's state|addresses|error|lookups|failures|next
# lookup in ms|ms since its answer|TTL|via, as one line (with a state of "?"
# when the client does not answer).
names_row() {
	names_get "'|'.join(str(x) for x in (n['state'], ','.join(n['addresses']), n['error'], n['lookups'], n['failures'], n['next_lookup_ms'], n['answered_ms_ago'], n['ttl'], n['via']))" ||
		echo '?||?|0|0|0|0|-|'
}

# names_queries T0 [TYPE]: the lookups fakedns logged since T0 (epoch seconds).
names_queries() {
	awk -v t0="$1" -v ty="${2:-}" '$1 >= t0 && (ty == "" || $3 == ty)' "$RUN/dns.log"
}

# names_traffic SECS: 1000 pps down and up for SECS seconds, in the background;
# names_traffic_wait waits for the end and names_loss reads what came of it.
names_traffic() {
	ip netns exec srv "$BIN/udpgen" -b 127.0.0.1:59301 -l -r 1000 -s "$SIZE" -d "$1" -g 2 >"$RUN/names-srv.out" &
	NAMES_PIDS=("$!")
	ip netns exec cli "$BIN/udpgen" -b 127.0.0.1:50000 -p 127.0.0.1:59401 -r 1000 -s "$SIZE" -d "$1" -g 2 >"$RUN/names-cli.out" &
	NAMES_PIDS+=("$!")
}
names_traffic_wait() {
	wait "${NAMES_PIDS[@]}"
}

# names_loss: sets NAMES_DOWN and NAMES_UP to "SENT LOST DUPLICATES" and
# NAMES_LOSS to a sentence about both, from the two udpgen reports. Fails when
# a direction lost more than 0.5 % or delivered a duplicate (as smoke judges),
# or sent nothing.
names_loss() {
	local us dg dd ds ug ud
	read -r us dg dd < <(sed -n 's/^sent=\([0-9]*\) .* uniq=\([0-9]*\) dup=\([0-9]*\) .*/\1 \2 \3/p' "$RUN/names-cli.out")
	read -r ds ug ud < <(sed -n 's/^sent=\([0-9]*\) .* uniq=\([0-9]*\) dup=\([0-9]*\) .*/\1 \2 \3/p' "$RUN/names-srv.out")
	: "${us:=0}" "${dg:=0}" "${dd:=0}" "${ds:=0}" "${ug:=0}" "${ud:=0}"
	NAMES_DOWN="$ds $((ds - dg)) $dd"
	NAMES_UP="$us $((us - ug)) $ud"
	NAMES_LOSS="down lost $((ds - dg)) of $ds (dup $dd), up lost $((us - ug)) of $us (dup $ud)"
	[ "$ds" -gt 0 ] && [ "$us" -gt 0 ] && [ $((dg * 1000)) -ge $((ds * 995)) ] && [ $((ug * 1000)) -ge $((us * 995)) ] &&
		[ "$dd" = 0 ] && [ "$ud" = 0 ]
}

# names_ms T0: milliseconds since T0 (epoch seconds).
names_ms() {
	awk -v t0="$1" -v now="$(date +%s.%N)" 'BEGIN { printf "%d", (now - t0) * 1000 }'
}

# names_case ID ok|fail TEXT: one line per case, kept for the summary.
names_case() {
	local tag=PASS
	[ "$2" = ok ] || { tag=FAIL && NAMES_FAIL=1; }
	echo "$tag: $1: $3"
	NAMES_SUMMARY+=("$tag: $1: $3")
}

# names_a T0: the start, T0 being the time the engines were started.
names_a() {
	local out row state addrs via
	if out=$(names_py live "$1" "$NAMES_START_LIMIT" "${NAMES_ADDRS[0]}"); then
		row=$(names_row) && IFS='|' read -r state addrs _ _ _ _ _ _ via <<<"$row"
		if [ "$state" = ok ] && [ "$addrs" = "${NAMES_ADDRS[0]}" ] && [[ $via =~ ^l[123]\ 10\.0\.[123]\.2:5353$ ]]; then
			names_case "a start" ok "$out; $NAMES_NAME $state $addrs through '$via'"
		else
			names_case "a start" fail "$out; but the name is '$row'"
		fi
	else
		names_case "a start" fail "$out (limit ${NAMES_START_LIMIT} s)"
	fi
}

# names_b: the server moves to the other address, NAMES_FLIPS times, under
# traffic. names_py flip prints the numbers of each move.
names_b() {
	local i out new rc ok=1 live=""
	names_traffic $((4 + 6 + 14 * (NAMES_FLIPS - 1))) # the first change takes ~4 s, the next ones up to ~13
	sleep 1.5
	for i in $(seq 1 "$NAMES_FLIPS"); do
		new=${NAMES_ADDRS[$((i % 2))]}
		out=$(names_py flip "$NAMES_NAME" "$NAMES_CUR" "$new" "$NAMES_CUT_LIMIT")
		rc=$?
		echo "   change $i:"
		echo "$out" | grep -v '^LIVE'
		live="$live${live:+,} $(echo "$out" | sed -n 's/^LIVE //p')"
		[ "$rc" = 0 ] || ok=0
		NAMES_CUR=$new
		sleep 1.5
	done
	names_traffic_wait
	names_loss
	echo "   tunnel through the changes: $NAMES_LOSS"
	if [ "$ok" = 1 ]; then
		names_case "b IP change" ok "$NAMES_FLIPS changes under 1000 pps, every link live on the new address after${live} (limit ${NAMES_CUT_LIMIT} s); $NAMES_LOSS"
	else
		names_case "b IP change" fail "a cut or a recovery above ${NAMES_CUT_LIMIT} s, or none: live after${live}; $NAMES_LOSS"
	fi
}

# names_c: DNS down while the server works. The zone is not restored here:
# names_d takes it from there.
names_c() {
	local t0 row state addrs err l0 f0 l1 f1 q
	row=$(names_row) && IFS='|' read -r _ _ _ l0 f0 _ <<<"$row"
	names_zone "$NAMES_NAME DROP"
	t0=$(date +%s.%N)
	names_traffic "$NAMES_DOWN_S"
	sleep "$NAMES_DOWN_S"
	row=$(names_row) && IFS='|' read -r state addrs err l1 f1 _ <<<"$row"
	names_traffic_wait
	q=$(names_queries "$t0" | wc -l)
	if names_loss && [ "$state" = ok ] && [ "$addrs" = "$NAMES_CUR" ] && [ "$err" = timeout ] && [ "$f1" -gt "$f0" ] &&
		names_py live "$(date +%s.%N)" 1 "$NAMES_CUR" >/dev/null; then
		names_case "c DNS down" ok "${NAMES_DOWN_S} s dropped: $state $addrs, error '$err', failures +$((f1 - f0)) in lookups +$((l1 - l0)), $q questions reached fakedns; $NAMES_LOSS; every link live"
	else
		names_case "c DNS down" fail "${NAMES_DOWN_S} s dropped: name '$state' '$addrs' error '$err' (want ok, $NAMES_CUR, timeout), failures +$((f1 - f0)), lookups +$((l1 - l0)), $q questions; $NAMES_LOSS"
	fi
}

# names_d: DNS is back, with TTL 0, for NAMES_TTL0_S s.
names_d() {
	local t0 qa qaaaa first gap row ttl err
	t0=$(date +%s.%N)
	names_zone "$NAMES_NAME A $NAMES_CUR 0"
	sleep "$NAMES_TTL0_S"
	qa=$(names_queries "$t0" A | wc -l)
	qaaaa=$(names_queries "$t0" AAAA | wc -l)
	first=$(names_queries "$t0" A | awk -v t0="$t0" 'NR == 1 { printf "%.1f", $1 - t0 }')
	gap=$(names_queries "$t0" A | awk 'NR > 1 && (min == "" || $1 - prev < min) { min = $1 - prev } { prev = $1 } END { print min == "" ? "-" : sprintf("%.1f", min) }')
	row=$(names_row) && IFS='|' read -r _ _ err _ _ _ _ ttl _ <<<"$row"
	if [ "$qa" -ge 1 ] && [ "$qa" -le 3 ] && [ "$qaaaa" -le 3 ] && [ "$err" != timeout ] &&
		{ [ "$gap" = - ] || awk -v g="$gap" 'BEGIN { exit !(g >= 29) }'; }; then
		names_case "d TTL 0" ok "in ${NAMES_TTL0_S} s: $qa A and $qaaaa AAAA questions (3 at most of each), the smallest gap between A questions ${gap} s (29 at least), the first ${first} s after DNS came back, answer TTL ${ttl:-?}, error '${err}'"
	else
		names_case "d TTL 0" fail "in ${NAMES_TTL0_S} s: $qa A (1 to 3 wanted) and $qaaaa AAAA (at most 3) questions, the smallest gap between A questions ${gap} s (29 at least), the first ${first:--} s after DNS came back, error '${err}' (not timeout)"
	fi
}

# names_e: NXDOMAIN while the server answers, then with no server either.
names_e() {
	local row next t0 state addrs err ok=1 got=""
	names_zone "$NAMES_NAME NXDOMAIN"
	t0=$(date +%s.%N)
	row=$(names_row) && IFS='|' read -r _ _ _ _ _ next _ <<<"$row"
	names_traffic $(((${next:-30000} + 999) / 1000 + 4))
	while [ "$(names_ms "$t0")" -lt $((${next:-30000} + 3000)) ]; do
		row=$(names_row) && IFS='|' read -r state addrs err _ <<<"$row"
		[ "${err:-}" = nxdomain ] && { got=$(names_ms "$t0"); break; }
		sleep 0.1
	done
	names_traffic_wait
	names_loss || ok=0
	row=$(names_row) && IFS='|' read -r state addrs err _ <<<"$row"
	if [ "$ok" = 1 ] && [ -n "$got" ] && [ "$state" = ok ] && [ "$addrs" = "$NAMES_CUR" ] &&
		names_py live "$(date +%s.%N)" 1 "$NAMES_CUR" >/dev/null; then
		names_case "e NXDOMAIN" ok "NXDOMAIN came ${got} ms after the change (the lookup was due in ${next} ms): $state $addrs kept, every link live; $NAMES_LOSS"
	else
		names_case "e NXDOMAIN" fail "NXDOMAIN after '${got:-never}' ms (due in ${next} ms): name '$state' '$addrs' error '$err' (want ok, $NAMES_CUR); $NAMES_LOSS"
	fi
	names_e_gone
}

# names_e_gone: NXDOMAIN and no server: nothing left to keep.
names_e_gone() {
	local t0 row state addrs err next reasons="" took
	ip -n srv addr del "$NAMES_CUR/32" dev lo
	t0=$(date +%s.%N)
	while [ "$(names_ms "$t0")" -lt 20000 ]; do
		row=$(names_row) && IFS='|' read -r state _ <<<"$row"
		reasons=$(jget "$RUN/client-now.json" "','.join(sorted(set(l['reason'] for l in d['links'] if l['name'] != 'lo')))")
		[ "$state" != ok ] && [ "$reasons" = "server name not resolved" ] && break
		sleep 0.1
	done
	took=$(names_ms "$t0")
	row=$(names_row) && IFS='|' read -r state addrs err _ _ next _ <<<"$row"
	if { [ "$state" = none ] || [ "$state" = failed ]; } && [ -z "$addrs" ] && [ "$reasons" = "server name not resolved" ]; then
		names_case "e2 NXDOMAIN, no server" ok "${took} ms after the address went: state $state, no address, error '$err', links say '$reasons'; next lookup in ${next} ms"
	else
		names_case "e2 NXDOMAIN, no server" fail "after ${took} ms: state '$state', addresses '$addrs', error '$err', link reasons '$reasons'; next lookup in ${next} ms"
	fi
	[ "$NAMES_RECOVER_S" -gt 0 ] && names_e_back
}

# names_e_back: the server and its record come back; every link has to be
# live again within NAMES_RECOVER_S s.
names_e_back() {
	local out
	ip -n srv addr add "$NAMES_CUR/32" dev lo
	names_zone "$NAMES_NAME A $NAMES_CUR 30"
	if out=$(names_py live "$(date +%s.%N)" "$NAMES_RECOVER_S" "$NAMES_CUR"); then
		names_case "e3 the server comes back" ok "$out after the record and the address came back (limit ${NAMES_RECOVER_S} s)"
	else
		names_case "e3 the server comes back" fail "$out after the record and the address came back (limit ${NAMES_RECOVER_S} s); $(names_row)"
	fi
}

names() {
	local sock="$RUN/client.sock" t0 i a
	NAMES_FAIL=0 NAMES_SUMMARY=() NAMES_CUR=${NAMES_ADDRS[0]}
	if [ "$(printf %s "$sock" | wc -c)" -gt 107 ]; then
		echo "FAIL: $sock is longer than 107 bytes (sun_path): use a shorter RUN"
		echo "names: FAILED"
		return 1
	fi
	ENGINE=c NLINKS=3
	CLIENT_EXTRA="control_socket = $sock;resolver =${CLIENT_EXTRA:+;$CLIENT_EXTRA}"
	SERVER_EXTRA="control_socket = $RUN/server.sock${SERVER_EXTRA:+;$SERVER_EXTRA}"
	trap 'names_stop; teardown' EXIT
	trap 'names_stop; teardown; exit 130' INT TERM
	setup || return 1
	# The server is a name and no link has its own entry; each link asks the
	# srv end of its own veth.
	sed -i -e "s/^server = .*/server = $NAMES_NAME:59402/" -e '/^\[link /,$ { /^server = /d }' "$RUN/client.conf"
	for i in 1 2 3; do
		sed -i "/^\[link l$i\]/a resolver = 10.0.$i.2:5353" "$RUN/client.conf"
	done
	for a in "${NAMES_ADDRS[@]}"; do
		for i in 1 2 3; do
			ip -n cli route add "$a/32" via "10.0.$i.2" dev "l$i" metric "$i"
		done
	done
	ip -n srv addr add "$NAMES_CUR/32" dev lo
	names_zone "$NAMES_NAME A $NAMES_CUR 30"
	: >"$RUN/dns.log"
	FAKEDNS_LOG="$RUN/dns.log" ip netns exec srv python3 -I "$LAB/fakedns.py" "$RUN/zone" 5353 0.0.0.0 >"$RUN/fakedns.log" 2>&1 &
	echo $! >"$RUN/fakedns.pid"
	sleep 0.5
	t0=$(date +%s.%N)
	start
	names_a "$t0"
	if [ "$NAMES_FAIL" = 0 ]; then # nothing else to say without a tunnel
		names_b
		names_c
		names_d
		names_e
	fi
	grep -h "server name" "$RUN/client.log" | sed 's/^/   log: /' | tail -n 30
	if [ "$NAMES_FAIL" != 0 ]; then
		echo "--- client log"
		tail -n 30 "$RUN/client.log"
		echo "--- fakedns log"
		cat "$RUN/fakedns.log"
	fi
	echo "--- names summary"
	printf '%s\n' "${NAMES_SUMMARY[@]}"
	names_stop
	stop
	teardown
	[ "$NAMES_FAIL" = 0 ] && echo "names: ok" || echo "names: FAILED"
	return "$NAMES_FAIL"
}
