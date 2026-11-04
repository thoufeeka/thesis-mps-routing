#!/usr/bin/env python3
"""
summarize_cluster_results.py

Aggregates real MPS simulation benchmark outputs from the cluster run
(across bond dimensions, cost models, thread counts, and backends) into:
  1. results/real_sim_all_sweeps.csv
  2. results/Cluster_Real_Simulation_Report.md

Uses Python standard library only (zero external dependencies) so it can
run on any cluster node.
"""

import argparse
import csv
import glob
import math
import os
import sys
from collections import defaultdict


def safe_float(val, default=0.0):
    try:
        return float(val)
    except (ValueError, TypeError):
        return default


def safe_int(val, default=0):
    try:
        return int(float(val))
    except (ValueError, TypeError):
        return default


def mean(vals):
    return sum(vals) / len(vals) if vals else 0.0


def median(vals):
    if not vals:
        return 0.0
    s = sorted(vals)
    n = len(s)
    mid = n // 2
    if n % 2 == 1:
        return s[mid]
    return 0.5 * (s[mid - 1] + s[mid])


def circuit_family(name):
    lower = name.lower()
    if lower.startswith("qaoa"):
        return "QAOA"
    if lower.startswith("qft"):
        return "QFT"
    if lower.startswith("random"):
        return "Random"
    if lower.startswith("vqe"):
        return "VQE"
    return "Other"


def load_detailed_csvs(search_dirs):
    rows = []
    seen_files = set()
    for d in search_dirs:
        if not os.path.exists(d):
            continue
        pattern1 = os.path.join(d, "**", "*detailed*.csv")
        pattern2 = os.path.join(d, "*detailed*.csv")
        for path in sorted(glob.glob(pattern1, recursive=True) + glob.glob(pattern2)):
            abspath = os.path.abspath(path)
            if abspath in seen_files:
                continue
            seen_files.add(abspath)
            try:
                with open(abspath, "r", newline="") as f:
                    reader = csv.DictReader(f)
                    for row in reader:
                        if "circuit" in row and "bc_sim_ms_avg" in row:
                            row["_source_file"] = os.path.relpath(abspath)
                            rows.append(row)
            except Exception as exc:
                print(f"[WARN] Could not read {abspath}: {exc}", file=sys.stderr)
    return rows


def write_consolidated_csv(rows, out_path):
    if not rows:
        return
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    fieldnames = [k for k in rows[0].keys() if k != "_source_file"] + ["source_file"]
    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for r in rows:
            out_row = {k: r.get(k, "") for k in fieldnames if k != "source_file"}
            out_row["source_file"] = r.get("_source_file", "")
            writer.writerow(out_row)


def generate_markdown_report(rows, report_path):
    os.makedirs(os.path.dirname(os.path.abspath(report_path)), exist_ok=True)

    # Group by (backend, cost_model, threads, bond_dim)
    configs = defaultdict(list)
    for r in rows:
        key = (
            r.get("backend", "cpu"),
            r.get("cost_model", "cubic"),
            safe_int(r.get("threads", 1), 1),
            safe_int(r.get("bond_dim", 64), 64),
        )
        configs[key].append(r)

    lines = []
    lines.append("# Cluster Real MPS Simulation Benchmark Report")
    lines.append("")
    lines.append("Comparison of **BuildChain (BC)** (Maestro baseline) vs **RigidSeedLocalSearch (RS)**")
    lines.append("in real Matrix Product State simulation.")
    lines.append("")

    if not rows:
        lines.append("> No detailed simulation CSV rows found.")
        with open(report_path, "w") as f:
            f.write("\n".join(lines) + "\n")
        return

    # 1. Executive Summary Table across configurations
    lines.append("## 1. Executive Summary Across Configurations")
    lines.append("")
    lines.append("| Backend | Cost Model | Threads | Bond Dim ($\\chi$) | Circuits | Avg Dummy $\\Delta$% | Avg Real Swaps $\\Delta$% | Avg SVD Speedup % | Avg Wall-Clock Speedup % | Median Speedup % | RS Wins |")
    lines.append("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")

    for key in sorted(configs.keys()):
        backend, cost_model, threads, bond_dim = key
        c_rows = configs[key]
        dummy_deltas = [safe_float(r.get("dummy_cost_delta_pct")) for r in c_rows]
        swap_deltas = [safe_float(r.get("real_swaps_delta_pct")) for r in c_rows]
        svd_speedups = [safe_float(r.get("svd_speedup_pct")) for r in c_rows]
        sim_speedups = [safe_float(r.get("sim_speedup_pct")) for r in c_rows]
        wins = sum(1 for s in sim_speedups if s > 0.0)
        lines.append(
            f"| {backend} | {cost_model} | {threads} | {bond_dim} | {len(c_rows)} "
            f"| {mean(dummy_deltas):+.2f}% | {mean(swap_deltas):+.2f}% "
            f"| {mean(svd_speedups):+.2f}% | **{mean(sim_speedups):+.2f}%** "
            f"| {median(sim_speedups):+.2f}% | {wins}/{len(c_rows)} |"
        )
    lines.append("")

    # 2. Breakdown by Circuit Family
    lines.append("## 2. Breakdown by Circuit Family")
    lines.append("")
    lines.append("| Backend | Cost Model | Bond Dim ($\\chi$) | Family | Circuits | Avg Dummy $\\Delta$% | Avg Real Swaps $\\Delta$% | Avg Wall-Clock Speedup % |")
    lines.append("|---|---|---:|---|---:|---:|---:|---:|")

    for key in sorted(configs.keys()):
        backend, cost_model, threads, bond_dim = key
        by_fam = defaultdict(list)
        for r in configs[key]:
            by_fam[circuit_family(r.get("circuit", ""))].append(r)
        for fam in sorted(by_fam.keys()):
            f_rows = by_fam[fam]
            dd = mean([safe_float(r.get("dummy_cost_delta_pct")) for r in f_rows])
            sd = mean([safe_float(r.get("real_swaps_delta_pct")) for r in f_rows])
            sp = mean([safe_float(r.get("sim_speedup_pct")) for r in f_rows])
            lines.append(
                f"| {backend} ({threads}T) | {cost_model} | {bond_dim} | {fam} | {len(f_rows)} "
                f"| {dd:+.2f}% | {sd:+.2f}% | **{sp:+.2f}%** |"
            )
    lines.append("")

    # 3. Per-Circuit Detailed Tables
    lines.append("## 3. Per-Circuit Results by Configuration")
    lines.append("")

    for key in sorted(configs.keys()):
        backend, cost_model, threads, bond_dim = key
        c_rows = configs[key]
        lines.append(f"### Backend: `{backend}` | Cost Model: `{cost_model}` | Bond Dim: `{bond_dim}` | Threads: `{threads}`")
        lines.append("")
        lines.append("| Circuit | Qubits | Layers | 2Q Gates | BC Dummy | RS Dummy | Dummy $\\Delta$% | BC Swaps | RS Swaps | BC Peak $\\chi$ | RS Peak $\\chi$ | BC SVD (ms) | RS SVD (ms) | BC Sim (ms) | RS Sim (ms) | Speedup % | Speedup $\\times$ |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for r in c_rows:
            c = r.get("circuit", "")
            q = safe_int(r.get("qubits"))
            lay = safe_int(r.get("layers"))
            g2 = safe_int(r.get("two_qubit_gates"))
            bcd = safe_float(r.get("bc_dummy_cost"))
            rsd = safe_float(r.get("rs_dummy_cost"))
            dd = safe_float(r.get("dummy_cost_delta_pct"))
            bcs = safe_int(r.get("bc_real_swaps"))
            rss = safe_int(r.get("rs_real_swaps"))
            bcp = safe_int(r.get("bc_real_peak_bond"))
            rsp = safe_int(r.get("rs_real_peak_bond"))
            bcsvd = safe_float(r.get("bc_svd_ms"))
            rssvd = safe_float(r.get("rs_svd_ms"))
            bcsim = safe_float(r.get("bc_sim_ms_avg"))
            rssim = safe_float(r.get("rs_sim_ms_avg"))
            sp = safe_float(r.get("sim_speedup_pct"))
            spx = safe_float(r.get("sim_speedup_x"), 1.0)
            lines.append(
                f"| `{c}` | {q} | {lay} | {g2} | {bcd:.1f} | {rsd:.1f} | {dd:+.1f}% "
                f"| {bcs} | {rss} | {bcp} | {rsp} | {bcsvd:.2f} | {rssvd:.2f} "
                f"| {bcsim:.2f} | {rssim:.2f} | **{sp:+.2f}%** | {spx:.2f}x |"
            )
        lines.append("")

    with open(report_path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description="Aggregate cluster real simulation results.")
    parser.add_argument(
        "--dirs",
        nargs="+",
        default=["results"],
        help="Directories to search for detailed real simulation CSV files.",
    )
    parser.add_argument(
        "--csv-out",
        default="results/real_sim_all_sweeps.csv",
        help="Consolidated CSV output path.",
    )
    parser.add_argument(
        "--report-out",
        default="results/Cluster_Real_Simulation_Report.md",
        help="Markdown summary report path.",
    )
    args = parser.parse_args()

    rows = load_detailed_csvs(args.dirs)
    print(f"[summarize_cluster_results] Loaded {len(rows)} circuit-run rows.")
    write_consolidated_csv(rows, args.csv_out)
    generate_markdown_report(rows, args.report_out)
    print(f"[summarize_cluster_results] Wrote consolidated CSV to: {args.csv_out}")
    print(f"[summarize_cluster_results] Wrote Markdown report to:  {args.report_out}")


if __name__ == "__main__":
    main()

