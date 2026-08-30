#!/usr/bin/env python3
"""
generate_figures.py — Produces Figures 1-8 as publication-quality PNGs.
Requires: numpy, scipy, matplotlib
"""
import csv, math, os, sys, json
from collections import defaultdict

os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")
os.makedirs("results/figures", exist_ok=True)

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from scipy.stats import spearmanr

TRACE_FILE   = "mps_operation_trace.csv"
CIRCUIT_FILE = "benchmark_results_costmodel.csv"
AB_FILE      = "results/ab_routing_results.csv"
SHAPE_FILE   = "results/tables/shape_grouped_analysis.csv"
TABLE1_FILE  = "results/tables/table1_operation_level.csv"

STYLE = {
    "cubic": {"color": "#E06C75", "label": r"$C_{\rm cubic} = \chi_C^3$"},
    "svd":   {"color": "#61AFEF", "label": r"$C_{\rm svd} = \chi_L\chi_R\min(\chi_L,\chi_R)$"},
}
plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 11,
    "axes.labelsize": 12,
    "axes.titlesize": 13,
    "figure.dpi": 150,
    "axes.spines.top": False,
    "axes.spines.right": False,
})

def _f(s):
    try: return float(s)
    except: return None

# Load trace
print("Loading trace data for figures...")
rows = []
with open(TRACE_FILE, newline="") as f:
    for row in csv.DictReader(f):
        cc = _f(row["cost_cubic_real"]); cs = _f(row["cost_svd_real"]); t = _f(row["svd_time_ns"])
        if cc and cs and t and t > 0:
            row["_cc"] = cc; row["_cs"] = cs; row["_t"] = t
            rows.append(row)

cost_c = np.array([r["_cc"] for r in rows])
cost_s = np.array([r["_cs"] for r in rows])
times  = np.array([r["_t"]  for r in rows])

# Figure 1: log10(C_cubic) vs log10(measured SVD time)
print("Figure 1: C_cubic vs SVD time (operation-level)...")
fig, ax = plt.subplots(figsize=(7, 5))
ax.hexbin(np.log10(np.maximum(cost_c, 1e-9)), np.log10(np.maximum(times, 1)),
          gridsize=60, cmap="Reds", mincnt=1, linewidths=0.0)
rho, _ = spearmanr(cost_c, times)
ax.set_xlabel(r"$\log_{10}(C_{\rm cubic})$")
ax.set_ylabel(r"$\log_{10}(\text{SVD time [ns]})$")
ax.set_title(f"Figure 1: $C_{{\\rm cubic}} = \\chi_C^3$ vs Measured SVD Time\n"
             f"(N={len(rows):,}, Spearman ρ={rho:.4f})")
ax.text(0.05, 0.95, f"ρ = {rho:.4f}", transform=ax.transAxes, va="top",
        fontsize=12, color="#E06C75", fontweight="bold")
plt.tight_layout()
plt.savefig("results/figures/fig1_ccubic_vs_svdtime.png")
plt.close()

# Figure 2: log10(C_svd) vs log10(measured SVD time)
print("Figure 2: C_svd vs SVD time (operation-level)...")
fig, ax = plt.subplots(figsize=(7, 5))
ax.hexbin(np.log10(np.maximum(cost_s, 1e-9)), np.log10(np.maximum(times, 1)),
          gridsize=60, cmap="Blues", mincnt=1, linewidths=0.0)
rho_s, _ = spearmanr(cost_s, times)
ax.set_xlabel(r"$\log_{10}(C_{\rm svd})$")
ax.set_ylabel(r"$\log_{10}(\text{SVD time [ns]})$")
ax.set_title(f"Figure 2: $C_{{\\rm svd}} = \\chi_L\\chi_R\\min(\\chi_L,\\chi_R)$ vs Measured SVD Time\n"
             f"(N={len(rows):,}, Spearman ρ={rho_s:.4f})")
ax.text(0.05, 0.95, f"ρ = {rho_s:.4f}", transform=ax.transAxes, va="top",
        fontsize=12, color="#61AFEF", fontweight="bold")
plt.tight_layout()
plt.savefig("results/figures/fig2_csvd_vs_svdtime.png")
plt.close()

# Figure 3: Spearman ρ bar chart across subsets
print("Figure 3: Spearman ρ comparison bar chart...")
if os.path.exists(TABLE1_FILE):
    t1_rows = []
    with open(TABLE1_FILE, newline="") as f:
        for r in csv.DictReader(f): t1_rows.append(r)

    labels    = [r["subset"] for r in t1_rows]
    rho_cubic = [_f(r["cubic_rho"]) for r in t1_rows]
    rho_svd   = [_f(r["svd_rho"])   for r in t1_rows]
    n_subsets = len(labels)

    fig, ax = plt.subplots(figsize=(max(10, n_subsets*0.9), 5))
    x = np.arange(n_subsets)
    w = 0.35
    b1 = ax.bar(x - w/2, rho_cubic, w, color=STYLE["cubic"]["color"], label=r"$C_{\rm cubic}$", alpha=0.85)
    b2 = ax.bar(x + w/2, rho_svd,   w, color=STYLE["svd"]["color"],   label=r"$C_{\rm svd}$",   alpha=0.85)
    ax.set_xticks(x); ax.set_xticklabels(labels, rotation=35, ha="right", fontsize=9)
    ax.set_ylim(0.5, 1.02)
    ax.set_ylabel("Spearman ρ")
    ax.set_title("Figure 3: Spearman Rank Correlation — Cubic vs SVD Cost Model")
    ax.legend(loc="lower right")
    ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.8)
    for bar in list(b1)+list(b2):
        ax.annotate(f"{bar.get_height():.3f}", xy=(bar.get_x()+bar.get_width()/2, bar.get_height()),
                    xytext=(0, 2), textcoords="offset points", ha="center", va="bottom", fontsize=7)
    plt.tight_layout()
    plt.savefig("results/figures/fig3_spearman_comparison.png")
    plt.close()

# Figure 4: Shape-grouped predicted cost vs median measured time
print("Figure 4: Shape-grouped analysis...")
if os.path.exists(SHAPE_FILE):
    srecs = []
    with open(SHAPE_FILE, newline="") as f:
        for r in csv.DictReader(f): srecs.append(r)

    med_t  = np.array([_f(r["median_svd_time_ns"]) for r in srecs])
    rep_c  = np.array([_f(r["rep_cubic_cost"])  for r in srecs])
    rep_s  = np.array([_f(r["rep_svd_cost"])     for r in srecs])
    counts = np.array([int(r["count"]) for r in srecs])
    valid  = ~(np.isnan(med_t) | np.isnan(rep_c) | np.isnan(rep_s))

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    for ax, rep, style_key, fig_label in [
        (axes[0], rep_c, "cubic", "a"),
        (axes[1], rep_s, "svd",   "b"),
    ]:
        v = valid & ~np.isnan(rep)
        rho, _ = spearmanr(rep[v], med_t[v])
        sc = ax.scatter(np.log10(np.maximum(rep[v], 1e-6)), np.log10(np.maximum(med_t[v], 1)),
                        c=np.log10(counts[v]+1), cmap="viridis", s=30+counts[v]*0.2,
                        alpha=0.75, edgecolors="none")
        plt.colorbar(sc, ax=ax, label="log10(count)")
        key = "cubic" if style_key == "cubic" else "svd"
        ax.set_xlabel(f"log10(representative {style_key} cost)")
        ax.set_ylabel("log10(median SVD time [ns])")
        ax.set_title(f"({fig_label}) {STYLE[key]['label']}\nSpearman ρ = {rho:.4f}")
    fig.suptitle("Figure 4: Shape-Grouped Predicted Cost vs Measured Median SVD Time", fontsize=13)
    plt.tight_layout()
    plt.savefig("results/figures/fig4_shape_grouped.png")
    plt.close()

# Figure 5 & 6: Per-circuit end-to-end timing (from ab_routing_results.csv if available)
if os.path.exists(AB_FILE):
    print("Figure 5 & 6: Per-circuit A/B routing results...")
    ab_rows = []
    with open(AB_FILE, newline="") as f:
        for r in csv.DictReader(f): ab_rows.append(r)

    # Group by (circuit, cost_model) -> median svd time
    from collections import defaultdict
    circ_model = defaultdict(list)
    for r in ab_rows:
        circ_model[(r["circuit"], r["cost_model"])].append(_f(r["total_svd_time_ms"]) or 0)

    circuits_ab = sorted(set(r["circuit"] for r in ab_rows))
    med_cubic = []; med_svd = []; circ_labels = []
    for c in circuits_ab:
        mc = sorted(circ_model[(c,"cubic")])
        ms = sorted(circ_model[(c,"svd")])
        if not mc or not ms: continue
        med_cubic.append(mc[len(mc)//2])
        med_svd.append(ms[len(ms)//2])
        circ_labels.append(os.path.basename(c).replace(".qasm",""))

    # Figure 5
    fig, ax = plt.subplots(figsize=(10, 5))
    x = np.arange(len(circ_labels)); w = 0.35
    ax.bar(x - w/2, med_cubic, w, color=STYLE["cubic"]["color"], label=r"$C_{\rm cubic}$ routing", alpha=0.85)
    ax.bar(x + w/2, med_svd,   w, color=STYLE["svd"]["color"],   label=r"$C_{\rm svd}$ routing",   alpha=0.85)
    ax.set_xticks(x); ax.set_xticklabels(circ_labels, rotation=30, ha="right")
    ax.set_ylabel("Median Total SVD Time (ms)")
    ax.set_title("Figure 5: Per-Circuit Median Total SVD Time — Cubic vs SVD Routing")
    ax.legend()
    plt.tight_layout()
    plt.savefig("results/figures/fig5_per_circuit_svdtime.png")
    plt.close()

    # Figure 6: Delta %
    deltas = [100*(ms-mc)/mc if mc > 0 else float("nan") for mc, ms in zip(med_cubic, med_svd)]
    colors = ["#61AFEF" if d <= 0 else "#E06C75" for d in deltas]
    fig, ax = plt.subplots(figsize=(10, 5))
    bars = ax.bar(circ_labels, deltas, color=colors, alpha=0.85, edgecolor="white")
    ax.axhline(0, color="black", linewidth=1)
    ax.set_ylabel("Δ Total SVD Time (%)\n[negative = SVD routing faster]")
    ax.set_title("Figure 6: Percentage Change in Total SVD Time (SVD vs Cubic Routing)")
    plt.xticks(rotation=30, ha="right")
    for bar, d in zip(bars, deltas):
        ax.text(bar.get_x()+bar.get_width()/2, bar.get_height()+(0.3 if d>=0 else -1.2),
                f"{d:+.1f}%", ha="center", va="bottom", fontsize=9)
    plt.tight_layout()
    plt.savefig("results/figures/fig6_delta_svdtime.png")
    plt.close()

    # Figure 7: Tradeoff SWAP count vs SVD time change
    swap_cubic = defaultdict(list); swap_svd = defaultdict(list)
    for r in ab_rows:
        c = r["circuit"]; m = r["cost_model"]; sw = _f(r.get("inserted_swap_count", "")) or 0
        if m == "cubic": swap_cubic[c].append(sw)
        else: swap_svd[c].append(sw)

    delta_swaps = []; delta_svdtime = []
    for c, mc, ms in zip(circuits_ab, med_cubic, med_svd):
        sc = sorted(swap_cubic[c]); ss = sorted(swap_svd[c])
        if not sc or not ss: continue
        ds = ss[len(ss)//2] - sc[len(sc)//2]
        dt = 100*(ms - mc)/mc if mc > 0 else float("nan")
        delta_swaps.append(ds); delta_svdtime.append(dt)

    fig, ax = plt.subplots(figsize=(7, 5))
    ax.scatter(delta_swaps, delta_svdtime, color="#98C379", s=80, zorder=5, edgecolors="black", linewidth=0.5)
    ax.axhline(0, color="gray", linestyle="--", linewidth=0.8)
    ax.axvline(0, color="gray", linestyle="--", linewidth=0.8)
    for i, c in enumerate(circuits_ab):
        if i < len(delta_swaps):
            ax.annotate(os.path.basename(c).replace(".qasm",""), (delta_swaps[i], delta_svdtime[i]),
                       xytext=(5,5), textcoords="offset points", fontsize=8)
    ax.set_xlabel("Δ SWAP count (SVD − Cubic)")
    ax.set_ylabel("Δ Total SVD Time % (SVD − Cubic)")
    ax.set_title("Figure 7: Tradeoff — SWAP Count Change vs SVD Time Change")
    plt.tight_layout()
    plt.savefig("results/figures/fig7_tradeoff.png")
    plt.close()

    # Figure 8: Routing time overhead
    route_cubic = defaultdict(list); route_svd = defaultdict(list)
    for r in ab_rows:
        c = r["circuit"]; m = r["cost_model"]
        rt = _f(r.get("routing_optimization_time_ms","")) or 0
        if m == "cubic": route_cubic[c].append(rt)
        else: route_svd[c].append(rt)

    med_rc = []; med_rs = []
    for c in circuits_ab:
        rc_ = sorted(route_cubic[c]); rs_ = sorted(route_svd[c])
        if not rc_ or not rs_: continue
        med_rc.append(rc_[len(rc_)//2]); med_rs.append(rs_[len(rs_)//2])

    fig, ax = plt.subplots(figsize=(10, 5))
    x = np.arange(len(circ_labels)); w = 0.35
    ax.bar(x - w/2, med_rc, w, color=STYLE["cubic"]["color"], label=r"$C_{\rm cubic}$", alpha=0.85)
    ax.bar(x + w/2, med_rs, w, color=STYLE["svd"]["color"],   label=r"$C_{\rm svd}$",   alpha=0.85)
    ax.set_xticks(x); ax.set_xticklabels(circ_labels, rotation=30, ha="right")
    ax.set_ylabel("Median Routing Time (ms)")
    ax.set_title("Figure 8: Routing Optimization Time — Cubic vs SVD")
    ax.legend()
    plt.tight_layout()
    plt.savefig("results/figures/fig8_routing_time.png")
    plt.close()
else:
    print("  ab_routing_results.csv not found — Figures 5-8 skipped (run after Part 8 experiment)")

print("\nAll figures saved to results/figures/")
