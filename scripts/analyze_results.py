#!/usr/bin/env python3
"""
Loads mps_operation_trace.csv and computes SVD timing statistics,
Spearman correlations, and a circuit-level summary. Uses standard library only.
"""

import os
import sys
import csv
import math
from collections import defaultdict

TRACE_FILE = "mps_operation_trace.csv"
BENCHMARK_FILE = "benchmark_results_costmodel.csv"

def percentile(data, percent):
    if not data:
        return 0.0
    sorted_data = sorted(data)
    k = (len(sorted_data) - 1) * (percent / 100.0)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return float(sorted_data[int(k)])
    d0 = sorted_data[int(f)] * (c - k)
    d1 = sorted_data[int(c)] * (k - f)
    return float(d0 + d1)

def rank_data(data):
    n = len(data)
    indexed = sorted(enumerate(data), key=lambda x: x[1])
    ranks = [0.0] * n
    i = 0
    while i < n:
        j = i
        while j < n - 1 and indexed[j + 1][1] == indexed[j][1]:
            j += 1
        rank = (i + j + 2) / 2.0
        for k in range(i, j + 1):
            ranks[indexed[k][0]] = rank
        i = j + 1
    return ranks

def spearman_rho(x, y):
    n = len(x)
    if n < 2:
        return 0.0
    rx = rank_data(x)
    ry = rank_data(y)
    
    mean_rx = sum(rx) / n
    mean_ry = sum(ry) / n
    
    num = sum((rx[i] - mean_rx) * (ry[i] - mean_ry) for i in range(n))
    den_x = math.sqrt(sum((rx[i] - mean_rx) ** 2 for i in range(n)))
    den_y = math.sqrt(sum((ry[i] - mean_ry) ** 2 for i in range(n)))
    
    if den_x == 0 or den_y == 0:
        return 0.0
    return num / (den_x * den_y)

def main():
    if not os.path.exists(TRACE_FILE):
        print(f"Error: {TRACE_FILE} not found. Run benchmark first.")
        sys.exit(1)

    records = []
    with open(TRACE_FILE, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            records.append({
                'circuit': row['circuit'],
                'repetition': int(row['repetition']),
                'cost_model': row['cost_model'],
                'operation_index': int(row['operation_index']),
                'gate_type': row['gate_type'],
                'is_swap': int(row['is_swap']),
                'bond': int(row['bond']),
                'chi_left': float(row['chi_left']),
                'chi_center_before': float(row['chi_center_before']),
                'chi_right': float(row['chi_right']),
                'chi_center_after': float(row['chi_center_after']),
                'svd_rows': int(row['svd_rows']),
                'svd_cols': int(row['svd_cols']),
                'svd_solver': row['svd_solver'],
                'cost_cubic_real': float(row['cost_cubic_real']),
                'cost_svd_real': float(row['cost_svd_real']),
                'svd_time_ns': float(row['svd_time_ns']),
            })

    print(f"Loaded {len(records)} SVD operation records from {TRACE_FILE}.")

    # 1. Verification checks
    print("\n--- SANITY / INSTRUMENTATION VERIFICATION ---")
    circuits = sorted(list(set(r['circuit'] for r in records)))
    models = sorted(list(set(r['cost_model'] for r in records)))
    solvers = sorted(list(set(r['svd_solver'] for r in records)))
    print(f"Unique circuits: {circuits}")
    print(f"Cost models present: {models}")
    print(f"Solvers present: {solvers}")

    row_check = all(r['svd_rows'] == int(2 * r['chi_left']) for r in records)
    col_check = all(r['svd_cols'] == int(2 * r['chi_right']) for r in records)
    print(f"Verification - SVD rows == 2*chi_left: {row_check}")
    print(f"Verification - SVD cols == 2*chi_right: {col_check}")
    
    svd_times = [r['svd_time_ns'] for r in records]
    print(f"Min svd_time_ns: {min(svd_times)} ns, Max svd_time_ns: {max(svd_times)} ns")

    # 2. Shape-grouped SVD statistics
    print("\n--- SHAPE-GROUPED SVD TIMING STATISTICS ---")
    shape_groups = defaultdict(list)
    for r in records:
        key = (r['svd_rows'], r['svd_cols'], r['svd_solver'])
        shape_groups[key].append(r)

    print(f"{'Rows':<6} {'Cols':<6} {'Solver':<8} {'Count':<8} {'Median(ns)':<12} {'Q1(ns)':<10} {'Q3(ns)':<10} {'C_cubic_mean':<14} {'C_svd_mean':<14}")
    print("-" * 90)

    grouped_data = []
    for key, group in sorted(shape_groups.items()):
        rows, cols, solver = key
        times = [g['svd_time_ns'] for g in group]
        c_cubics = [g['cost_cubic_real'] for g in group]
        c_svds = [g['cost_svd_real'] for g in group]
        
        med_t = percentile(times, 50)
        q1_t = percentile(times, 25)
        q3_t = percentile(times, 75)
        mean_c = sum(c_cubics) / len(c_cubics)
        mean_s = sum(c_svds) / len(c_svds)

        grouped_data.append({
            'rows': rows, 'cols': cols, 'solver': solver,
            'count': len(times), 'median_time': med_t,
            'mean_cubic': mean_c, 'mean_svd': mean_s
        })

        print(f"{rows:<6} {cols:<6} {solver:<8} {len(times):<8} {med_t:<12.1f} {q1_t:<10.1f} {q3_t:<10.1f} {mean_c:<14.1f} {mean_s:<14.1f}")

    # 3. Spearman Correlations
    print("\n--- SPEARMAN CORRELATION ANALYSIS ---")
    all_cubic = [r['cost_cubic_real'] for r in records]
    all_svd = [r['cost_svd_real'] for r in records]
    all_times = [r['svd_time_ns'] for r in records]

    rho_cubic = spearman_rho(all_cubic, all_times)
    rho_svd = spearman_rho(all_svd, all_times)

    print("Overall (Per-Operation Raw):")
    print(f"  cost_cubic_real vs svd_time_ns: Spearman rho = {rho_cubic:.4f}")
    print(f"  cost_svd_real   vs svd_time_ns: Spearman rho = {rho_svd:.4f}")

    # Shape-Grouped correlation (median per matrix shape)
    if len(grouped_data) > 1:
        grp_cubic = [g['mean_cubic'] for g in grouped_data]
        grp_svd = [g['mean_svd'] for g in grouped_data]
        grp_times = [g['median_time'] for g in grouped_data]

        rho_grp_cubic = spearman_rho(grp_cubic, grp_times)
        rho_grp_svd = spearman_rho(grp_svd, grp_times)

        print("\nShape-Grouped (Median per Matrix Shape):")
        print(f"  cost_cubic_real vs median_time_ns: Spearman rho = {rho_grp_cubic:.4f}")
        print(f"  cost_svd_real   vs median_time_ns: Spearman rho = {rho_grp_svd:.4f}")

    # By Solver path
    print("\nBy SVD Solver Path:")
    for solver in solvers:
        sub = [r for r in records if r['svd_solver'] == solver]
        if len(sub) > 1:
            r_c = spearman_rho([s['cost_cubic_real'] for s in sub], [s['svd_time_ns'] for s in sub])
            r_s = spearman_rho([s['cost_svd_real'] for s in sub], [s['svd_time_ns'] for s in sub])
            print(f"  Solver '{solver}' (N={len(sub)}):")
            print(f"    cost_cubic_real vs svd_time_ns: rho = {r_c:.4f}")
            print(f"    cost_svd_real   vs svd_time_ns: rho = {r_s:.4f}")

    # 4. Circuit-level summary
    if os.path.exists(BENCHMARK_FILE):
        print("\n--- CIRCUIT-LEVEL SUMMARY ---")
        with open(BENCHMARK_FILE, "r") as f:
            reader = csv.DictReader(f)
            b_recs = list(reader)
        print(f"{'Circuit':<35} {'Model':<8} {'Reps':<5} {'SVD(ms)':<10} {'Sim(ms)':<10} {'Route(ms)':<10} {'PeakBD':<8} {'Swaps':<8}")
        print("-" * 105)
        for r in b_recs:
            print(f"{r['circuit']:<35} {r['cost_model']:<8} {r['repetition']:<5} {float(r['total_svd_time_ms']):<10.3f} {float(r['total_simulation_time_ms']):<10.3f} {float(r['routing_optimization_time_ms']):<10.3f} {r['real_peak_bond_dim']:<8} {r['inserted_swap_count']:<8}")

if __name__ == "__main__":
    main()
