#!/usr/bin/env python3
"""Tests for bench/radiorec.py on the answers the bench's four modems gave on
2026-10-14 (two RM520N-GL in one CPE, an RM502Q-AE, an RM520N-GL): LTE
alone, EN-DC with its NR5G-NSA line, carrier aggregation of three LTE
carriers, an NR secondary carrier, a carrier configured and not active,
"-" for what the modem does not report; plus the SA shape of Quectel's
manual. The line protocol it writes has to read back through
bench/fieldrec.py as cell-monitor's does. Run: python3 bench/radiorec_test.py
"""
import io
import os
import sys

sys.dont_write_bytecode = True  # no bench/__pycache__ in the tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fieldrec  # noqa: E402
import radiorec  # noqa: E402

fails = 0
checks = 0


def check(cond, what):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what)


ENTEL = ['+QENG: "servingcell","NOCONN","LTE","FDD",730,01,864D03,152,3100,7,5,5,C81,-83,-13,-48,16,11,-170,-']
WOM = ['+QENG: "servingcell","NOCONN"',
       '+QENG: "LTE","FDD",730,09,41C802,109,66736,66,5,5,137,-95,-14,-60,12,10,-70,-',
       '+QENG: "NR5G-NSA",730,09,24,-97,13,-10,641664,78,7,1']
MOVISTAR = ['+QENG: "servingcell","NOCONN","LTE","FDD",730,02,9B14721,91,3300,7,5,5,12D,-93,-13,-58,13,0,-,-']
SA = ['+QENG: "servingcell","CONNECT","NR5G-SA","TDD",730,09,1A2B3C4D5,321,7E8,641664,78,12,-88,-11,20,1,-']
CA_ENTEL = ['+QCAINFO: "PCC",3100,100,"LTE BAND 7",1,152,-84,-10,-49,14',
            '+QCAINFO: "SCC",1125,75,"LTE BAND 2",1,152,-76,-9,-60,17,0,-,-',
            '+QCAINFO: "SCC",9485,75,"LTE BAND 28",1,36,-87,-11,-68,8,0,-,-']
CA_WOM = ['+QCAINFO: "PCC",66736,100,"LTE BAND 66",1,109,-95,-13,-62,6',
          '+QCAINFO: "SCC",641664,7,"NR5G BAND 78",24']
CA_CLARO = ['+QCAINFO: "PCC",850,100,"LTE BAND 2",1,90,-95,-13,-62,18',
            '+QCAINFO: "SCC",2536,25,"LTE BAND 5",2,330,-91,-7,-66,5,0,-,-']
NW_WOM = ['+QNWINFO: "FDD LTE","73009","LTE BAND 66",66736', '+QNWINFO: "TDD NR5G","73009","NR5G BAND 78",641664']


def test_servingcell():
    (m, t, f), = radiorec.parse_servingcell(ENTEL)
    check(m == "cell_serving_lte" and t == {"state": "noconn", "duplex": "fdd", "band": "b7"}, f"entel tags: {t}")
    check(f["cellid"] == 0x864D03 and f["tac"] == 0xC81 and f["pci"] == 152 and f["earfcn"] == 3100, f"entel ids: {f}")
    check((f["rsrp"], f["rsrq"], f["rssi"], f["sinr"], f["cqi"]) == (-83, -13, -48, 16, 11), "entel signal")
    check(f["tx_power"] == -170 and f["srxlev"] is None, "entel tx power in tenths of dBm, srxlev '-' is None")

    pts = radiorec.parse_servingcell(WOM)
    check([p[0] for p in pts] == ["cell_serving_lte", "cell_serving_nr"], f"wom: LTE anchor and NR leg: {pts}")
    lte, nr = pts[0], pts[1]
    check(lte[1]["state"] == "noconn" and lte[1]["band"] == "b66" and lte[2]["earfcn"] == 66736, "wom anchor")
    check(nr[1] == {"state": "noconn", "mode": "nsa", "band": "n78"}, f"wom nr tags: {nr[1]}")
    check((nr[2]["pci"], nr[2]["rsrp"], nr[2]["sinr"], nr[2]["rsrq"], nr[2]["arfcn"]) == (24, -97, 13, -10, 641664),
          f"wom nr fields: {nr[2]}")

    (m, t, f), = radiorec.parse_servingcell(MOVISTAR)
    check(f["cqi"] == 0 and f["tx_power"] is None, "movistar idle: cqi 0, tx power '-'")

    (m, t, f), = radiorec.parse_servingcell(SA)
    check(m == "cell_serving_nr" and t["mode"] == "sa" and t["band"] == "n78" and t["state"] == "connect", f"sa: {t}")
    check(f["cellid"] == 0x1A2B3C4D5 and f["tac"] == 0x7E8 and (f["rsrp"], f["rsrq"], f["sinr"]) == (-88, -11, 20),
          f"sa fields: {f}")
    check(radiorec.parse_servingcell(["+QENG: \"servingcell\",\"SEARCH\""]) == [], "searching: no point")


def test_cainfo():
    pts = radiorec.parse_cainfo(CA_ENTEL)
    check([(t["cc_type"], t["cc_seq"], t["band"]) for _, t, _ in pts] == [("pcc", "1", "b7"), ("scc", "2", "b2"),
                                                                          ("scc", "3", "b28")], "entel: 3 carriers")
    check([f["bw_mhz"] for _, _, f in pts] == [20.0, 15.0, 15.0], "entel bandwidths from resource blocks")
    check(pts[2][2]["sinr"] == 8 and pts[2][2]["pci"] == 36, "entel b28 fields")
    pts = radiorec.parse_cainfo(CA_WOM)
    check(pts[1][1]["rat"] == "nr5g" and pts[1][1]["band"] == "n78", "wom: NR secondary carrier")
    check(pts[1][2] == {"arfcn": 641664, "bw_code": 7, "bw_mhz": 50, "pci": 24}, f"wom nr cc: {pts[1][2]}")
    pts = radiorec.parse_cainfo(CA_CLARO)
    check(pts[1][2]["cell_state"] == 2 and pts[1][2]["bw_mhz"] == 5.0, "claro: b5 configured, not active")


def test_nwinfo_and_lp():
    pts = radiorec.parse_nwinfo(NW_WOM)
    check([t for _, t, _ in pts] == [{"rat": "fdd_lte", "band": "lte_band_66"}, {"rat": "tdd_nr5g", "band": "nr5g_band_78"}],
          f"nwinfo tags as cell-monitor's san_tag: {pts}")
    m = radiorec.Modem("cpe1:/dev/ttyUSB6:WOM", "banco")
    check(m.tags == {"site": "banco", "cpe_id": "cpe1", "host": "cpe1", "modem_id": "wom", "leg": "wom",
                     "carrier": "wom"}, f"tags: {m.tags}")
    line = radiorec.lp_line("cell_nwinfo", {**m.tags, **pts[0][1]}, pts[0][2], 1791952829478000000)
    check(line == 'cell_nwinfo,site=banco,cpe_id=cpe1,host=cpe1,modem_id=wom,leg=wom,carrier=wom,rat=fdd_lte,'
          'band=lte_band_66 arfcn=66736i,plmn="73009" 1791952829478000000', f"line: {line}")
    check(radiorec.lp_line("x", {"a": "b"}, {"v": None}, 1) is None, "no field: no line")
    # what the recorder writes reads back through fieldrec as cell-monitor's lines do
    answers = {radiorec.CMDS[0]: WOM, radiorec.CMDS[1]: CA_WOM, radiorec.CMDS[2]: NW_WOM}
    lines = [radiorec.lp_line(meas, {**m.tags, **t}, f, 1791952829478000000 + i)
             for i, (meas, t, f) in enumerate(radiorec.points(answers))]
    rf = fieldrec.RF()
    fieldrec.read_rf_lp(io.StringIO("".join(l + "\n" for l in lines)), rf, "auto")
    check(not rf.unknown and not rf.unused, f"fieldrec knows every measurement: {rf.unknown} {rf.unused}")
    check(rf.records == len(lines) == 6, f"fieldrec took every line: {rf.records} of {len(lines)}")
    try:
        radiorec.Modem("cpe1:/dev/ttyUSB6", "banco")
        check(False, "a modem without its leg is refused")
    except ValueError:
        check(True, "")


def main():
    test_servingcell()
    test_cainfo()
    test_nwinfo_and_lp()
    print(f"radiorec_test: {checks - fails}/{checks} ok" if not fails else f"radiorec_test: {fails} of {checks} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
