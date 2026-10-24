#!/bin/sh
# setup.sh: builds and installs srsRAN 4G with ZMQ (no SDR hardware) and
# writes the configs celda.sh uses into $CONF (default ~/sdr/conf). Debian 13:
# srsRAN is not packaged, GNU Radio 3.10 with gr-zeromq is. Needs sudo.
#   SRSRAN_REV  the srsRAN_4G revision (default bef8680, tested 2026-10-22)
# It applies srsran-cola.patch: a byte cap on the UE's RLC UM queue, read
# from SRSRAN_UM_TX_QUEUE_BYTES (celda.sh's QUEUE_BYTES); unset, srsRAN
# behaves as upstream.
set -e
D=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$HOME/sdr/srsRAN_4G}; CONF=${CONF:-$HOME/sdr/conf}; REV=${SRSRAN_REV:-bef8680}
sudo apt-get install -y build-essential cmake git libfftw3-dev libmbedtls-dev libboost-program-options-dev \
	libconfig++-dev libsctp-dev libzmq3-dev libyaml-cpp-dev libgtest-dev libpcsclite-dev pkg-config \
	gnuradio iperf3
[ -d "$SRC" ] || git clone https://github.com/srsran/srsRAN_4G.git "$SRC"
cd "$SRC" && git fetch -q origin && git checkout -q "$REV"
git apply --check "$D/srsran-cola.patch" 2>/dev/null && git apply "$D/srsran-cola.patch"
mkdir -p build && cd build
# -Werror off: gcc 14 rejects one of srsRAN's own tests (block_test.c)
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_ZEROMQ=ON -DENABLE_UHD=OFF -DENABLE_BLADERF=OFF -DENABLE_SOAPYSDR=OFF \
	-DENABLE_GUI=OFF -DENABLE_WERROR=OFF -DENABLE_ALL_TEST=OFF -DBUILD_TESTING=OFF ..
make -j"$(nproc)"
sudo make install && sudo ldconfig
mkdir -p "$CONF"
cp ../srsenb/enb.conf.example "$CONF/enb.conf"; cp ../srsenb/rr.conf.example "$CONF/rr.conf"
cp ../srsenb/sib.conf.example "$CONF/sib.conf"; cp ../srsenb/rb.conf.example "$CONF/rb.conf"
cp ../srsepc/epc.conf.example "$CONF/epc.conf"; cp ../srsue/ue.conf.example "$CONF/ue.conf"
cp ../srsepc/user_db.csv.example "$CONF/user_db.csv"
# UE k is IMSI 00101012345678k (ue.conf's key and OPc); the example has k=0.
# The core rewrites this file when it stops: add users with it stopped.
for k in 1 2 3; do
	echo "ueb$k,mil,00101012345678$k,00112233445566778899aabbccddeeff,opc,63bfa50ee6523365ff14c1f45f88737d,8000,0000000012b8,7,dynamic" >> "$CONF/user_db.csv"
done
echo "setup: srsRAN installed, configs in $CONF; try: NUE=2 bench/celda/celda.sh start"
