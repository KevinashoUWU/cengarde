#!/bin/bash
# recv.sh: the receiver of the simulated broadcast (bench/stream/), for your own
# server (the cengarde VPS or another): an ffmpeg that listens for the RTMP or
# SRT stream of send.sh and
#   - records it as it comes (-c copy, Matroska), and
#   - logs the arrival of every video packet with the wall clock (packets.csv:
#     arrival time, pts, size, key frame), so that report.py can find the
#     stalls (gaps in arrival), the frames that came late against the sender's
#     timeline and the jumps in pts. ffmpeg copies the packets to a second
#     output, a pipe, that ffprobe reads and this script stamps as each line
#     comes out.
# It also writes ffmpeg's -progress, the TCP state of the accepted socket every
# 100 ms (ss -tin: out-of-order segments, DSACKs, receive RTT) or the UDP
# socket's buffer and drops for SRT, and, with -S, SRT's own receiver
# statistics from srt-live-transmit (loss, drops, retransmissions, delay).
#
# mediamtx (a single binary, github.com/bluenviron/mediamtx) also receives
# RTMP and SRT and has an API with per-connection stats, but it needs a
# download and a config; nothing here requires it.
#
# Stopping: Ctrl-C or SIGTERM send SIGINT to ffmpeg (never SIGTERM, which can
# leave the recording cut) and wait for it. By default it serves one stream
# after another, each in its own run directory; -1 serves one and exits.

LC_ALL=C # EPOCHREALTIME and awk follow the locale's decimal separator
export LC_ALL
set -u

VERSION=1

usage() {
	cat <<'EOF'
Usage: recv.sh [options]

  -p PROTO   rtmp (default) or srt
  -b ADDR    address to listen on (0.0.0.0; use the tunnel's address to be reachable only through it)
  -P PORT    port (1935 for RTMP, 9000 for SRT)
  -a APP     RTMP application (live)
  -k FILE    key file (mode 600): the RTMP stream key the sender uses (the same file as
             send.sh -k), or the SRT passphrase
  -l MS      SRT latency in ms (200)
  -S         SRT through srt-live-transmit (package srt-tools): receiver statistics
  -t SEC     stop SEC seconds after the stream starts (0: when it ends, the default)
  -1         serve one stream and exit (default: one after another)
  -R         do not record the stream (keep the logs only)
  -n NAME    run name (part of the directory name)
  -o DIR     where run directories go ($STREAM_RUNS, or ./stream-runs)
  -h         this help

For RTMP the key is required (ffmpeg only accepts the stream name it expects):
  printf 'mystream\n' >~/.config/cengarde-stream/own.key; chmod 600 ~/.config/cengarde-stream/own.key
EOF
}

PROTO=rtmp
BIND=0.0.0.0
PORT=
APP=live
KEYFILE=
SRT_MS=200
RELAY=0
STOPAFTER=0
ONCE=0
RECORD=1
NAME=
OUT=${STREAM_RUNS:-./stream-runs}
# shellcheck disable=SC2034 # read by lib.sh
SAMPLE_S=0.1
STATS_PERIOD=0.2
FFMPEG=${FFMPEG:-ffmpeg}
FFPROBE=${FFPROBE:-ffprobe}

while getopts p:b:P:a:k:l:St:1Rn:o:h o; do
	case $o in
	p) PROTO=$OPTARG ;;
	b) BIND=$OPTARG ;;
	P) PORT=$OPTARG ;;
	a) APP=$OPTARG ;;
	k) KEYFILE=$OPTARG ;;
	l) SRT_MS=$OPTARG ;;
	S) RELAY=1 ;;
	t) STOPAFTER=$OPTARG ;;
	1) ONCE=1 ;;
	R) RECORD=0 ;;
	n) NAME=$OPTARG ;;
	o) OUT=$OPTARG ;;
	h) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done

die() { echo "recv.sh: $*" >&2; exit 1; }
note() { echo "recv.sh: $*" >&2; }

HERE=$(cd "$(dirname "$0")" && pwd)
[ -r "$HERE/lib.sh" ] || die "lib.sh is missing next to recv.sh: copy the whole bench/stream directory"
KEY=
# shellcheck source=/dev/null
. "$HERE/lib.sh"

case $PROTO in rtmp) : "${PORT:=1935}" ;; srt) : "${PORT:=9000}" ;; *) die "-p is rtmp or srt" ;; esac
case $PORT$SRT_MS$STOPAFTER in '' | *[!0-9]*) die "-P, -l and -t take whole numbers" ;; esac
command -v "$FFMPEG" >/dev/null 2>&1 || die "no ffmpeg (Debian: apt install ffmpeg)"
FFV=$("$FFMPEG" -hide_banner -version 2>/dev/null | head -1 | sed 's/^ffmpeg version //; s/ Copyright.*//')
HAVE_PROBE=1
command -v "$FFPROBE" >/dev/null 2>&1 || { HAVE_PROBE=0; note "no ffprobe: no per-packet arrival log, only -progress"; }
if [ "$RELAY" = 1 ]; then
	[ "$PROTO" = srt ] || die "-S is for SRT"
	command -v srt-live-transmit >/dev/null 2>&1 || die "-S needs srt-live-transmit (Debian: apt install srt-tools)"
fi

if [ -n "$KEYFILE" ]; then
	if [ ! -f "$KEYFILE" ] || [ ! -r "$KEYFILE" ]; then die "cannot read the key file $KEYFILE"; fi
	mode=$(stat -c %a "$KEYFILE")
	[ $((8#$mode & 8#077)) -eq 0 ] || die "the key file $KEYFILE has mode $mode: chmod 600 it"
	kdir=$(cd "$(dirname "$KEYFILE")" && pwd)
	if git -C "$kdir" rev-parse --is-inside-work-tree >/dev/null 2>&1 && ! git -C "$kdir" check-ignore -q "$KEYFILE" 2>/dev/null; then
		die "the key file $KEYFILE is inside a git work tree and not ignored: keep it outside the repository"
	fi
	IFS= read -r KEY <"$KEYFILE" || [ -n "$KEY" ]
	KEY=${KEY%$'\r'}
	case $KEY in '' | *[[:space:]]*) die "the key file must hold one word, on its first line" ;; esac
	if [ "$PROTO" = srt ] && { [ "${#KEY}" -lt 10 ] || [ "${#KEY}" -gt 79 ]; }; then
		die "an SRT passphrase has 10 to 79 characters (the first line of $KEYFILE has ${#KEY})"
	fi
fi
[ "$PROTO" = rtmp ] && [ -z "$KEY" ] && die "RTMP needs the stream key the sender uses: -k FILE"

# ---- one session ---------------------------------------------------------------------------
FFPID=
RLPID=
STOP_SENT=0
STOP_AT=0
SHUTDOWN=0
READERS=()
RUN=
STOPPED_BY=

log() {
	stamp
	[ -n "$RUN" ] && printf '%s %s\n' "$NOW" "$*" >>"$RUN/recv.log"
	note "$*"
}

stop_ffmpeg() { # SIGINT, never SIGTERM; again every 15 s, three times in all
	[ -n "$FFPID" ] && kill -0 "$FFPID" 2>/dev/null || return 0
	if [ "$STOP_SENT" -lt 3 ]; then
		kill -INT "$FFPID" 2>/dev/null
		stamp
		STOP_AT=${NOW%.*}
		STOP_SENT=$((STOP_SENT + 1))
	fi
}
# shellcheck disable=SC2317 # run by the traps below
on_signal() {
	SHUTDOWN=1
	[ -z "$STOPPED_BY" ] && STOPPED_BY=interrupt
	log "signal received: stopping ffmpeg with SIGINT"
	stop_ffmpeg
}
# shellcheck disable=SC2317
cleanup() {
	local p
	[ -n "$RLPID" ] && kill "$RLPID" 2>/dev/null
	for p in "${READERS[@]}"; do
		kill "$p" 2>/dev/null
	done
	[ -n "$RUN" ] && rm -f "$RUN/.progress.fifo" "$RUN/.stderr.fifo" "$RUN/.in.fifo"
}
trap on_signal INT TERM
trap cleanup EXIT

write_meta() { # END_EPOCH FIRST_PACKET_EPOCH PACKETS
	{
		printf '{\n'
		printf '  "kind": "receiver", "tool": "recv.sh", "version": %s,\n' "$VERSION"
		printf '  "name": "%s", "run_id": "%s", "host": "%s",\n' "$(jesc "$NAME")" "$(jesc "$RUNID")" "$(jesc "$(hostname 2>/dev/null)")"
		printf '  "start_epoch": %s, "start_iso": "%s",\n' "$START_REAL" "$(iso "$START_REAL")"
		printf '  "end_epoch": %s, "end_iso": %s,\n' "$(num "$1")" "$([ -n "$1" ] && printf '"%s"' "$(iso "$1")" || printf null)"
		printf '  "first_packet_epoch": %s, "video_packets": %s,\n' "$(num "$2")" "$(num "$3")"
		printf '  "listen": {"proto": "%s", "bind": "%s", "port": %s, "transport": "%s", "app": "%s", "key_in_file": %s},\n' \
			"$PROTO" "$(jesc "$BIND")" "$PORT" "$([ "$PROTO" = srt ] && echo udp || echo tcp)" "$(jesc "$APP")" "$([ -n "$KEY" ] && echo true || echo false)"
		printf '  "srt": {"latency_ms": %s, "relay_stats": %s},\n' "$SRT_MS" "$([ "$RELAY" = 1 ] && echo true || echo false)"
		printf '  "recording": %s, "stopped_by": %s, "stop_after_s": %s,\n' "$([ "$RECORD" = 1 ] && printf '"rec.mkv"' || printf null)" \
			"$([ -n "$STOPPED_BY" ] && printf '"%s"' "$STOPPED_BY" || printf null)" "$STOPAFTER"
		printf '  "arrival_log": %s,\n' "$([ "$HAVE_PROBE" = 1 ] && printf '"packets.csv"' || printf null)"
		printf '  "kernel": {"uname": "%s", "tcp_congestion_control": "%s"},\n' "$(jesc "$(uname -srm)")" \
			"$(cat /proc/sys/net/ipv4/tcp_congestion_control 2>/dev/null || echo unknown)"
		printf '  "ffmpeg_version": "%s"\n' "$(jesc "$FFV")"
		printf '}\n'
	} >"$RUN/meta.json.tmp" && mv "$RUN/meta.json.tmp" "$RUN/meta.json"
}

session() {
	local n=$1 in_url first="" el now_s t_start last_status=0 rc npk packets
	RUNID=$(date +%Y%m%d-%H%M%S)-rx${NAME:+-$NAME}$([ "$n" -gt 1 ] && echo "-$n")
	RUN=$OUT/$RUNID
	mkdir -p "$RUN" || die "cannot create $RUN"
	chmod 700 "$RUN" 2>/dev/null
	STOP_SENT=0
	STOPPED_BY=
	READERS=()
	stamp
	START_REAL=$NOW
	t_start=${NOW%.*}
	mkfifo "$RUN/.progress.fifo" "$RUN/.stderr.fifo" && chmod 600 "$RUN/.progress.fifo" "$RUN/.stderr.fifo"
	stamp_lines <"$RUN/.progress.fifo" >>"$RUN/progress.log" &
	READERS+=($!)
	stamp_lines <"$RUN/.stderr.fifo" >>"$RUN/ffmpeg.log" &
	READERS+=($!)
	[ "$HAVE_PROBE" = 1 ] && echo "t,pts,dts,size,flags" >"$RUN/packets.csv"

	# RTMP carries the codec parameters in its first message, so a short probe is enough and keeps the
	# start-up burst small; MPEG-TS has them in-band, at the next key frame (up to a GOP away): the default probe.
	local fin=()
	if [ "$PROTO" = rtmp ]; then
		in_url="rtmp://$BIND:$PORT/$APP/$KEY"
		fin+=(-analyzeduration 100000 -probesize 100000 -listen 1 -i "$in_url")
		log "listening for RTMP on rtmp://$BIND:$PORT/$APP/*** (run $RUNID)"
	elif [ "$RELAY" = 1 ]; then
		# srt-live-transmit receives (and keeps the statistics) and writes the stream to a fifo.
		mkfifo "$RUN/.in.fifo" && chmod 600 "$RUN/.in.fifo"
		in_url="srt://$BIND:$PORT?mode=listener&latency=$SRT_MS"
		[ -n "$KEY" ] && in_url="$in_url&passphrase=$KEY&pbkeylen=16"
		npk=50
		srt-live-transmit "$in_url" file://con -c:1316 -a:no -statsout:"$RUN/srt-stats.csv" -pf:csv -s:"$npk" -f -loglevel:warn \
			>"$RUN/.in.fifo" 2> >(stamp_lines >>"$RUN/srt-relay.log") &
		RLPID=$!
		fin+=(-i "$RUN/.in.fifo")
		log "listening for SRT on $BIND:$PORT through srt-live-transmit, latency $SRT_MS ms (run $RUNID)"
	else
		in_url="srt://$BIND:$PORT?mode=listener&latency=$((SRT_MS * 1000))" # ffmpeg's latency is in microseconds
		[ -n "$KEY" ] && in_url="$in_url&passphrase=$KEY&pbkeylen=16"
		fin+=(-i "$in_url")
		log "listening for SRT on $BIND:$PORT, latency $SRT_MS ms (run $RUNID)"
	fi

	local fout=()
	[ "$STOPAFTER" -gt 0 ] && fout=(-t "$STOPAFTER")
	local outs=()
	[ "$RECORD" = 1 ] && outs+=(-map '0:v?' -map '0:a?' -c copy "${fout[@]}" -f matroska "$RUN/rec.mkv")
	if [ "$HAVE_PROBE" = 1 ]; then
		outs+=(-map 0:v:0 -c copy "${fout[@]}" -f nut pipe:1)
	elif [ "$RECORD" = 0 ]; then
		outs+=(-map 0:v:0 -c copy "${fout[@]}" -f null -)
	fi
	write_meta "" "" ""
	if [ "$HAVE_PROBE" = 1 ]; then
		"$FFMPEG" -nostdin -hide_banner -loglevel info -nostats -stats_period "$STATS_PERIOD" -progress "$RUN/.progress.fifo" \
			"${fin[@]}" "${outs[@]}" 2>"$RUN/.stderr.fifo" \
			> >("$FFPROBE" -v error -probesize 32 -analyzeduration 0 -select_streams v:0 \
				-show_entries packet=pts_time,dts_time,size,flags -of csv=p=0 -i pipe:0 2>>"$RUN/ffprobe.log" | stamp_csv >>"$RUN/packets.csv") &
	else
		"$FFMPEG" -nostdin -hide_banner -loglevel info -nostats -stats_period "$STATS_PERIOD" -progress "$RUN/.progress.fifo" \
			"${fin[@]}" "${outs[@]}" 2>"$RUN/.stderr.fifo" >/dev/null &
	fi
	FFPID=$!

	if [ "$PROTO" = rtmp ]; then
		ss_sampler tcp "$PORT" "$FFPID" "$RUN/tcp.csv" &
	else
		ss_sampler udp "$PORT" "${RLPID:-$FFPID}" "$RUN/udp.csv" &
	fi
	READERS+=($!)

	while kill -0 "$FFPID" 2>/dev/null; do
		sleep 0.5 &
		wait $!
		stamp
		now_s=${NOW%.*}
		el=$((now_s - t_start))
		if [ -z "$first" ] && [ "$HAVE_PROBE" = 1 ] && [ "$(wc -l <"$RUN/packets.csv")" -gt 1 ]; then
			first=1
			log "the stream has started"
		fi
		if [ "$first" = 1 ] && [ $((el - last_status)) -ge 5 ]; then
			last_status=$el
			packets=$(($(wc -l <"$RUN/packets.csv") - 1))
			log "$packets video packets so far"
		fi
		if [ "$STOP_SENT" -gt 0 ] && [ "$STOP_SENT" -lt 3 ] && [ $((now_s - STOP_AT)) -gt 15 ]; then
			stop_ffmpeg
		elif [ "$STOP_SENT" -ge 3 ] && [ $((now_s - STOP_AT)) -gt 30 ]; then
			log "ffmpeg ignored SIGINT three times: SIGKILL"
			kill -KILL "$FFPID" 2>/dev/null
		fi
	done
	wait "$FFPID"
	rc=$?
	stamp
	local end=$NOW
	[ -z "$STOPPED_BY" ] && STOPPED_BY=stream-ended
	if [ -n "$RLPID" ]; then
		for _ in $(seq 1 25); do
			kill -0 "$RLPID" 2>/dev/null || break
			sleep 0.2
		done
		kill "$RLPID" 2>/dev/null
		wait "$RLPID" 2>/dev/null
		RLPID=
	fi
	sleep 0.5 # the readers write their last lines
	local firstt="" npk2=0
	if [ "$HAVE_PROBE" = 1 ]; then
		npk2=$(($(wc -l <"$RUN/packets.csv") - 1))
		[ "$npk2" -gt 0 ] && firstt=$(sed -n 2p "$RUN/packets.csv" | cut -d, -f1)
	fi
	write_meta "$end" "$firstt" "$npk2"
	log "ffmpeg ended (exit $rc, $STOPPED_BY); $npk2 video packets; run directory $RUN"
	[ "$npk2" -gt 0 ] && echo "recv.sh: analyse with: $HERE/report.py <sender run> --rx $RUN" >&2
	# ffmpeg failing at once, with no stream, means the port or the key is wrong: do not spin.
	if [ "$npk2" -le 0 ] && [ $((${end%.*} - t_start)) -lt 3 ] && [ "$STOPPED_BY" = stream-ended ]; then
		tail -n 5 "$RUN/ffmpeg.log" >&2
		return 2
	fi
	return 0
}

n=0
while :; do
	n=$((n + 1))
	session "$n"
	rc=$?
	[ "$rc" = 2 ] && exit 1
	[ "$ONCE" = 1 ] || [ "$SHUTDOWN" = 1 ] && break
done
exit 0
