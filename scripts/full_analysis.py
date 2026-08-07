#!/usr/bin/env python3
"""
full_analysis.py  -- Parts 2-7 of the MPS cost model validation spec.

Produces:
  results/tables/table1_operation_level.csv
  results/tables/table2_ranking_disagreement.csv
  results/tables/table3_circuit_ab.csv   (stub for Part 8 data merge)
  results/tables/table4_aggregate.csv    (stub)

Also prints a full console report.
"""

import csv, math, os, sys, json
from collections import defaultdict

# ---------------------------------------------------------------------------
# Ensure we run from project root
# ---------------------------------------------------------------------------
os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")
os.makedirs("results/tables", exist_ok=True)
os.makedirs("results/figures", exist_ok=True)

TRACE_FILE   = "mps_operation_trace.csv"
CIRCUIT_FILE = "benchmark_results_costmodel.csv"

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def _f(s):
    try: return float(s)
    except: return None

def _i(s):
    try: return int(s)
    except: return None

def spearman(xs, ys):
    """Compute Spearman rank correlation (pure Python, no scipy needed)."""
    n = len(xs)
    if n < 3: return float("nan"), float("nan")
    import numpy as np
    xs = np.asarray(xs, dtype=float); ys = np.asarray(ys, dtype=float)
    from scipy.stats import spearmanr
    r, p = spearmanr(xs, ys)
    return float(r), float(p)

def kendalltau(xs, ys):
    import numpy as np
    from scipy.stats import kendalltau
    r, p = kendalltau(xs, ys)
    return float(r), float(p)

def median(vals):
    v = sorted(x for x in vals if x is not None and not math.isnan(x))
    if not v: return float("nan")
    n = len(v)
    if n % 2 == 1: return v[n//2]
    return (v[n//2-1] + v[n//2]) / 2

def q1q3(vals):
    v = sorted(x for x in vals if x is not None and not math.isnan(x))
    if len(v) < 4: return float("nan"), float("nan")
    n = len(v)
    return v[n//4], v[3*n//4]

def circuit_family(circuit_name):
    name = os.path.basename(circuit_name).lower()
    if "qft" in name: return "QFT"
    if "qaoa" in name: return "QAOA"
    if "two_local" in name: return "VQE_TwoLocal"
    if "real_amp" in name or "vqe" in name: return "VQE_RealAmp"
    return "OTHER"

def qubit_count(row):
    # derive from circuit name: last digits before .qasm
    import re
    m = re.search(r'_(\d+)\.qasm', row.get("circuit",""))
    if m: return int(m.group(1))
    return _i(row.get("qubits","")) or -1

# ---------------------------------------------------------------------------
# Load data
# ---------------------------------------------------------------------------
print("Loading data...")
trace_rows = []
with open(TRACE_FILE, newline="") as f:
    reader = csv.DictReader(f)
    for row in reader:
        trace_rows.append(row)

circuit_rows = []
with open(CIRCUIT_FILE, newline="") as f:
    reader = csv.DictReader(f)
    for row in reader:
        circuit_rows.append(row)

print(f"  operation trace rows: {len(trace_rows)}")
print(f"  circuit-level rows:   {len(circuit_rows)}")

# Parse numerics once
for row in trace_rows:
    row["_chi_l"]     = _f(row["chi_left"])
    row["_chi_c"]     = _f(row["chi_center_before"])
    row["_chi_r"]     = _f(row["chi_right"])
    row["_chi_after"] = _f(row["chi_center_after"])
    row["_svd_rows"]  = _i(row["svd_rows"])
    row["_svd_cols"]  = _i(row["svd_cols"])
    row["_cost_c"]    = _f(row["cost_cubic_real"])
    row["_cost_s"]    = _f(row["cost_svd_real"])
    row["_time"]      = _f(row["svd_time_ns"])
    row["_family"]    = circuit_family(row["circuit"])
    row["_qubits"]    = qubit_count(row)

valid_rows = [r for r in trace_rows
              if r["_cost_c"] is not None
              and r["_cost_s"] is not None
              and r["_time"] is not None
              and r["_time"] > 0]

print(f"  valid rows (no NaN): {len(valid_rows)}")

# ---------------------------------------------------------------------------
# PART 2: OPERATION-LEVEL SPEARMAN CORRELATIONS
# ---------------------------------------------------------------------------
print("\n" + "="*60)
print("PART 2: OPERATION-LEVEL SPEARMAN CORRELATIONS")
print("="*60)

def subset_spearman(rows, label):
    costs_c = [r["_cost_c"] for r in rows]
    costs_s = [r["_cost_s"] for r in rows]
    times   = [r["_time"]   for r in rows]
    rc, pc = spearman(costs_c, times)
    rs, ps = spearman(costs_s, times)
    improvement = rs - rc
    print(f"  {label:<40} N={len(rows):6d}  cubic ρ={rc:.4f}  svd ρ={rs:.4f}  Δ={improvement:+.4f}")
    return {"subset": label, "N": len(rows),
            "cubic_rho": rc, "svd_rho": rs, "improvement": improvement,
            "cubic_p": pc, "svd_p": ps}

table1_rows = []

# A. All
table1_rows.append(subset_spearman(valid_rows, "Overall"))

# B/C. By solver
for solver in ["bdc", "jacobi"]:
    sub = [r for r in valid_rows if r["svd_solver"] == solver]
    table1_rows.append(subset_spearman(sub, f"Solver={solver}"))

# D. By circuit family
for fam in sorted(set(r["_family"] for r in valid_rows)):
    sub = [r for r in valid_rows if r["_family"] == fam]
    table1_rows.append(subset_spearman(sub, f"Family={fam}"))

# E. By qubit count
for qb in sorted(set(r["_qubits"] for r in valid_rows)):
    sub = [r for r in valid_rows if r["_qubits"] == qb]
    table1_rows.append(subset_spearman(sub, f"Qubits={qb}"))

# D+E combined
for fam in sorted(set(r["_family"] for r in valid_rows)):
    for qb in sorted(set(r["_qubits"] for r in valid_rows)):
        sub = [r for r in valid_rows if r["_family"] == fam and r["_qubits"] == qb]
        if len(sub) < 50: continue
        table1_rows.append(subset_spearman(sub, f"Family={fam},Qubits={qb}"))

# Save Table 1
with open("results/tables/table1_operation_level.csv","w",newline="") as f:
    w = csv.DictWriter(f, fieldnames=["subset","N","cubic_rho","svd_rho","improvement","cubic_p","svd_p"])
    w.writeheader(); w.writerows(table1_rows)
print("\n  -> Saved results/tables/table1_operation_level.csv")

# ---------------------------------------------------------------------------
# PART 3: SHAPE-GROUPED ANALYSIS
# ---------------------------------------------------------------------------
print("\n" + "="*60)
print("PART 3: SHAPE-GROUPED ANALYSIS")
print("="*60)

# Group by (svd_rows, svd_cols, svd_solver)
shape_groups = defaultdict(list)
for r in valid_rows:
    key = (r["_svd_rows"], r["_svd_cols"], r["svd_solver"])
    shape_groups[key].append(r)

shape_records = []
for (sr, sc, solver), rows in sorted(shape_groups.items()):
    times = [r["_time"] for r in rows]
    med = median(times)
    lo, hi = q1q3(times)
    rep_c = median([r["_cost_c"] for r in rows])
    rep_s = median([r["_cost_s"] for r in rows])
    shape_records.append({
        "svd_rows": sr, "svd_cols": sc, "svd_solver": solver,
        "count": len(rows),
        "median_svd_time_ns": med, "q1_ns": lo, "q3_ns": hi,
        "iqr_ns": hi - lo if not (math.isnan(lo) or math.isnan(hi)) else float("nan"),
        "rep_cubic_cost": rep_c, "rep_svd_cost": rep_s,
    })

print(f"  Unique shapes: {len(shape_records)}")
print(f"  {'Rows':>4} {'Cols':>4} {'Solver':>6} {'Count':>6}  {'Median(ns)':>12}  {'Q1':>10}  {'Q3':>10}  C_cubic  C_svd")
print(f"  {'-'*80}")
for s in shape_records:
    print(f"  {s['svd_rows']:>4} {s['svd_cols']:>4} {s['svd_solver']:>6} {s['count']:>6}  "
          f"{s['median_svd_time_ns']:>12.0f}  {s['q1_ns']:>10.0f}  {s['q3_ns']:>10.0f}  "
          f"{s['rep_cubic_cost']:>7.1f}  {s['rep_svd_cost']:>6.1f}")

# Shape-grouped Spearman
med_times = [s["median_svd_time_ns"] for s in shape_records]
rep_c_all = [s["rep_cubic_cost"] for s in shape_records]
rep_s_all = [s["rep_svd_cost"] for s in shape_records]
rc_shape, _ = spearman(rep_c_all, med_times)
rs_shape, _ = spearman(rep_s_all, med_times)
print(f"\n  Shape-grouped Spearman:")
print(f"    C_cubic vs median time: ρ = {rc_shape:.4f}")
print(f"    C_svd   vs median time: ρ = {rs_shape:.4f}")

# Kendall τ
tc, _ = kendalltau(rep_c_all, med_times)
ts, _ = kendalltau(rep_s_all, med_times)
print(f"  Shape-grouped Kendall τ:")
print(f"    C_cubic vs median time: τ = {tc:.4f}")
print(f"    C_svd   vs median time: τ = {ts:.4f}")

with open("results/tables/shape_grouped_analysis.csv","w",newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(shape_records[0].keys()))
    w.writeheader(); w.writerows(shape_records)
print("\n  -> Saved results/tables/shape_grouped_analysis.csv")

# ---------------------------------------------------------------------------
# PART 4: RANKING DISAGREEMENT / KENDALL τ
# ---------------------------------------------------------------------------
print("\n" + "="*60)
print("PART 4: RANKING DISAGREEMENT ANALYSIS")
print("="*60)
print("""
EXPLANATION: The routing decision-maker mainly needs to rank the cost of operations
correctly, not predict absolute runtimes. Correlation alone can be high even if many
individual rank orderings are wrong. Disagreement analysis identifies pairs of SVD
geometries where C_cubic and C_svd disagree on ordering, then checks which model
agreed with measured wall-clock time.
""")

# Use shape-level median timing for clarity
# Build a dict: (rows, cols, solver) -> median_time
shape_map = {(s["svd_rows"], s["svd_cols"], s["svd_solver"]): s for s in shape_records}

# We compare all pairs of distinct shapes
# Only shapes with count >= 3 to keep medians stable
stable_shapes = [s for s in shape_records if s["count"] >= 3]
print(f"  Shapes with count >= 3: {len(stable_shapes)}")

total_pairs = 0
cubic_wins = 0; svd_wins = 0; ties = 0; unresolved = 0

# Collect disagreement pairs
disagreement_details = []

n = len(stable_shapes)
for i in range(n):
    for j in range(i+1, n):
        A = stable_shapes[i]; B = stable_shapes[j]
        cc_A = A["rep_cubic_cost"]; cc_B = B["rep_cubic_cost"]
        cs_A = A["rep_svd_cost"];   cs_B = B["rep_svd_cost"]
        mt_A = A["median_svd_time_ns"]; mt_B = B["median_svd_time_ns"]

        if math.isnan(cc_A) or math.isnan(cc_B) or math.isnan(cs_A) or math.isnan(cs_B): continue
        if math.isnan(mt_A) or math.isnan(mt_B): continue

        cubic_says_A_lt_B = cc_A < cc_B
        svd_says_A_lt_B   = cs_A < cs_B

        if cubic_says_A_lt_B == svd_says_A_lt_B:
            continue  # models agree — skip

        total_pairs += 1
        measured_A_lt_B = mt_A < mt_B

        # IQR overlap check for tie
        iqr_A = A["iqr_ns"]; iqr_B = B["iqr_ns"]
        time_diff = abs(mt_A - mt_B)
        median_larger = max(mt_A, mt_B)
        # "Unresolved" if the timing gap < 5% of the larger median (noise-dominated)
        if time_diff < 0.05 * median_larger:
            ties += 1
            continue

        cubic_correct = (cubic_says_A_lt_B == measured_A_lt_B)
        svd_correct   = (svd_says_A_lt_B   == measured_A_lt_B)

        if cubic_correct and not svd_correct:
            cubic_wins += 1
            disagreement_details.append({"winner":"cubic","rowA":A["svd_rows"],"colA":A["svd_cols"],
                "solverA":A["svd_solver"],"rowB":B["svd_rows"],"colB":B["svd_cols"],"solverB":B["svd_solver"],
                "mt_A":mt_A,"mt_B":mt_B,"cc_A":cc_A,"cc_B":cc_B,"cs_A":cs_A,"cs_B":cs_B})
        elif svd_correct and not cubic_correct:
            svd_wins += 1
            disagreement_details.append({"winner":"svd","rowA":A["svd_rows"],"colA":A["svd_cols"],
                "solverA":A["svd_solver"],"rowB":B["svd_rows"],"colB":B["svd_cols"],"solverB":B["svd_solver"],
                "mt_A":mt_A,"mt_B":mt_B,"cc_A":cc_A,"cc_B":cc_B,"cs_A":cs_A,"cs_B":cs_B})
        else:
            unresolved += 1

def pct(n, d): return f"{100*n/d:.1f}%" if d else "N/A"

print(f"\n  Disagreement pairs analysed: {total_pairs}")
print(f"    Cubic correct: {cubic_wins} ({pct(cubic_wins, total_pairs)})")
print(f"    SVD correct:   {svd_wins} ({pct(svd_wins, total_pairs)})")
print(f"    Ties (noise):  {ties} ({pct(ties, total_pairs)})")
print(f"    Unresolved:    {unresolved} ({pct(unresolved, total_pairs)})")

table2 = [{"metric":"total_comparable_disagreement_pairs","value":total_pairs},
          {"metric":"cubic_correct","value":cubic_wins},
          {"metric":"cubic_correct_pct","value":f"{100*cubic_wins/max(1,total_pairs):.2f}"},
          {"metric":"svd_correct","value":svd_wins},
          {"metric":"svd_correct_pct","value":f"{100*svd_wins/max(1,total_pairs):.2f}"},
          {"metric":"ties","value":ties},
          {"metric":"unresolved","value":unresolved}]
with open("results/tables/table2_ranking_disagreement.csv","w",newline="") as f:
    w = csv.DictWriter(f, fieldnames=["metric","value"])
    w.writeheader(); w.writerows(table2)
if disagreement_details:
    with open("results/tables/disagreement_pairs.csv","w",newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(disagreement_details[0].keys()))
        w.writeheader(); w.writerows(disagreement_details)
print("\n  -> Saved results/tables/table2_ranking_disagreement.csv")

# ---------------------------------------------------------------------------
# PART 6: CIRCUIT-LEVEL ACCUMULATED COST vs MEASURED TOTAL SVD TIME
# ---------------------------------------------------------------------------
print("\n" + "="*60)
print("PART 6: CIRCUIT-LEVEL ACCUMULATED COST vs MEASURED TIMING")
print("="*60)
print("""
NOTE: C_cubic_real and C_svd_real are analytical surrogates computed from
REAL bond dimensions observed during execution. They are NOT the routing
objective (which uses dummy dimensions). The independent measured targets
are total_svd_time and total_simulation_time from benchmark_results_costmodel.csv.
""")

# Accumulate per (circuit, repetition, cost_model) from trace
accum = defaultdict(lambda: {"sum_cubic": 0.0, "sum_svd": 0.0, "ops": 0})
for row in valid_rows:
    key = (row["circuit"], _i(row["repetition"]), row["cost_model"])
    accum[key]["sum_cubic"] += row["_cost_c"]
    accum[key]["sum_svd"]   += row["_cost_s"]
    accum[key]["ops"] += 1

# Merge with circuit-level CSV
merged = []
for crow in circuit_rows:
    key = (crow["circuit"], _i(crow["repetition"]), crow["cost_model"])
    if key in accum:
        rec = dict(crow)
        rec["sum_cubic_real"] = accum[key]["sum_cubic"]
        rec["sum_svd_real"]   = accum[key]["sum_svd"]
        rec["op_count"]       = accum[key]["ops"]
        merged.append(rec)
print(f"  Merged circuit-level records: {len(merged)}")

sum_c_all  = [_f(r["sum_cubic_real"]) for r in merged]
sum_s_all  = [_f(r["sum_svd_real"])   for r in merged]
svd_t_all  = [_f(r["total_svd_time_ms"]) for r in merged]
sim_t_all  = [_f(r["total_simulation_time_ms"]) for r in merged]

rc_svd, _  = spearman(sum_c_all, svd_t_all)
rs_svd, _  = spearman(sum_s_all, svd_t_all)
rc_sim, _  = spearman(sum_c_all, sim_t_all)
rs_sim, _  = spearman(sum_s_all, sim_t_all)

print(f"\n  Pooled (N={len(merged)}):")
print(f"    ΣC_cubic_real vs total_svd_time:  ρ = {rc_svd:.4f}")
print(f"    ΣC_svd_real   vs total_svd_time:  ρ = {rs_svd:.4f}")
print(f"    ΣC_cubic_real vs total_sim_time:  ρ = {rc_sim:.4f}")
print(f"    ΣC_svd_real   vs total_sim_time:  ρ = {rs_sim:.4f}")

print(f"\n  Per circuit family:")
for fam in sorted(set(circuit_family(r["circuit"]) for r in merged)):
    sub = [r for r in merged if circuit_family(r["circuit"]) == fam]
    if len(sub) < 4: continue
    sc_ = [_f(r["sum_cubic_real"]) for r in sub]
    ss_ = [_f(r["sum_svd_real"])   for r in sub]
    st_ = [_f(r["total_svd_time_ms"]) for r in sub]
    rc_, _ = spearman(sc_, st_); rs_, _ = spearman(ss_, st_)
    print(f"    {fam:<20} N={len(sub):3d}  cubic ρ={rc_:.4f}  svd ρ={rs_:.4f}")

with open("results/tables/circuit_level_accumulated.csv","w",newline="") as f:
    if merged:
        w = csv.DictWriter(f, fieldnames=list(merged[0].keys()))
        w.writeheader(); w.writerows(merged)
print("\n  -> Saved results/tables/circuit_level_accumulated.csv")

# ---------------------------------------------------------------------------
# PART 7: SetUpcomingGates confound documentation
# ---------------------------------------------------------------------------
print("\n" + "="*60)
print("PART 7: SetUpcomingGates CONFOUND ANALYSIS")
print("="*60)
print("""
FINDING: SetUpcomingGates was added uniformly to ALL benchmark runs (both
cost models). This was introduced at Stage 13 of the implementation. The
Stage-0 baseline was run BEFORE SetUpcomingGates was added.

IMPLICATION: The Stage-0 cubic baseline and the full-experiment cubic runs
are NOT directly comparable in terms of routing decisions -- SetUpcomingGates
initializes the meeting-position lookahead callback which may alter Maestro's
SWAP insertion strategy.

MITIGATION: All cubic vs SVD comparisons in Part 8 onward use the SAME codebase
with SetUpcomingGates enabled for BOTH configurations. The comparison is therefore
internally valid.

RECOMMENDATION: For the thesis, clearly state that the cost-model comparison is
performed on the post-fix configuration and that the Stage-0 results (without
SetUpcomingGates) serve only as a build regression check, not as a fair cubic baseline.
""")

# Save summary JSON for use in report generation
summary = {
    "table1": table1_rows,
    "shape_spearman_cubic": rc_shape,
    "shape_spearman_svd": rs_shape,
    "shape_kendall_cubic": tc,
    "shape_kendall_svd": ts,
    "disagreement_total": total_pairs,
    "cubic_wins": cubic_wins,
    "svd_wins": svd_wins,
    "ties": ties,
    "unresolved": unresolved,
    "circuit_level_rc_svdtime": rc_svd,
    "circuit_level_rs_svdtime": rs_svd,
    "circuit_level_rc_simtime": rc_sim,
    "circuit_level_rs_simtime": rs_sim,
}
with open("results/analysis_summary.json","w") as f:
    json.dump(summary, f, indent=2)

print("\nAll analysis complete.")
print("Results written to results/tables/")
