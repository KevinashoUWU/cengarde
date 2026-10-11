# shellcheck shell=bash disable=SC2034 # LAB_CI and ENGINE are read by lab.sh
# The server's socket to WireGuard never takes WireGuard's own port:
# "bench/lab.sh wgport".
#
# The server opens one socket per session to WireGuard, on a port the
# kernel picks from the ephemeral range. While WireGuard is not up, its own
# port is free; if it is in that range (the lab's fake WireGuard listens on
# 59301), the kernel can pick it, and the socket then holds the port
# WireGuard needs and talks to itself: what the client sends up comes back
# down to it. CI hit that once (2026-10-11, the smoke of a package job).
#
# Here the server's namespace has only 59301 and 59302 to pick from, and
# the server and the client start ROUNDS times (6), with WireGuard not up
# yet: every session has to take 59302, and data has to flow both ways.
# Without the guard in wg_socket (server.c) a round fails half the time.
LAB_CI=1

wgport() {
	local fail=0 fired=0 r i dir sent uniq dup
	ENGINE=c
	setup || return 1
	ip netns exec srv sysctl -qw net.ipv4.ip_local_port_range="59301 59302" || {
		echo "FAIL: cannot narrow the ephemeral range"
		teardown
		return 1
	}
	for r in $(seq 1 "${ROUNDS:-6}"); do
		start || { fail=1; break; }
		for i in $(seq 1 50); do
			grep -q "new client" "$RUN/server.log" && break
			sleep 0.1
		done
		if ! grep -q "new client" "$RUN/server.log"; then
			echo "FAIL: round $r: no session"
			fail=1
		elif ip netns exec srv ss -Hlun 'sport = :59301' | grep -q . ||
			ip netns exec srv ss -Hun 'sport = :59301' | grep -q .; then
			echo "FAIL: round $r: the server holds WireGuard's port 59301"
			ip netns exec srv ss -Huanp
			fail=1
		fi
		for dir in up down; do
			"$dir" 2000 1 >/dev/null
			sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$RUN/tx.out")
			uniq=$(sed -n 's/.*uniq=\([0-9]*\).*/\1/p' "$RUN/rx.out")
			dup=$(sed -n 's/.*dup=\([0-9]*\).*/\1/p' "$RUN/rx.out")
			if [ -z "$sent" ] || [ -z "$uniq" ] || [ $((uniq * 1000)) -lt $((sent * 995)) ] || [ "$dup" != 0 ]; then
				echo "FAIL: round $r: $dir: sent=$sent uniq=$uniq dup=$dup"
				fail=1
			fi
		done
		stop
		grep -q "WireGuard's own port" "$RUN/server.log" && fired=$((fired + 1))
		[ "$fail" = 0 ] || break
	done
	if [ "$fail" = 0 ]; then
		# The kernel picks one of the two at random: the guard fires in about
		# half the rounds, and in none only once in 2^ROUNDS runs.
		echo "wgport: ok (${ROUNDS:-6} rounds; the kernel offered WireGuard's port in $fired)"
	else
		echo "--- server log"
		cat "$RUN/server.log"
	fi
	teardown
	return "$fail"
}
