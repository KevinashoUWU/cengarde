#!/bin/sh
# celda.sh start|stop: a virtual LTE cell on this PC: srsEPC + srsENB + the
# GNU Radio channel (canal.py, one noise knob per UE on XML-RPC :8765) +
# NUE srsUEs, UE k in netns ue(k+1) (the core is 172.16.0.1).
#
#   PRB    50 (10 MHz, default), 75 (15 MHz) or 100 (20 MHz)
#   EARFCN downlink EARFCN (default 3350, band 7; 1125: band 2, Entel's)
#   NUE    number of UEs (default 1); UE 0 is the one cengarde rides, the
#          others are the cell's "other users"
#   LOGS   logs, metrics (enb_report.json, ue*_metrics.csv) (default /tmp/celda)
D=$(cd "$(dirname "$0")" && pwd); C=${CONF:-$HOME/sdr/conf}; LOGS=${LOGS:-/tmp/celda}; PRB=${PRB:-50}; EARFCN=${EARFCN:-3350}; NUE=${NUE:-1}
case "$PRB" in 50) SR=11.52e6 ;; 75) SR=15.36e6 ;; 100) SR=23.04e6 ;; *) echo "PRB: 50, 75 or 100"; exit 2 ;; esac
case "$1" in
stop)
	sudo -n pkill -x srsue; sudo -n pkill -x srsenb; sudo -n pkill -x srsepc
	[ -f "$LOGS/canal.pid" ] && kill "$(cat "$LOGS/canal.pid")" 2>/dev/null
	sleep 1; exit 0 ;;
start)
	mkdir -p "$LOGS"
	sed "0,/dl_earfcn = [0-9]*;/s//dl_earfcn = $EARFCN;/" "$C/rr.conf" > "$LOGS/rr.conf"
	cd "$C" || exit 1
	sudo -n sh -c "srsepc epc.conf --log.filename=$LOGS/epc.log > $LOGS/epc.out 2>&1 &"
	sleep 2
	sudo -n sh -c "srsenb enb.conf --enb.n_prb=$PRB --enb_files.rr_config=$LOGS/rr.conf --rf.device_name=zmq --rf.device_args='fail_on_disconnect=true,tx_port=tcp://*:2000,rx_port=tcp://localhost:2100,id=enb,base_srate=$SR' --expert.metrics_csv_enable=true --expert.metrics_csv_filename=$LOGS/enb_metrics.csv --expert.metrics_period_secs=1 --expert.report_json_enable=true --expert.report_json_filename=$LOGS/enb_report.json --log.filename=$LOGS/enb.log > $LOGS/enb.out 2>&1 &"
	sleep 2
	nohup python3 "$D/canal.py" "$NUE" > "$LOGS/canal.out" 2>&1 & echo $! > "$LOGS/canal.pid"
	sleep 2
	k=0
	while [ "$k" -lt "$NUE" ]; do
		ns=ue$((k + 1)); sudo -n ip netns add "$ns" 2>/dev/null
		sudo -n sh -c "srsue ue.conf --usim.imsi=00101012345678$k --usim.imei=35349006987331$k --rat.eutra.dl_earfcn=$EARFCN --rf.device_name=zmq --rf.device_args='tx_port=tcp://*:$((2001 + 10 * k)),rx_port=tcp://localhost:$((2101 + 10 * k)),id=ue$k,base_srate=$SR' --gw.netns=$ns --general.metrics_csv_enable=true --general.metrics_period_secs=1 --general.metrics_csv_filename=$LOGS/ue${k}_metrics.csv --log.filename=$LOGS/ue$k.log > $LOGS/ue$k.out 2>&1 &"
		k=$((k + 1))
	done
	n=0
	for i in $(seq 1 45); do
		n=$(grep -l "attach successful" "$LOGS"/ue*.out 2>/dev/null | wc -l)
		[ "$n" -ge "$NUE" ] && { echo "celda: $n UE attached (PRB $PRB, EARFCN $EARFCN)"; grep -h "IP:" "$LOGS"/ue*.out; exit 0; }
		sleep 1
	done
	echo "celda: $n of $NUE attached (see $LOGS)"; exit 1 ;;
*) echo "usage: [PRB=..] [EARFCN=..] [NUE=..] $0 start|stop"; exit 2 ;;
esac
