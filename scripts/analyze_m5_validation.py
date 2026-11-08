#!/usr/bin/env python3
"""
Statistical analysis for Month 5 Deliverable (M5):
Computes correlation (Pearson r, Spearman rho) between:
  - Proxy cost reduction (-dummy_cost_delta_pct)
  - Real SWAP count reduction (-real_swaps_delta_pct)
  - Real SVD speedup (svd_speedup_pct)
  - Real wall-clock simulation speedup (sim_speedup_pct)
across all RigidSeedLocalSearch validation runs.
"""

import csv
import math
import os


def rank_data(vals):
    n = len(vals)
    indexed = sorted(enumerate(vals), key=lambda x: x[1])
    ranks = [0.0] * n
    i = 0
    while i < n:
        j = i
        while j + 1 < n and indexed[j + 1][1] == indexed[i][1]:
            j += 1
        avg_rank = 0.5 * (i + j) + 1.0
        for k in range(i, j + 1):
            ranks[indexed[k][0]] = avg_rank
        i = j + 1
    return ranks


def pearson_r(x, y):
    n = len(x)
    if n < 2:
        return 0.0
    mx = sum(x) / n
    my = sum(y) / n
    num = sum((xi - mx) * (yi - my) for xi, yi in zip(x, y))
    denx = math.sqrt(sum((xi - mx) ** 2 for xi in x))
    deny = math.sqrt(sum((yi - my) ** 2 for yi in y))
    if denx == 0 or deny == 0:
        return 0.0
    return num / (denx * deny)


def spearman_rho(x, y):
    return pearson_r(rank_data(x), rank_data(y))


def analyze_file(path, label):
    with open(path, "r", newline="") as f:
        rows = list(csv.DictReader(f))

    print(f"\n======================================================================")
    print(f" DATASET: {label} ({path})")
    print(f"======================================================================")

    for subset_name, filt in [
        ("All 10 RigidSeed cost-reduction circuits", lambda r: True),
        ("8 Non-trivial cost-reduction circuits (|delta| >= 3.2%, excl. qft_30/40 1-swap ties)", lambda r: abs(float(r["dummy_cost_delta_pct"])) >= 1.0),
    ]:
        sub = [r for r in rows if filt(r)]
        proxy_red = [-float(r["dummy_cost_delta_pct"]) for r in sub]
        swap_red = [-float(r["real_swaps_delta_pct"]) for r in sub]
        svd_sp = [float(r["svd_speedup_pct"]) for r in sub]
        sim_sp = [float(r["sim_speedup_pct"]) for r in sub]
        sim_min_sp = [
            (float(r["bc_sim_ms_min"]) - float(r["rs_sim_ms_min"])) / float(r["bc_sim_ms_min"]) * 100.0
            for r in sub
        ]

        total_bc_ms = sum(float(r["bc_sim_ms_avg"]) for r in sub)
        total_rs_ms = sum(float(r["rs_sim_ms_avg"]) for r in sub)
        agg_speedup_pct = (total_bc_ms - total_rs_ms) / total_bc_ms * 100.0
        agg_speedup_x = total_bc_ms / total_rs_ms if total_rs_ms > 0 else 1.0

        wins_avg = sum(1 for s in sim_sp if s > 0)
        wins_min = sum(1 for s in sim_min_sp if s > 0)
        wins_svd = sum(1 for s in svd_sp if s > 0)

        print(f"\n--- Subset: {subset_name} (N={len(sub)}) ---")
        print(f"  Avg Proxy Cost Reduction : {sum(proxy_red)/len(sub):+.2f}%")
        print(f"  Avg Real SWAP Reduction  : {sum(swap_red)/len(sub):+.2f}%")
        print(f"  Avg SVD Speedup          : {sum(svd_sp)/len(sub):+.2f}%  (Wins: {wins_svd}/{len(sub)})")
        print(f"  Avg Wall-Clock Speedup   : {sum(sim_sp)/len(sub):+.2f}%  (Wins: {wins_avg}/{len(sub)})")
        print(f"  Avg Min-Time Speedup     : {sum(sim_min_sp)/len(sub):+.2f}%  (Wins: {wins_min}/{len(sub)})")
        print(f"  Total Suite Time (BC->RS): {total_bc_ms:.1f} ms -> {total_rs_ms:.1f} ms ({agg_speedup_pct:+.2f}%, {agg_speedup_x:.2f}x)")
        print(f"  Correlations:")
        print(f"    Proxy Cost Red. vs Real SWAP Red. : Pearson r = {pearson_r(proxy_red, swap_red):+.4f}, Spearman rho = {spearman_rho(proxy_red, swap_red):+.4f}")
        print(f"    Proxy Cost Red. vs Real SVD Spd.  : Pearson r = {pearson_r(proxy_red, svd_sp):+.4f}, Spearman rho = {spearman_rho(proxy_red, svd_sp):+.4f}")
        print(f"    Proxy Cost Red. vs Wall-Clock Spd.: Pearson r = {pearson_r(proxy_red, sim_sp):+.4f}, Spearman rho = {spearman_rho(proxy_red, sim_sp):+.4f}")
        print(f"    Real SWAP Red.  vs Wall-Clock Spd.: Pearson r = {pearson_r(swap_red, sim_sp):+.4f}, Spearman rho = {spearman_rho(swap_red, sim_sp):+.4f}")
        print(f"    Real SVD Spd.   vs Wall-Clock Spd.: Pearson r = {pearson_r(svd_sp, sim_sp):+.4f}, Spearman rho = {spearman_rho(svd_sp, sim_sp):+.4f}")


if __name__ == "__main__":
    analyze_file("results/real_sim_detailed_results.csv", "Cubic, chi=64, 1T")
    analyze_file("results/real_sim_rigidseed_svd_chi64_1T_detailed.csv", "ThinSVDGeometry (svd), chi=64, 1T")
    analyze_file("results/real_sim_rigidseed_cubic_chi32_1T_detailed.csv", "Cubic, chi=32, 1T")
    analyze_file("results/real_sim_rigidseed_cubic_chi64_8T_detailed.csv", "Cubic, chi=64, 8T")

