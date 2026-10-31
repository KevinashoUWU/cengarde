"""Offline check of three ideas (Google copilot, 2026-10-31) on curva1 (techo,
2026-10-28): fixed-rate UDP per modem, 40-160 % of its TCP ceiling, 10 s steps,
every record captured at both ends (send and arrival times).
(5) rate by arrival spacing (windowed mean) vs the max of burst samples;
(2) queue-delay slope: lead time before the queue crosses the target;
(1) fast forgetting after a capacity drop: max-filter vs variance-adaptive EWMA."""
import sys, numpy as np, pandas as pd
REC = 1300 * 8  # bits per record
NAMES = {1: "Entel", 2: "WOM", 3: "Movistar", 4: "Claro"}
d = pd.read_csv(sys.argv[1], usecols=["link", "t_send", "t_arr"])
d["share"], d["ln"] = d.link // 10, d.link % 10
rows, leads, forget = [], [], []
for ln in sorted(d.ln.unique()):
    L = d[d.ln == ln]
    arr_all = L.dropna(subset=["t_arr"])
    base = ((arr_all.t_arr - arr_all.t_send) / 1e6).quantile(0.001)  # ms, the path without queue
    for share in sorted(L.share.unique()):
        S = L[L.share == share]
        t0, t1 = S.t_send.min(), S.t_send.max()
        A = S.dropna(subset=["t_arr"]).sort_values("t_arr")
        if len(A) < 100:
            continue
        ta = (A.t_arr.values - t0) / 1e9
        owd = (A.t_arr.values - A.t_send.values) / 1e6 - base
        dur = (t1 - t0) / 1e9
        offered = len(S) * REC / dur / 1e6
        m = (ta > 2) & (ta < dur - 1)
        truth = m.sum() * REC / (ta[m].max() - ta[m].min()) / 1e6 if m.sum() > 10 else np.nan
        loss = 1 - len(A) / len(S)
        sat = offered > truth * 1.05 or loss > 0.01
        # delivery samples every 10 ms: bits arrived in the last 40 ms (about an SFACK's worth)
        grid = np.arange(2, dur - 1, 0.01)
        cum = np.searchsorted(ta, grid)
        def win(w):
            back = np.searchsorted(ta, grid - w)
            return (cum - back) * REC / w / 1e6
        s40 = win(0.04)
        emax = pd.Series(s40).rolling(150, min_periods=50).max().values      # max of 1.5 s (linkcc: 1-2 s)
        edisp = win(0.5)                                                     # spacing: mean over 500 ms
        emed = pd.Series(s40).rolling(100, min_periods=50).median().values   # median of 1 s
        ok = ~np.isnan(emax)
        rows.append(dict(link=NAMES[ln], share=share, offered=offered, truth=truth, loss=loss * 100, sat=sat,
                         owd_p50=np.median(owd), owd_p95=np.percentile(owd, 95),
                         max_bias=np.nanmean(emax[ok]) / truth, disp_bias=np.nanmean(edisp[ok]) / truth,
                         med_bias=np.nanmean(emed[ok]) / truth,
                         max_p90=np.nanpercentile(emax[ok], 90) / truth, disp_p90=np.nanpercentile(edisp[ok], 90) / truth))
        # (2) queue slope: owd smoothed 20 ms on a 10 ms grid
        g2 = np.arange(0, dur, 0.01)
        idx = np.clip(np.searchsorted(ta, g2), 1, len(ta) - 1)
        q = pd.Series(owd[idx - 1]).rolling(2, min_periods=1).median().values
        cross = np.argmax(q > 60) if (q > 60).any() else None
        slope = (q - np.roll(q, 20)) / 0.2  # ms per s over the last 200 ms
        slope[:20] = 0
        fire = np.where((slope > 100) & (q > 15))[0]
        tf = fire[0] if len(fire) else None
        leads.append(dict(link=NAMES[ln], share=share, crossed=cross is not None,
                          lead_ms=(cross - tf) * 10 if cross is not None and tf is not None else np.nan,
                          slope_fired=tf is not None))
        # (1) after a drop: 100 ms truth while saturated
        if sat:
            b = np.arange(1, dur - 1, 0.1)
            c = np.searchsorted(ta, b)
            tr = np.diff(c) * REC / 0.1 / 1e6  # Mbit/s per 100 ms
            # max filter 1.5 s over 40 ms samples resampled to 100 ms
            mx = pd.Series(win(0.04)).rolling(150, min_periods=50).max().values[::10][:len(tr)]
            # adaptive EWMA: alpha 0.1, 0.5 when the delay's 300 ms spread passes twice its long-run spread
            qq = q[::10][:len(tr)]
            jit = pd.Series(qq).rolling(3).std().values
            lj = np.nanmedian(jit) if np.isfinite(np.nanmedian(jit)) else 1
            ew, e = [], tr[0]
            for k, x in enumerate(tr):
                a = 0.5 if (np.isfinite(jit[k]) and jit[k] > 2 * max(lj, 1)) else 0.1
                e = e + a * (x - e)
                ew.append(e)
            ew = np.array(ew)
            n = min(len(tr), len(mx))
            over_mx = np.nansum(np.clip(mx[:n] - tr[:n], 0, None)) / np.nansum(tr[:n])
            over_ew = np.nansum(np.clip(ew[:n] - tr[:n], 0, None)) / np.nansum(tr[:n])
            under_ew = np.nansum(np.clip(tr[:n] - ew[:n], 0, None)) / np.nansum(tr[:n])
            forget.append(dict(link=NAMES[ln], share=share, over_max=over_mx, over_ewma=over_ew, under_ewma=under_ew,
                               cv100=np.nanstd(tr) / np.nanmean(tr)))
R = pd.DataFrame(rows); Q = pd.DataFrame(leads); F = pd.DataFrame(forget)
pd.set_option("display.width", 200); pd.set_option("display.max_rows", 200)
print(R.round(2).to_string(index=False))
sat = R[R.sat]
print("\nSATURATED steps (delivered = capacity): estimate / truth, mean (p90)")
print("  max-filter 1.5 s: %.2f (%.2f)   spacing 500 ms: %.2f (%.2f)   median 1 s: %.2f" % (
    sat.max_bias.mean(), sat.max_p90.mean(), sat.disp_bias.mean(), sat.disp_p90.mean(), sat.med_bias.mean()))
print("  per link:\n" + sat.groupby("link")[["max_bias", "disp_bias", "med_bias"]].mean().round(2).to_string())
uns = R[~R.sat]
print("UNSATURATED steps: max %.2f  spacing %.2f  median %.2f (truth = what was offered)" % (
    uns.max_bias.mean(), uns.disp_bias.mean(), uns.med_bias.mean()))
print("\nQUEUE SLOPE (fires at slope > 100 ms/s with queue > 15 ms; target 60 ms)")
c = Q[Q.crossed]
print("  steps crossing 60 ms: %d; slope fired before crossing in %d; lead p50 %.0f ms, p10 %.0f ms" % (
    len(c), (c.lead_ms > 0).sum(), c.lead_ms.median(), c.lead_ms.quantile(0.1)))
nc = Q[~Q.crossed]
print("  steps never crossing: %d; slope fired anyway (false alarm) in %d" % (len(nc), nc.slope_fired.sum()))
print("\nFORGETTING (saturated steps; over/under-estimate as a share of what was delivered)")
print("  max-filter over %.1f %%   adaptive EWMA over %.1f %%, under %.1f %%   100 ms CV %.2f" % (
    F.over_max.mean() * 100, F.over_ewma.mean() * 100, F.under_ewma.mean() * 100, F.cv100.mean()))
print(F.groupby("link")[["over_max", "over_ewma", "under_ewma", "cv100"]].mean().round(3).to_string())
