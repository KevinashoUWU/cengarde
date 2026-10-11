#!/bin/bash
# send.sh: the sender of the simulated broadcast (bench/stream/). It runs on
# the PC that sits on the router's LAN, so that its traffic goes through the
# cengarde tunnel, and sends a test picture that makes stalls visible to
# RTMP/RTMPS (YouTube, or recv.sh on your own server) or to SRT.
#
# The picture is testsrc2 (or smptehdbars) with a burned-in clock and frame
# counter, plus a 1 kHz sine. CLK is the nominal capture time of the frame
# (the start of the run plus its pts), to the millisecond; NOW is the real
# wall clock when the frame was made, to the second. When the sender is held
# back (a full socket buffer, a slow encoder) CLK stays on the nominal
# timeline, so what you read on the received picture against your own clock is
# the delay, and a frozen or jumping picture is a stall.
#
# While ffmpeg runs, everything is written, with wall-clock stamps, to a run
# directory that report.py reads: ffmpeg's -progress, the state of the stream's
# TCP socket every 100 ms (ss -tin) or of its UDP socket, SRT statistics when
# the relay is used, an optional headers-only tcpdump, and a meta.json.
#
# THE KEY: it is read from a file (mode 600), never typed on the command line
# and never stored in the run directory (the logs and meta.json have it
# masked). But ffmpeg still gets the URL, key included, in its argv: any user
# of this PC can read it with ps while the run lasts. Run this on a PC you
# own, or mount /proc with hidepid=2. The same goes for the SRT passphrase.
#
# Stopping: Ctrl-C, SIGTERM or -d SECONDS send SIGINT to ffmpeg (never SIGTERM,
# which can leave files cut) and wait for it to finish.

LC_ALL=C # EPOCHREALTIME and awk follow the locale's decimal separator
export LC_ALL
set -u

VERSION=1

usage() {
	cat <<'EOF'
Usage: send.sh [options] DEST

DEST (the key is not part of it):
  rtmp://HOST[:PORT]/APP    RTMP; the stream key goes after APP (from -k)
  rtmps://HOST[:PORT]/APP   RTMP over TLS (YouTube: rtmps://a.rtmps.youtube.com:443/live2)
  srt://HOST:PORT           SRT, caller (the optional -k file is the passphrase)

Options:
  -k FILE    key file, mode 600 or 400: the RTMP stream key, or the SRT passphrase
  -e ENC     auto (default), av1_nvenc, h264_nvenc, hevc_nvenc, libx264, libsvtav1
             auto takes the first of $STREAM_ENCODER_ORDER (av1_nvenc h264_nvenc libx264)
             that passes a one-frame test encode, through the container the run needs
  -b RATE    video bitrate: 6M (default), 8M, 6000k, 4500000
  -s WxH     picture size (1920x1080)
  -f FPS     frames per second (30)
  -g SEC     GOP length in seconds, fixed (2)
  -B MS      encoder VBV buffer in ms (1000)
  -d SEC     run for SEC seconds (0: until Ctrl-C, the default)
  -l MS      SRT latency in ms (200)
  -C CONT    SRT container: auto (mpegts; matroska for AV1), mpegts, matroska, nut
  -S         SRT through srt-live-transmit (package srt-tools): gives SRT statistics
  -P         headers-only tcpdump of the stream (needs root, or sudo -n)
  -c SPEC    cengarde status snapshot at the start and the end: a file, or HOST:PATH over ssh
  -n NAME    run name (part of the directory name)
  -o DIR     where run directories go ($STREAM_RUNS, or ./stream-runs)
  -p PATTERN testsrc2 (default) or smptehdbars
  -N LEVEL   noise added so that the encoder fills the bitrate (8; 0 turns it off)
  -L SEC     lead added to the burned clock's origin, for the start-up time (1.0)
  -T         test the encoders and tools, then exit (--check)
  -D         print the ffmpeg command, key masked, then exit (--dry-run)
  -h         this help

Long forms: --encoder --bitrate --size --fps --gop --buffer --duration --latency --container
  --srt-stats --pcap --cengarde --name --out --pattern --noise --lead --key --check --dry-run
  (as --name VALUE or --name=VALUE)
EOF
}

long2short() {
	case $1 in
	--encoder) echo -e ;; --bitrate) echo -b ;; --size) echo -s ;; --fps) echo -f ;; --gop) echo -g ;;
	--buffer) echo -B ;; --duration) echo -d ;; --latency) echo -l ;; --container) echo -C ;;
	--cengarde) echo -c ;; --name) echo -n ;; --out) echo -o ;; --pattern) echo -p ;; --noise) echo -N ;;
	--lead) echo -L ;; --key) echo -k ;;
	--srt-stats) echo -S ;; --pcap) echo -P ;; --check) echo -T ;; --dry-run) echo -D ;; --help) echo -h ;;
	*) return 1 ;;
	esac
}
ARGS=()
while [ $# -gt 0 ]; do
	case $1 in
	--*=*)
		s=$(long2short "${1%%=*}") || { echo "send.sh: unknown option ${1%%=*}" >&2; exit 2; }
		ARGS+=("$s" "${1#*=}")
		;;
	--*)
		s=$(long2short "$1") || { echo "send.sh: unknown option $1" >&2; exit 2; }
		ARGS+=("$s")
		;;
	*) ARGS+=("$1") ;;
	esac
	shift
done
set -- "${ARGS[@]}"

KEYFILE=
ENC=auto
RATE=6M
SIZE=1920x1080
FPS=30
GOP_S=2
BUF_MS=1000
DURATION=0
SRT_MS=200
CONT=auto
RELAY=0
PCAP=0
CGSPEC=
NAME=
OUT=${STREAM_RUNS:-./stream-runs}
PATTERN=testsrc2
NOISE=8
LEAD=1.0
CHECK=0
DRY=0
SAMPLE_S=0.1
STATS_PERIOD=0.2
FFMPEG=${FFMPEG:-ffmpeg}
AUTO_ORDER=${STREAM_ENCODER_ORDER:-"av1_nvenc h264_nvenc libx264"}

while getopts k:e:b:s:f:g:B:d:l:C:SPc:n:o:p:N:L:TDh o; do
	case $o in
	k) KEYFILE=$OPTARG ;;
	e) ENC=$OPTARG ;;
	b) RATE=$OPTARG ;;
	s) SIZE=$OPTARG ;;
	f) FPS=$OPTARG ;;
	g) GOP_S=$OPTARG ;;
	B) BUF_MS=$OPTARG ;;
	d) DURATION=$OPTARG ;;
	l) SRT_MS=$OPTARG ;;
	C) CONT=$OPTARG ;;
	S) RELAY=1 ;;
	P) PCAP=1 ;;
	c) CGSPEC=$OPTARG ;;
	n) NAME=$OPTARG ;;
	o) OUT=$OPTARG ;;
	p) PATTERN=$OPTARG ;;
	N) NOISE=$OPTARG ;;
	L) LEAD=$OPTARG ;;
	T) CHECK=1 ;;
	D) DRY=1 ;;
	h) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
DEST=${1:-}
REQ_ENC=$ENC

die() { echo "send.sh: $*" >&2; exit 1; }
note() { echo "send.sh: $*" >&2; }

HERE=$(cd "$(dirname "$0")" && pwd)
[ -r "$HERE/lib.sh" ] || die "lib.sh is missing next to send.sh: copy the whole bench/stream directory"
KEY=
# shellcheck source=/dev/null
. "$HERE/lib.sh"

# to_bps 6M -> 6000000 (or "bad")
to_bps() {
	awk -v r="$1" 'BEGIN {
		n = r + 0; u = r; sub(/^[0-9.]+/, "", u); u = tolower(u)
		if (u == "k" || u == "kbit/s" || u == "kbps") n *= 1e3
		else if (u == "m" || u == "mbit/s" || u == "mbps") n *= 1e6
		else if (u == "g") n *= 1e9
		else if (u != "") { print "bad"; exit }
		if (n <= 0) { print "bad"; exit }
		printf "%.0f\n", n }'
}

# ---- encoders ------------------------------------------------------------------------------
# build_enc_args ENC TIER: sets ENC_ARGS. Tier "full" adds the low-latency
# options of each encoder; "core" has only the ones every build is sure to
# have. The probe falls back from full to core, so a build that lacks an
# option still works (and says so).
build_enc_args() {
	local enc=$1 tier=$2
	local br=$BPS buf=$BUFBITS gop=$GOPF
	case $enc in
	h264_nvenc | hevc_nvenc | av1_nvenc)
		# CBR, low-latency tune, no B frames, a fixed GOP of IDR frames.
		ENC_ARGS=(-c:v "$enc" -preset p4 -tune ll -rc cbr -b:v "$br" -maxrate "$br" -bufsize "$buf" -g "$gop" -bf 0 -pix_fmt yuv420p)
		[ "$tier" = full ] && ENC_ARGS+=(-rc-lookahead 0 -forced-idr 1 -no-scenecut 1 -zerolatency 1)
		case $enc in
		h264_nvenc) ENC_ARGS+=(-profile:v high) ;;
		hevc_nvenc) ENC_ARGS+=(-profile:v main) ;;
		esac
		;;
	libx264)
		ENC_ARGS=(-c:v libx264 -preset "${X264_PRESET:-veryfast}" -tune zerolatency -b:v "$br" -minrate "$br" -maxrate "$br"
			-bufsize "$buf" -g "$gop" -keyint_min "$gop" -sc_threshold 0 -bf 0 -pix_fmt yuv420p)
		[ "$tier" = full ] && ENC_ARGS+=(-profile:v high)
		;;
	libsvtav1)
		# SVT-AV1 on the CPU: constant bitrate, low-delay prediction, no look-ahead.
		ENC_ARGS=(-c:v libsvtav1 -preset "${SVT_PRESET:-10}" -b:v "$br" -g "$gop" -pix_fmt yuv420p)
		[ "$tier" = full ] && ENC_ARGS+=(-svtav1-params rc=2:pred-struct=1:lookahead=0:scd=0)
		;;
	*) return 1 ;;
	esac
}

# container_for ENC PROTO: the container the stream goes in. AV1 does not go
# in MPEG-TS: ffmpeg writes it there but cannot read it back (checked with
# ffmpeg 6.1), so SRT carries AV1 in Matroska.
container_for() {
	case $2 in
	rtmp | rtmps) echo flv ;;
	srt)
		case $CONT in
		auto) case $1 in *av1*) echo matroska ;; *) echo mpegts ;; esac ;;
		mpegts | matroska | nut) echo "$CONT" ;;
		*) return 1 ;;
		esac
		;;
	esac
}

# probe_encoder ENC MUX: a one-frame test encode with the options the run will
# use, first to nothing and then through the container. The list of encoders
# shows the nvenc ones on a machine without a GPU: only an encode proves it.
# Sets PROBE_TIER (full or core) or PROBE_WHY.
PROBE_TIER=
PROBE_WHY=
probe_encoder() {
	local enc=$1 mux=$2 tier out ok=0 mopts=()
	PROBE_TIER=
	PROBE_WHY=
	"$FFMPEG" -hide_banner -encoders 2>/dev/null | grep -qw -- "$enc" || { PROBE_WHY="this ffmpeg has no $enc"; return 1; }
	for tier in full core; do
		build_enc_args "$enc" "$tier" || return 1
		if out=$("$FFMPEG" -nostdin -hide_banner -loglevel error -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 1 \
			"${ENC_ARGS[@]}" -f null - 2>&1); then
			ok=1
			break
		fi
	done
	if [ "$ok" = 0 ]; then
		PROBE_WHY=$(printf '%s\n' "$out" | grep -v '^$' | grep -m1 -i -E 'cannot|error|fail|unknown|not ' || printf '%s' "$out" | head -1)
		return 1
	fi
	[ "$mux" = matroska ] && mopts=(-live 1)
	if ! out=$("$FFMPEG" -nostdin -hide_banner -loglevel error -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 2 \
		"${ENC_ARGS[@]}" "${mopts[@]}" -f "$mux" - 2>&1 >/dev/null); then
		PROBE_WHY="$enc encodes, but this ffmpeg cannot put it in $mux"
		[ "$mux" = flv ] && PROBE_WHY="$PROBE_WHY: RTMP needs Enhanced RTMP for AV1 and HEVC (ffmpeg 6.1 or newer); this is $FFV"
		return 1
	fi
	PROBE_TIER=$tier
	return 0
}

check_all() {
	local e mux proto first="" t
	echo "ffmpeg: $FFV"
	echo "bash: $BASH_VERSION (EPOCHREALTIME: $([ -n "${EPOCHREALTIME:-}" ] && echo yes || echo 'no, date is used'))"
	for t in ffprobe ss tcpdump srt-live-transmit python3 sudo; do
		printf '  %-18s %s\n' "$t" "$(command -v "$t" || echo 'not found')"
	done
	printf '  %-18s %s\n' "font" "${FONT:-none found (apt install fonts-dejavu-core)}"
	printf '  %-18s %s\n' "drawtext filter" "$("$FFMPEG" -hide_banner -filters 2>/dev/null | grep -qw drawtext && echo yes || echo 'NO: no burned-in clock')"
	printf '  %-18s %s\n' "tcp congestion" "$(cat /proc/sys/net/ipv4/tcp_congestion_control 2>/dev/null || echo '?')"
	echo "one-frame test encodes at $((BPS / 1000)) kbit/s:"
	for e in av1_nvenc h264_nvenc hevc_nvenc libx264 libsvtav1; do
		for proto in flv mpegts; do
			mux=$proto
			[ "$proto" = mpegts ] && case $e in *av1*) mux=matroska ;; esac
			if probe_encoder "$e" "$mux"; then
				printf '  %-12s in %-9s ok (options: %s)' "$e" "$mux" "$PROBE_TIER"
				if [ "$proto" = flv ] && [ -z "$first" ]; then
					case " $AUTO_ORDER " in *" $e "*) first=$e; printf '   <- auto picks this for RTMP' ;; esac
				fi
				echo
			else
				printf '  %-12s in %-9s no: %s\n' "$e" "$mux" "$PROBE_WHY"
			fi
		done
	done
	[ -n "$first" ] || echo "  nothing in the auto order ($AUTO_ORDER) works with RTMP"
}

# ---- tools and numbers ---------------------------------------------------------------------
command -v "$FFMPEG" >/dev/null 2>&1 || die "no ffmpeg (Debian: apt install ffmpeg)"
FFV=$("$FFMPEG" -hide_banner -version 2>/dev/null | head -1 | sed 's/^ffmpeg version //; s/ Copyright.*//')

FONT=
for f in /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf /usr/share/fonts/dejavu/DejaVuSansMono.ttf \
	/usr/share/fonts/TTF/DejaVuSansMono.ttf /usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf; do
	[ -r "$f" ] && { FONT=$f; break; }
done
[ -z "$FONT" ] && command -v fc-match >/dev/null 2>&1 && FONT=$(fc-match -f '%{file}' monospace 2>/dev/null)

BPS=$(to_bps "$RATE")
[ "$BPS" = bad ] && die "bad bitrate: $RATE"
case $SIZE in [0-9]*x[0-9]*) ;; *) die "bad size: $SIZE (WxH)" ;; esac
W=${SIZE%x*}
H=${SIZE#*x}
case $FPS$GOP_S$DURATION$SRT_MS$BUF_MS$NOISE$W$H in *[!0-9]*) die "-s, -f, -g, -d, -l, -B and -N take whole numbers" ;; esac
case $LEAD in '' | *[!0-9.]*) die "bad lead: $LEAD" ;; esac
GOPF=$((FPS * GOP_S))
BUFBITS=$((BPS * BUF_MS / 1000))
[ "$GOPF" -ge 1 ] || die "bad GOP"

if [ "$CHECK" = 1 ]; then
	check_all
	exit 0
fi

# ---- destination ---------------------------------------------------------------------------
[ -n "$DEST" ] || { usage >&2; exit 2; }
PROTO=${DEST%%://*}
case $PROTO in rtmp | rtmps | srt) ;; *) die "DEST must start with rtmp://, rtmps:// or srt://" ;; esac
REST=${DEST#*://}
HOSTPORT=${REST%%[/?]*}
PATHQ=${REST#"$HOSTPORT"}
case $HOSTPORT in
\[*\]:*) DHOST=${HOSTPORT%%\]*}] DPORT=${HOSTPORT##*]:} ;;
\[*\]) DHOST=$HOSTPORT DPORT= ;;
*:*) DHOST=${HOSTPORT%:*} DPORT=${HOSTPORT##*:} ;;
*) DHOST=$HOSTPORT DPORT= ;;
esac
if [ -z "$DPORT" ]; then
	case $PROTO in rtmp) DPORT=1935 ;; rtmps) DPORT=443 ;; srt) die "an SRT destination needs a port: srt://HOST:PORT" ;; esac
fi
case $DPORT in '' | *[!0-9]*) die "bad port in $DEST" ;; esac
if [ "$RELAY" = 1 ]; then
	[ "$PROTO" = srt ] || die "-S is for SRT"
	command -v srt-live-transmit >/dev/null 2>&1 || die "-S needs srt-live-transmit (Debian: apt install srt-tools)"
fi
if [ "$PROTO" = srt ]; then
	case $PATHQ in *passphrase=*) die "put the passphrase in the key file (-k), not in the URL" ;; esac
fi

# ---- the key -------------------------------------------------------------------------------
if [ -n "$KEYFILE" ]; then
	if [ ! -f "$KEYFILE" ] || [ ! -r "$KEYFILE" ]; then die "cannot read the key file $KEYFILE"; fi
	mode=$(stat -c %a "$KEYFILE")
	[ $((8#$mode & 8#077)) -eq 0 ] || die "the key file $KEYFILE has mode $mode: chmod 600 it"
	# A key file inside a git work tree that does not ignore it could be committed.
	kdir=$(cd "$(dirname "$KEYFILE")" && pwd)
	if git -C "$kdir" rev-parse --is-inside-work-tree >/dev/null 2>&1 && ! git -C "$kdir" check-ignore -q "$KEYFILE" 2>/dev/null; then
		die "the key file $KEYFILE is inside a git work tree and not ignored: keep it outside the repository (e.g. ~/.config/cengarde-stream/)"
	fi
	IFS= read -r KEY <"$KEYFILE" || [ -n "$KEY" ]
	KEY=${KEY%$'\r'}
	case $KEY in '' | *[[:space:]]*) die "the key file must hold one word, the key, on its first line" ;; esac
	if [ "$PROTO" = srt ] && { [ "${#KEY}" -lt 10 ] || [ "${#KEY}" -gt 79 ]; }; then
		die "an SRT passphrase has 10 to 79 characters (the first line of $KEYFILE has ${#KEY})"
	fi
fi
case $PROTO in rtmp | rtmps) [ -n "$KEY" ] || die "RTMP needs the stream key: -k FILE" ;; esac

# ---- the encoder ---------------------------------------------------------------------------
ENC_NOTE=
if [ "$ENC" = auto ]; then
	chosen=
	for e in $AUTO_ORDER; do
		mux=$(container_for "$e" "$PROTO") || die "bad container: $CONT"
		if probe_encoder "$e" "$mux"; then
			chosen=$e
			ENC_NOTE="auto: $e passed a one-frame test encode (options: $PROBE_TIER)"
			break
		fi
		note "auto: not $e: $PROBE_WHY"
	done
	[ -n "$chosen" ] || die "auto: none of $AUTO_ORDER can encode here; send.sh -T says why"
	ENC=$chosen
else
	mux=$(container_for "$ENC" "$PROTO") || die "bad container: $CONT"
	probe_encoder "$ENC" "$mux" || die "$ENC: $PROBE_WHY"
	ENC_NOTE="requested: $ENC passed a one-frame test encode (options: $PROBE_TIER)"
fi
ENC_TIER=$PROBE_TIER
MUX=$(container_for "$ENC" "$PROTO") || die "bad container: $CONT"
build_enc_args "$ENC" "$ENC_TIER"
[ "$ENC_TIER" = core ] && note "$ENC: this build lacks some low-latency options, so only the core ones are used"
note "encoder $ENC in $MUX, $((BPS / 1000)) kbit/s, ${SIZE}@${FPS}, GOP of $GOPF frames"

case $PATTERN in testsrc2 | smptehdbars) ;; *) die "pattern: testsrc2 or smptehdbars" ;; esac

# ---- output URL ----------------------------------------------------------------------------
case $PROTO in
rtmp | rtmps)
	BASE=${DEST%/}
	OUTURL="$BASE/$KEY"
	MASKED="$BASE/***"
	;;
srt)
	q="mode=caller&latency=$((SRT_MS * 1000))&pkt_size=1316&transtype=live" # ffmpeg's latency is in microseconds
	[ -n "$KEY" ] && q="$q&passphrase=$KEY&pbkeylen=16"
	extra=${PATHQ#\?}
	[ -n "$extra" ] && [ "$extra" != "$PATHQ" ] && q="$q&$extra"
	OUTURL="srt://$HOSTPORT?$q"
	MASKED=$OUTURL
	[ -n "$KEY" ] && MASKED=${OUTURL//"$KEY"/***}
	;;
esac

FF_PRE=("$FFMPEG" -nostdin -hide_banner -loglevel info -nostats -stats_period "$STATS_PERIOD")
FF_OUT=(-map 0:v:0 -map 1:a:0 "${ENC_ARGS[@]}" -c:a aac -b:a 128k -ar 48000 -ac 2)
[ "$DURATION" -gt 0 ] && FF_OUT+=(-t "$DURATION")
case $MUX in
flv) FF_OUT+=(-flvflags no_duration_filesize -f flv) ;;
mpegts) FF_OUT+=(-mpegts_flags +resend_headers -f mpegts) ;;
matroska) FF_OUT+=(-live 1 -f matroska) ;;
nut) FF_OUT+=(-f nut) ;;
esac

# make_graph: the picture, with the clock's origin taken now, as late as possible.
tz=$(date +%z)
TZS=$(( ${tz:0:1}1 * (10#${tz:1:2} * 3600 + 10#${tz:3:2} * 60) ))
ORIGIN=0
VGRAPH=
make_graph() {
	local fs1=$((H / 14)) fs2=$((H / 22)) fontopt box t0 hasdraw=0
	[ "$fs1" -ge 10 ] || fs1=10
	[ "$fs2" -ge 8 ] || fs2=8
	stamp
	ORIGIN=$(awk -v e="$NOW" -v l="$LEAD" 'BEGIN { printf "%.3f", e + l }')
	# The origin as seconds of the local day, which drawtext adds to the pts.
	t0=$(awk -v o="$ORIGIN" -v z="$TZS" 'BEGIN { t = o + z; printf "%.3f", t - 86400 * int(t / 86400) }')
	if [ -n "$FONT" ]; then fontopt="fontfile=$FONT"; else fontopt="font=monospace"; fi
	VGRAPH="${PATTERN}=size=${W}x${H}:rate=${FPS}"
	[ "$NOISE" -gt 0 ] && VGRAPH="$VGRAPH,noise=alls=${NOISE}:allf=t"
	"$FFMPEG" -hide_banner -filters 2>/dev/null | grep -qw drawtext && hasdraw=1
	if [ "$hasdraw" = 1 ]; then
		box="fontcolor=white:box=1:boxcolor=black@0.7:boxborderw=6:x=w*0.03"
		VGRAPH="$VGRAPH,drawtext=${fontopt}:fontsize=${fs1}:${box}:y=h*0.62:text='CLK %{pts\\:hms\\:${t0}\\:24HH}'"
		VGRAPH="$VGRAPH,drawtext=${fontopt}:fontsize=${fs2}:${box}:y=h*0.76:text='FRAME %{n}  ${ENC} $((BPS / 1000))k'"
		VGRAPH="$VGRAPH,drawtext=${fontopt}:fontsize=${fs2}:fontcolor=yellow:box=1:boxcolor=black@0.7:boxborderw=6:x=w*0.03:y=h*0.84:text='NOW %{localtime\\:%T}'"
	else
		note "this ffmpeg has no drawtext filter: no burned-in clock"
	fi
}

if [ "$DRY" = 1 ]; then
	make_graph
	echo "${FF_PRE[*]} -progress FIFO -re -f lavfi -i \"$VGRAPH\" -re -f lavfi -i sine=frequency=1000:sample_rate=48000 ${FF_OUT[*]} $([ "$RELAY" = 1 ] && echo RELAY_FIFO || echo "$MASKED")"
	[ "$RELAY" = 1 ] && echo "srt-live-transmit file://con srt://$HOSTPORT?mode=caller&latency=$SRT_MS... -statsout:srt-stats.csv -pf:csv -f"
	exit 0
fi

# ---- the run directory ---------------------------------------------------------------------
RUNID=$(date +%Y%m%d-%H%M%S)${NAME:+-$NAME}
RUN=$OUT/$RUNID
mkdir -p "$RUN" || die "cannot create $RUN"
chmod 700 "$RUN" 2>/dev/null
CCFG=$(cat /proc/sys/net/ipv4/tcp_congestion_control 2>/dev/null || echo unknown)
QDISC=$(cat /proc/sys/net/core/default_qdisc 2>/dev/null || echo unknown)
stamp
START_REAL=$NOW

log() {
	stamp
	printf '%s %s\n' "$NOW" "$*" >>"$RUN/send.log"
	note "$*"
}

# snapshot SPEC FILE: a file, or HOST:PATH read over ssh
snapshot() {
	local ok=0
	case $1 in
	'') return 0 ;;
	/* | ./* | ../*) cp -- "$1" "$2" 2>/dev/null && ok=1 ;;
	*:*) ssh -o BatchMode=yes -o ConnectTimeout=3 "${1%%:*}" cat -- "${1#*:}" >"$2" 2>/dev/null && ok=1 ;;
	*) cp -- "$1" "$2" 2>/dev/null && ok=1 ;;
	esac
	[ "$ok" = 1 ] || { rm -f "$2"; log "no cengarde status from $1"; return 1; }
}

# ---- meta.json -----------------------------------------------------------------------------
STOPPED_BY=
FF_EXIT=
END_REAL=
LPORT=
SOCKPROTO=
write_meta() {
	{
		printf '{\n'
		printf '  "kind": "sender", "tool": "send.sh", "version": %s,\n' "$VERSION"
		printf '  "name": "%s", "run_id": "%s", "host": "%s",\n' "$(jesc "$NAME")" "$(jesc "$RUNID")" "$(jesc "$(hostname 2>/dev/null)")"
		printf '  "start_epoch": %s, "start_iso": "%s",\n' "$START_REAL" "$(iso "$START_REAL")"
		if [ -n "$END_REAL" ]; then
			printf '  "end_epoch": %s, "end_iso": "%s",\n' "$END_REAL" "$(iso "$END_REAL")"
		else
			printf '  "end_epoch": null, "end_iso": null,\n'
		fi
		printf '  "duration_requested_s": %s, "stopped_by": %s, "ffmpeg_exit": %s,\n' "$DURATION" \
			"$([ -n "$STOPPED_BY" ] && printf '"%s"' "$STOPPED_BY" || printf null)" "$(num "$FF_EXIT")"
		printf '  "dest": {"scheme": "%s", "host": "%s", "port": %s, "transport": "%s", "key_in_file": %s, "url_masked": "%s"},\n' \
			"$PROTO" "$(jesc "$DHOST")" "$DPORT" "$([ "$PROTO" = srt ] && echo udp || echo tcp)" "$([ -n "$KEY" ] && echo true || echo false)" "$(jesc "$MASKED")"
		printf '  "encoder": {"requested": "%s", "used": "%s", "options_tier": "%s", "selection": "%s", "container": "%s"},\n' \
			"$REQ_ENC" "$ENC" "$ENC_TIER" "$(jesc "$ENC_NOTE")" "$MUX"
		printf '  "target_bitrate_bps": %s, "vbv_buffer_ms": %s,\n' "$BPS" "$BUF_MS"
		printf '  "video": {"size": "%s", "fps": %s, "gop_s": %s, "gop_frames": %s, "pattern": "%s", "noise": %s},\n' \
			"$SIZE" "$FPS" "$GOP_S" "$GOPF" "$PATTERN" "$NOISE"
		printf '  "srt": {"latency_ms": %s, "relay_stats": %s},\n' "$SRT_MS" "$([ "$RELAY" = 1 ] && echo true || echo false)"
		printf '  "clock": {"burn_origin_epoch": %s, "lead_s": %s, "tz_offset_s": %s, "note": "CLK is the origin plus the pts; report.py estimates the real offset"},\n' "$ORIGIN" "$LEAD" "$TZS"
		printf '  "kernel": {"uname": "%s", "tcp_congestion_control": "%s", "default_qdisc": "%s"},\n' "$(jesc "$(uname -srm)")" "$CCFG" "$QDISC"
		printf '  "socket": {"proto": "%s", "local_port": %s, "sample_interval_s": %s},\n' "${SOCKPROTO:-none}" "$(num "$LPORT")" "$SAMPLE_S"
		printf '  "cengarde_status": {"start": %s, "end": %s},\n' \
			"$([ -f "$RUN/cengarde-start.json" ] && printf '"cengarde-start.json"' || printf null)" \
			"$([ -f "$RUN/cengarde-end.json" ] && printf '"cengarde-end.json"' || printf null)"
		printf '  "tcpdump": %s,\n' "$([ -f "$RUN/headers.pcap" ] && printf '"headers.pcap"' || printf null)"
		printf '  "ffmpeg_version": "%s",\n' "$(jesc "$FFV")"
		printf '  "command": "%s"\n' "$(jesc "${FF_PRE[*]} -progress FIFO ${FF_OUT[*]} $([ "$RELAY" = 1 ] && echo RELAY_FIFO || echo "$MASKED")")"
		printf '}\n'
	} >"$RUN/meta.json.tmp" && mv "$RUN/meta.json.tmp" "$RUN/meta.json"
}

# ---- the run -------------------------------------------------------------------------------
FFPID=
TDPID=
RLPID=
READERS=()
STOP_SENT=0
STOP_AT=0
stop_ffmpeg() { # SIGINT, never SIGTERM: again every 15 s, three times in all
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
	[ -z "$STOPPED_BY" ] && STOPPED_BY=interrupt
	log "signal received: stopping ffmpeg with SIGINT"
	stop_ffmpeg
}
# shellcheck disable=SC2317
cleanup() {
	local p
	for p in $RLPID $TDPID; do
		kill "$p" 2>/dev/null
	done
	for p in "${READERS[@]}"; do
		kill "$p" 2>/dev/null
	done
	rm -f "$RUN/.progress.fifo" "$RUN/.stderr.fifo" "$RUN/.relay.fifo"
}
trap on_signal INT TERM
trap cleanup EXIT

PFIFO=$RUN/.progress.fifo
EFIFO=$RUN/.stderr.fifo
mkfifo "$PFIFO" "$EFIFO" || die "cannot make fifos in $RUN"
chmod 600 "$RUN/.progress.fifo" "$RUN/.stderr.fifo"
stamp_lines <"$PFIFO" >>"$RUN/progress.log" &
READERS+=($!)
stamp_lines <"$EFIFO" >>"$RUN/ffmpeg.log" &
READERS+=($!)

snapshot "$CGSPEC" "$RUN/cengarde-start.json"
write_meta

if [ "$PCAP" = 1 ]; then
	tp="tcp port $DPORT"
	[ "$PROTO" = srt ] && tp="udp port $DPORT"
	# 96 bytes: the link-layer, IP and TCP headers and almost no payload (plain RTMP carries the key in
	# the clear, in its first messages; RTMPS does not)
	TD=(tcpdump -i any -s 96 -nn -w "$RUN/headers.pcap" "$tp")
	[ "$(id -u)" != 0 ] && TD=(sudo -n "${TD[@]}")
	"${TD[@]}" >"$RUN/tcpdump.log" 2>&1 &
	TDPID=$!
	sleep 0.5
	kill -0 "$TDPID" 2>/dev/null || { log "tcpdump did not start (root or sudo -n needed): $(head -c 200 "$RUN/tcpdump.log")"; TDPID=; }
fi

SRT_URL=
if [ "$RELAY" = 1 ]; then
	# ffmpeg writes the stream to a fifo that srt-live-transmit sends over SRT.
	# Its stats (cumulative, every N packets, with a Timepoint) go to srt-stats.csv.
	RFIFO=$RUN/.relay.fifo
	mkfifo "$RFIFO" && chmod 600 "$RFIFO"
	npk=$((BPS / 8 / 1316 / 5))
	[ "$npk" -ge 10 ] || npk=10
	# maxbw: without it libsrt paces the sender from an estimate of the input rate that starts low, and
	# the first key frame, which is a burst, ages past the latency in the send buffer and is dropped.
	SRT_URL="srt://$HOSTPORT?mode=caller&latency=$SRT_MS&maxbw=$((BPS * 3 / 8))"
	[ -n "$KEY" ] && SRT_URL="$SRT_URL&passphrase=$KEY&pbkeylen=16"
	srt-live-transmit file://con "$SRT_URL" -c:1316 -statsout:"$RUN/srt-stats.csv" -pf:csv -s:"$npk" -f -loglevel:warn \
		<"$RFIFO" > >(stamp_lines >>"$RUN/srt-relay.log") 2>&1 &
	RLPID=$!
	OUTURL=$RFIFO
fi

make_graph
FF_IN=(-re -f lavfi -i "$VGRAPH" -re -f lavfi -i "sine=frequency=1000:sample_rate=48000")
# -y: ffmpeg refuses to open an output that exists, and the relay's fifo does
"${FF_PRE[@]}" -y -progress "$PFIFO" "${FF_IN[@]}" "${FF_OUT[@]}" "$OUTURL" 2>"$EFIFO" >/dev/null &
FFPID=$!
log "ffmpeg started (pid $FFPID) to ${MASKED}"
write_meta

# The stream's socket: poll fast so that the first sample is the start of the connection.
tries=0
while [ -z "$LPORT" ] && kill -0 "$FFPID" 2>/dev/null && [ "$tries" -lt 400 ]; do
	if [ "$PROTO" = srt ]; then
		SOCKPROTO=udp
		LPORT=$(find_sock udp "${RLPID:-$FFPID}" "$DPORT")
	else
		SOCKPROTO=tcp
		LPORT=$(find_sock tcp "$FFPID" "$DPORT")
	fi
	[ -n "$LPORT" ] && break
	tries=$((tries + 1))
	sleep 0.05
done
if [ -n "$LPORT" ]; then
	log "stream socket: local port $LPORT ($SOCKPROTO), sampled every ${SAMPLE_S}s"
	ss_sampler "$SOCKPROTO" "$LPORT" "$FFPID" "$RUN/$([ "$SOCKPROTO" = tcp ] && echo tcp || echo udp).csv" &
	READERS+=($!)
else
	SOCKPROTO=
	kill -0 "$FFPID" 2>/dev/null && log "no socket found for ffmpeg: no ss samples"
fi
write_meta

# status_line: the last numbers, every 5 s
status_line() {
	local br sp dr tcp=""
	read -r br sp dr < <(tail -n 16 "$RUN/progress.log" 2>/dev/null | awk '
		$2 ~ /^bitrate=/ { b = $2; sub(/.*=/, "", b) }
		$2 ~ /^speed=/ { s = $2; sub(/.*=/, "", s) }
		$2 ~ /^drop_frames=/ { d = $2; sub(/.*=/, "", d) }
		END { print (b == "" ? "-" : b), (s == "" ? "-" : s), (d == "" ? "-" : d) }')
	if [ "$SOCKPROTO" = tcp ] && [ -s "$RUN/tcp.csv" ]; then
		tcp=$(tail -n 1 "$RUN/tcp.csv" | awk -F, '{ printf " | tcp cwnd %s rtt %s ms retrans %s unacked %s", $6, $8, $16, $14 }')
	fi
	log "bitrate ${br} speed ${sp} dropped ${dr}${tcp}"
}

t_start=${START_REAL%.*}
last_status=0
while kill -0 "$FFPID" 2>/dev/null; do
	sleep 0.5 &
	wait $!
	stamp
	now_s=${NOW%.*}
	el=$((now_s - t_start))
	if [ $((el - last_status)) -ge 5 ]; then
		last_status=$el
		status_line
	fi
	if [ "$STOP_SENT" = 0 ]; then
		# A stuck ffmpeg (a dead path) must still end: -d plus 20 s, then SIGINT.
		if [ "$DURATION" -gt 0 ] && [ "$el" -gt $((DURATION + 20)) ]; then
			STOPPED_BY=watchdog
			log "ffmpeg did not stop at $DURATION s: SIGINT"
			stop_ffmpeg
		fi
	elif [ "$STOP_SENT" -lt 3 ]; then
		[ $((now_s - STOP_AT)) -gt 15 ] && stop_ffmpeg
	elif [ $((now_s - STOP_AT)) -gt 30 ]; then
		log "ffmpeg ignored SIGINT three times: SIGKILL"
		kill -KILL "$FFPID" 2>/dev/null
	fi
done
wait "$FFPID"
FF_EXIT=$?
stamp
END_REAL=$NOW
# ffmpeg exits with 255 when a signal stops it, which is what we asked for.
[ "$STOP_SENT" -gt 0 ] && [ "$FF_EXIT" = 255 ] && FF_EXIT=0
if [ -z "$STOPPED_BY" ]; then
	if [ "$FF_EXIT" != 0 ]; then
		STOPPED_BY=ffmpeg-error
	elif [ "$DURATION" -gt 0 ]; then
		STOPPED_BY=duration
	else
		STOPPED_BY=ffmpeg-exit
	fi
fi
log "ffmpeg ended (exit $FF_EXIT, $STOPPED_BY)"

snapshot "$CGSPEC" "$RUN/cengarde-end.json"
sleep 0.3 # let the samplers and the readers write their last lines
# The relay sees the end of its fifo and finishes the SRT stream by itself; give it time.
if [ -n "$RLPID" ]; then
	for _ in $(seq 1 $((SRT_MS / 200 + 5))); do
		kill -0 "$RLPID" 2>/dev/null || break
		sleep 0.2
	done
fi
for p in $TDPID $RLPID; do
	kill "$p" 2>/dev/null
done
[ -n "$TDPID$RLPID" ] && wait $TDPID $RLPID 2>/dev/null
TDPID=
RLPID=
write_meta
echo "send.sh: run directory $RUN"
echo "send.sh: analyse with: $(dirname "$0")/report.py $RUN"
[ "$FF_EXIT" = 0 ] || { tail -n 5 "$RUN/ffmpeg.log" >&2; exit 1; }
exit 0
