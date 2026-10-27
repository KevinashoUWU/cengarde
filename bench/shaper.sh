#!/bin/sh
# cake on the tunnel's interface at the rate the engine says (story 013,
# PR 4): the engine sums its links' capacities (capacity.h) and publishes
# shape_mbit in its status; this applies it, so that the queue forms in
# cake (fair between flows, with its AQM) and not in the modems.
#
#   sh shaper.sh -i IFACE [-s STATUS] [-c CLIENT] [-p SECS] [-d PCT] [-m KBIT] [-f]
#
#   -i IFACE   the interface whose egress is shaped: on the router the
#              engine's TUN (cg0: the upload); on the VPS the server's TUN
#              (cg0: the download; one device for every router, so with
#              several the rate is the one -c names)
#   -s STATUS  the engine's status (/var/run/cengarde.json on OpenWrt,
#              /run/cengarde/status.json on the VPS)
#   -c CLIENT  on the VPS: the router's name; its newest session's rate
#   -p SECS    how often (2)
#   -d PCT     a change smaller than this is not applied (10)
#   -m KBIT    never below this (2000)
#   -f         stay in front (no daemon); cake comes off at the end
#
# Reads the status with jsonfilter (OpenWrt) or python3. Needs tc and the
# cake qdisc (kmod-sched-cake on OpenWrt). An experiment for now: the
# measures decide whether it becomes a service of the package.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -u
IFACE='' STATUS='' CLIENT='' PERIOD=2 DELTA=10 MIN=2000
while getopts i:s:c:p:d:m:f o; do
	case $o in
	i) IFACE=$OPTARG ;;
	s) STATUS=$OPTARG ;;
	c) CLIENT=$OPTARG ;;
	p) PERIOD=$OPTARG ;;
	d) DELTA=$OPTARG ;;
	m) MIN=$OPTARG ;;
	f) ;;
	*) echo "see: sh $0" >&2; exit 2 ;;
	esac
done
[ -n "$IFACE" ] || { echo "shaper: -i IFACE" >&2; exit 2; }
if [ -z "$STATUS" ]; then
	for f in /var/run/cengarde.json /run/cengarde/status.json; do
		[ -r "$f" ] && STATUS=$f && break
	done
fi
[ -r "$STATUS" ] || { echo "shaper: no status file" >&2; exit 2; }

# The rate in kbit/s, or nothing.
rate() {
	if [ -z "$CLIENT" ] && command -v jsonfilter >/dev/null; then
		jsonfilter -i "$STATUS" -e '@.upload.shape_mbit' 2>/dev/null
	else
		python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
c = sys.argv[2]
if c:
    s = [x for x in d.get("sessions", []) if x.get("client") == c and x.get("newest")]
    v = s[0].get("shape_mbit") if s else None
else:
    v = d.get("upload", {}).get("shape_mbit")
print("" if v is None else v)' "$STATUS" "$CLIENT" 2>/dev/null
	fi | awk 'NF { printf "%d\n", $1 * 1000 }'
}

applied=0
stop() {
	tc qdisc del dev "$IFACE" root 2>/dev/null
	echo "shaper: cake off $IFACE"
	exit 0
}
trap stop INT TERM
while :; do
	k=$(rate)
	if [ -n "$k" ]; then
		[ "$k" -lt "$MIN" ] && k=$MIN
		# applied once, or moved more than DELTA %
		if [ "$applied" = 0 ] || [ $((k * 100)) -gt $((applied * (100 + DELTA))) ] ||
			[ $((k * 100)) -lt $((applied * (100 - DELTA))) ]; then
			if tc qdisc replace dev "$IFACE" root cake bandwidth "${k}kbit" 2>/dev/null; then
				echo "$(date +%T) shaper: $IFACE cake ${k}kbit"
				applied=$k
			fi
		fi
	fi
	sleep "$PERIOD"
done
