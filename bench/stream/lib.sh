# shellcheck shell=bash
# lib.sh: what send.sh and recv.sh share (bench/stream/). Sourced, never run.
# The scripts set LC_ALL=C before sourcing it, because EPOCHREALTIME and awk
# print the locale's decimal separator (a comma in es_CL).

# stamp: NOW = the wall clock in seconds with microseconds (bash 5 has it, else date)
if [ -n "${EPOCHREALTIME:-}" ]; then
	stamp() { NOW=$EPOCHREALTIME; }
else
	stamp() { NOW=$(date +%s.%N); }
fi

# iso EPOCH: 2026-10-11T19:20:21.123Z
iso() { date -u -d "@$1" +%Y-%m-%dT%H:%M:%S.%3NZ 2>/dev/null; }

# jesc TEXT: TEXT escaped for a JSON string
jesc() {
	local s=$1
	s=${s//\\/\\\\}
	s=${s//\"/\\\"}
	s=${s//$'\n'/\\n}
	s=${s//$'\t'/\\t}
	s=${s//$'\r'/\\r}
	printf '%s' "$s"
}

# num X: X when it is a number, else null (for JSON)
num() { case $1 in '' | *[!0-9.-]*) printf 'null' ;; *) printf '%s' "$1" ;; esac; }

# stamp_lines: stdin to stdout, every line behind the wall clock, with $KEY and
# $PASS masked (the scripts set them; empty means nothing to mask)
stamp_lines() {
	local line
	while IFS= read -r line; do
		[ -n "${KEY:-}" ] && line=${line//"$KEY"/'***'}
		stamp
		printf '%s %s\n' "$NOW" "$line"
	done
}

# stamp_csv: stdin to stdout as "WALLCLOCK,LINE"
stamp_csv() {
	local line
	while IFS= read -r line; do
		stamp
		printf '%s,%s\n' "$NOW" "$line"
	done
}

# find_sock PROTO PID DPORT: the local port of the socket PID holds towards
# DPORT (for udp, any socket of PID: SRT's is not connected)
find_sock() {
	local proto=$1 pid=$2 dport=$3 loc peer rest
	while read -r _ _ _ loc peer rest; do
		case $rest in
		*"pid=$pid,"*)
			if [ "$proto" = udp ]; then
				echo "${loc##*:}"
				return 0
			fi
			case $peer in *":$dport") echo "${loc##*:}"; return 0 ;; esac
			;;
		esac
	done < <(if [ "$proto" = tcp ]; then ss -tnpH 2>/dev/null; else ss -uanpH 2>/dev/null; fi)
	return 1
}

# The awk programs that turn ss lines into CSV. A TCP line (ss -tinHO) is
#   STATE recvq sendq local peer cc wscale:.. rto:.. rtt:SRTT/VAR .. cwnd:N .. retrans:CUR/TOTAL ..
# The counters the kernel prints only when they are not 0 (retrans, unacked,
# lost, sacked, notsent, bytes_retrans, the busy and *_limited times, the
# receive-side ooopack and dsack_dups) come out as 0 when absent; an absent
# ssthresh means "not set yet" and stays empty. A listening socket is skipped.
# Rates are in bit/s whatever the iproute2 version prints (bps, Kbps, Mbps).
# shellcheck disable=SC2034 # AWK_TCP and AWK_UDP are used by the scripts
read -r -d '' AWK_TCP <<'EOF'
function rate(v,   n) {
	n = v + 0
	if (v ~ /Gbps$/) n *= 1e9; else if (v ~ /Mbps$/) n *= 1e6; else if (v ~ /Kbps$/) n *= 1e3
	return sprintf("%.0f", n)
}
function ms(v) { sub(/ms.*/, "", v); return v }
BEGIN {
	print "t,state,local,peer,cc,cwnd,ssthresh,rtt_ms,rttvar_ms,minrtt_ms,rto_ms,mss,pmtu,unacked,retrans_cur,retrans_total,lost,sacked,reordering,notsent,sendq,bytes_sent,bytes_acked,bytes_retrans,delivered,delivery_rate_bps,pacing_rate_bps,send_bps,busy_ms,rwnd_limited_ms,sndbuf_limited_ms,app_limited,segs_out,data_segs_out,snd_wnd,bytes_received,segs_in,rcv_ooopack,dsack_dups,rcv_rtt_ms,rcv_space"
	fflush()
}
$1 == "T" { t = $2; next }
NF >= 5 && $1 != "LISTEN" {
	state = $1; sendq = $3; loc = $4; peer = $5
	cc = ""; cwnd = ""; ssth = ""; rtt = ""; rttvar = ""; minrtt = ""; rto = ""; mss = ""; pmtu = ""
	unack = 0; rcur = 0; rtot = 0; lost = 0; sacked = 0; reord = ""; notsent = 0
	bsent = ""; backed = ""; bretr = 0; deliv = ""; drate = ""; prate = ""; srate = ""
	busy = 0; rwl = 0; sbl = 0; app = 0; segs = ""; dsegs = ""; swnd = ""
	brecv = ""; segsin = ""; ooo = 0; dsack = 0; rcvrtt = ""; rcvsp = ""
	for (i = 6; i <= NF; i++) {
		tok = $i
		if (tok == "send") { srate = rate($(i + 1)); i++; continue }
		if (tok == "pacing_rate") { prate = rate($(i + 1)); i++; continue }
		if (tok == "delivery_rate") { drate = rate($(i + 1)); i++; continue }
		if (tok == "app_limited") { app = 1; continue }
		c = index(tok, ":")
		if (c == 0) {
			if (cc == "" && tok !~ /^(ts|sack|ecn|ecnseen|fastopen|dsack)$/) cc = tok
			continue
		}
		k = substr(tok, 1, c - 1); v = substr(tok, c + 1)
		if (k == "rtt") { split(v, a, "/"); rtt = a[1]; rttvar = a[2] }
		else if (k == "retrans") { split(v, a, "/"); rcur = a[1]; rtot = a[2] }
		else if (k == "cwnd") cwnd = v
		else if (k == "ssthresh") ssth = v
		else if (k == "minrtt") minrtt = v
		else if (k == "rto") rto = v
		else if (k == "mss") mss = v
		else if (k == "pmtu") pmtu = v
		else if (k == "unacked") unack = v
		else if (k == "lost") lost = v
		else if (k == "sacked") sacked = v
		else if (k == "reordering") reord = v
		else if (k == "notsent") notsent = v
		else if (k == "bytes_sent") bsent = v
		else if (k == "bytes_acked") backed = v
		else if (k == "bytes_retrans") bretr = v
		else if (k == "delivered") deliv = v
		else if (k == "busy") busy = ms(v)
		else if (k == "rwnd_limited") rwl = ms(v)
		else if (k == "sndbuf_limited") sbl = ms(v)
		else if (k == "segs_out") segs = v
		else if (k == "data_segs_out") dsegs = v
		else if (k == "snd_wnd") swnd = v
		else if (k == "bytes_received") brecv = v
		else if (k == "segs_in") segsin = v
		else if (k == "rcv_ooopack") ooo = v
		else if (k == "dsack_dups") dsack = v
		else if (k == "rcv_rtt") rcvrtt = v
		else if (k == "rcv_space") rcvsp = v
	}
	print t "," state "," loc "," peer "," cc "," cwnd "," ssth "," rtt "," rttvar "," minrtt "," rto "," mss "," pmtu "," unack "," rcur "," rtot "," lost "," sacked "," reord "," notsent "," sendq "," bsent "," backed "," bretr "," deliv "," drate "," prate "," srate "," busy "," rwl "," sbl "," app "," segs "," dsegs "," swnd "," brecv "," segsin "," ooo "," dsack "," rcvrtt "," rcvsp
	fflush()
}
EOF
# A UDP line (ss -uamHO) ends with skmem:(r0,rb212992,t0,tb212992,f0,w0,o0,bl0,d0).
read -r -d '' AWK_UDP <<'EOF'
BEGIN { print "t,state,recvq,sendq,rmem_alloc,rcvbuf,wmem_alloc,sndbuf,drops"; fflush() }
$1 == "T" { t = $2; next }
NF >= 5 {
	r = ""; rb = ""; tt = ""; tb = ""; d = ""
	for (i = 6; i <= NF; i++) if ($i ~ /^skmem:/) {
		s = $i; gsub(/^skmem:\(|\)$/, "", s); n = split(s, a, ",")
		for (j = 1; j <= n; j++) {
			if (a[j] ~ /^rb/) rb = substr(a[j], 3)
			else if (a[j] ~ /^tb/) tb = substr(a[j], 3)
			else if (a[j] ~ /^r[0-9]/) r = substr(a[j], 2)
			else if (a[j] ~ /^t[0-9]/) tt = substr(a[j], 2)
			else if (a[j] ~ /^d[0-9]/) d = substr(a[j], 2)
		}
	}
	print t "," $1 "," $2 "," $3 "," r "," rb "," tt "," tb "," d
	fflush()
}
EOF

# ss_sampler PROTO SPORT PID FILE: one ss sample of the sockets on local port
# SPORT every SAMPLE_S, until PID is gone; CSV to FILE. It ends by itself, so
# no stray loop is left if the script dies.
ss_sampler() {
	local proto=$1 sport=$2 pid=$3 file=$4 flags=-tinHO prog=$AWK_TCP
	if [ "$proto" = udp ]; then
		flags=-uamHO
		prog=$AWK_UDP
	fi
	{
		while kill -0 "$pid" 2>/dev/null; do
			stamp
			printf 'T %s\n' "$NOW"
			ss "$flags" "( sport = :$sport )" 2>/dev/null
			sleep "${SAMPLE_S:-0.1}"
		done
	} | awk "$prog" >>"$file"
}
