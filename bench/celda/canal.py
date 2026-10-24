#!/usr/bin/env python3
"""canal.py [N_UE]: a GNU Radio channel between srsENB and N srsUEs over ZMQ
(REQ/REP, as srsRAN expects).

  downlink: eNB tx :2000 -> one channel per UE -> UE k rx :2101+10k
  uplink:   UE k tx :2001+10k -> its channel -> sum -> eNB rx :2100

Uplink as in a real cell: each UE's signal arrives with its own gain (its
path loss: set_ul_gain(g, ue), 1.0 = as sent), the signals add up, and the
eNB's receiver adds one noise floor for all (set_ul_noise(v), a noise
voltage). A UE's SNR is then its gain against the common floor; a weak UE
does not make the others worse. Downlink: one channel per UE (its own
receiver noise, set_dl_noise(v, ue)). Live over XML-RPC on 127.0.0.1:8765;
get_power() gives mean |x|^2 at each point. srsRAN's ZMQ samples are large
(about 250-450 rms), so useful noise voltages are ~300-5000."""
import sys
import threading
from xmlrpc.server import SimpleXMLRPCServer

from gnuradio import blocks, channels, gr, zeromq

C = gr.sizeof_gr_complex


def chan(seed):
    return channels.channel_model(noise_voltage=0.0, frequency_offset=0.0, epsilon=1.0, taps=[1.0], noise_seed=seed)


class Canal(gr.top_block):
    def __init__(self, n):
        gr.top_block.__init__(self, "canal")
        self.n = n
        self.dl_src = zeromq.req_source(C, 1, "tcp://localhost:2000", 100, False, -1)
        self.ul_snk = zeromq.rep_sink(C, 1, "tcp://*:2100", 100, False, -1)
        self.dl_ch, self.ul_gain, self.ul_src, self.dl_snk = [], [], [], []
        self.ul_floor = chan(99)
        add = blocks.add_cc() if n > 1 else None
        for k in range(n):
            d = chan(1 + 2 * k)
            g = blocks.multiply_const_cc(1.0)
            ds = zeromq.rep_sink(C, 1, f"tcp://*:{2101 + 10 * k}", 100, False, -1)
            us = zeromq.req_source(C, 1, f"tcp://localhost:{2001 + 10 * k}", 100, False, -1)
            self.connect(self.dl_src, d, ds)
            self.connect(us, g)
            self.connect(g, (add, k) if add else self.ul_floor)
            self.dl_ch.append(d)
            self.ul_gain.append(g)
            self.ul_src.append(us)
            self.dl_snk.append(ds)
        if add:
            self.connect(add, self.ul_floor)
        self.connect(self.ul_floor, self.ul_snk)
        self.probes = {}
        points = [("dl_in", self.dl_src)] + [(f"ul_in{k}", self.ul_src[k]) for k in range(n)] + \
                 [("ul_out", self.ul_floor)]
        for name, blk in points:
            mag = blocks.complex_to_mag_squared()
            avg = blocks.moving_average_ff(100000, 1.0 / 100000, 4000)
            prb = blocks.probe_signal_f()
            self.connect(blk, mag, avg, prb)
            self.probes[name] = prb

    def get_power(self):
        return {k: p.level() for k, p in self.probes.items()}

    def set_dl_noise(self, v, ue=0):
        self.dl_ch[int(ue)].set_noise_voltage(float(v))
        return float(v)

    def set_ul_noise(self, v, ue=None):
        """The eNB's noise floor (one for every UE; ue is ignored)."""
        self.ul_floor.set_noise_voltage(float(v))
        return float(v)

    def set_ul_gain(self, g, ue=0):
        self.ul_gain[int(ue)].set_k(complex(float(g), 0))
        return float(g)


tb = Canal(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
srv = SimpleXMLRPCServer(("127.0.0.1", 8765), logRequests=False, allow_none=True)
for f in (tb.set_dl_noise, tb.set_ul_noise, tb.set_ul_gain, tb.get_power):
    srv.register_function(f, f.__name__)
threading.Thread(target=srv.serve_forever, daemon=True).start()
tb.start()
tb.wait()
