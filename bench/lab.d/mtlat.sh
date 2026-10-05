# shellcheck shell=bash disable=SC2034 # ENGINE and NLINKS are read by lab.sh
# Latency and CPU of the client's link_threads modes: "bench/lab.sh mtlat"
# (by hand; PR 3b, docs/historias/011).
#
# For each of MTLAT_RUNS (5) runs, interleaved, each mode of MTLAT_MODES
# ("legacy off on on+busy": on+busy is on with busy_poll_us = 50 on the
# client) carries MTLAT_RATES (2000 20000 80000) packets per second for
# MTLAT_S (5) seconds in each of MTLAT_DIRS (down up), 3 links. Each line
# is lab.sh's report (loss, p50/p99 end to end, CPU per packet of each end,
# from /proc: every thread of the process), and for the download the
# engine's own hop stamps: how long a batch waited between the thread that
# read it and the hub that took it (link_threads off and on; p50 and p99 of
# the link whose p50 is highest, over the last 5 s). The end prints the
# median of each point over the runs. Reported, not judged: in this VM p50
# moves by about 30 us between identical runs.
MTLAT_RUNS=${MTLAT_RUNS:-5}
MTLAT_MODES=${MTLAT_MODES:-legacy off on on+busy}
MTLAT_RATES=${MTLAT_RATES:-2000 20000 80000}
MTLAT_DIRS=${MTLAT_DIRS:-down up}
MTLAT_S=${MTLAT_S:-5}

mtlat() {
	local r mode rate dir hop line extra out=$RUN/mtlat.txt
	ENGINE=c NLINKS=3
	trap teardown EXIT
	trap 'teardown; exit 130' INT TERM
	mkdir -p "$RUN"
	: >"$out"
	for r in $(seq 1 "$MTLAT_RUNS"); do
		for mode in $MTLAT_MODES; do
			extra="link_threads = ${mode%+busy};control_socket = $RUN/client.sock"
			[ "$mode" = "${mode%+busy}" ] || extra="$extra;busy_poll_us = 50"
			CLIENT_EXTRA=$extra
			setup && start || return 1
			for rate in $MTLAT_RATES; do
				for dir in $MTLAT_DIRS; do
					hop=""
					line=$("$dir" "$rate" "$MTLAT_S")
					if [ "$dir" = down ] && [ "${mode%+busy}" != legacy ]; then
						"$CENGARDE_BIN" ctl -s "$RUN/client.sock" status >"$RUN/client-now.json"
						hop=$(jget "$RUN/client-now.json" \
							'max((l["hop_us"]["down"] or {"p50": -1, "p99": -1} for l in d["links"]), key=lambda h: h["p50"])')
						hop=" hop=$hop"
					fi
					echo "run=$r mode=$mode dir=$dir $line$hop" | tee -a "$out"
				done
			done
			stop
		done
	done
	teardown
	echo "## medians over $MTLAT_RUNS runs"
	python3 - "$out" <<'EOF'
import re, statistics, sys
pts = {}
for line in open(sys.argv[1]):
    m = re.search(r"mode=(\S+) dir=(\S+) pps=(\d+).*loss=\s*([\d.]+)%.*p50=\s*(\d+)us p99=\s*(\d+)us.*us/pkt client=\s*([\d.]+) server=\s*([\d.]+)", line)
    if not m:
        continue
    hop = re.search(r"hop=\{'p50': (-?\d+), 'p99': (-?\d+)\}", line)
    k = (m[2], int(m[3]), m[1])
    pts.setdefault(k, []).append([float(m[4]), int(m[5]), int(m[6]), float(m[7]), float(m[8])] +
                                 ([int(hop[1]), int(hop[2])] if hop else []))
for (d, rate, mode), v in sorted(pts.items()):
    med = [statistics.median(x[i] for x in v) for i in range(len(v[0]))]
    hop = " hop p50=%dus p99=%dus" % (med[5], med[6]) if len(med) > 5 else ""
    print("%-4s %6d %-7s loss=%.2f%% p50=%dus p99=%dus us/pkt client=%.1f server=%.1f%s" %
          (d, rate, mode, med[0], med[1], med[2], med[3], med[4], hop))
EOF
}
