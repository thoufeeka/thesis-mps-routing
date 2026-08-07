#!/usr/bin/env python3
"""
verify_data.py — Part 1 (PART 1 of spec): Verify existing benchmark data integrity.
"""
import csv, math, sys, os
from collections import defaultdict

TRACE_FILE   = "mps_operation_trace.csv"
CIRCUIT_FILE = "benchmark_results_costmodel.csv"
VALID_SOLVERS = {"bdc", "jacobi"}
VALID_MODELS  = {"cubic", "svd"}

def _float(s):
    try: return float(s)
    except: return None

def _int(s):
    try: return int(s)
    except: return None

def check_trace(path):
    print(f"\n{'='*60}\nCHECKING: {path}\n{'='*60}")
    rows = []; errors = []; warnings = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames
        print(f"Columns ({len(fieldnames)}): {fieldnames}")
        for row in reader:
            rows.append(row)
    print(f"Total rows: {len(rows)}")

    # NaN/Inf
    nan_count = 0; inf_count = 0
    numeric_cols = ["chi_left","chi_center_before","chi_right","chi_center_after",
                    "svd_rows","svd_cols","cost_cubic_real","cost_svd_real","svd_time_ns"]
    for row in rows:
        for col in numeric_cols:
            v = _float(row.get(col,""))
            if v is None: nan_count += 1
            elif math.isinf(v): inf_count += 1
    print(f"NaN values: {nan_count}  Inf values: {inf_count}")

    circuits = sorted(set(r["circuit"] for r in rows))
    models   = sorted(set(r["cost_model"] for r in rows))
    solvers  = sorted(set(r["svd_solver"] for r in rows))
    print(f"Circuits ({len(circuits)}): {circuits}")
    print(f"Cost models: {models}  SVD solvers: {solvers}")
    for s in solvers:
        if s not in VALID_SOLVERS: warnings.append(f"UNKNOWN solver: '{s}'")

    # Repetition counts
    rep_counts = defaultdict(set)
    for row in rows:
        rep_counts[(row["circuit"],row["cost_model"])].add(_int(row["repetition"]))
    print("\nRepetitions per (circuit, cost_model):")
    for (c,m), reps in sorted(rep_counts.items()):
        print(f"  {c} [{m}]: reps={sorted(reps)}")

    # Warmup contamination (rep==0)
    warmup_rows = [r for r in rows if _int(r["repetition"]) == 0]
    if warmup_rows: errors.append(f"WARMUP CONTAMINATION: {len(warmup_rows)} rows with rep=0")
    else: print("Warmup check: PASS (no rep=0)")

    # Duplicate keys
    seen = set(); dup = 0
    for row in rows:
        key = (row["circuit"],row["repetition"],row["cost_model"],row["operation_index"])
        if key in seen: dup += 1
        seen.add(key)
    if dup: errors.append(f"DUPLICATE KEYS: {dup}")
    else: print("Duplicate check: PASS")

    # operation_index reset per group
    groups = defaultdict(list)
    for row in rows:
        groups[(row["circuit"],row["cost_model"],_int(row["repetition"]))].append(_int(row["operation_index"]))
    reset_ok = True
    for key, idxs in sorted(groups.items()):
        if idxs[0] != 1:
            reset_ok = False; errors.append(f"op_index doesn't start at 1 for {key}")
        for a,b in zip(idxs,idxs[1:]):
            if b != a+1:
                reset_ok = False; warnings.append(f"op_index gap in {key}: {a}->{b}"); break
    if reset_ok: print("operation_index reset check: PASS")

    # SVD geometry check
    geom_mismatch = 0
    for row in rows:
        sr = _int(row["svd_rows"]); sc = _int(row["svd_cols"])
        cl = _float(row["chi_left"]); cr = _float(row["chi_right"])
        if sr is not None and cl is not None and sr != int(2*cl): geom_mismatch += 1
        if sc is not None and cr is not None and sc != int(2*cr): geom_mismatch += 1
    if geom_mismatch: errors.append(f"SVD GEOMETRY MISMATCH: {geom_mismatch}")
    else: print("SVD geometry check (svd_rows==2χL, svd_cols==2χR): PASS")

    # Cost formula recomputation
    cubic_mm = 0; svd_mm = 0; tol = 1e-3
    for row in rows:
        cc = _float(row["chi_center_before"])
        cl = _float(row["chi_left"]); cr = _float(row["chi_right"])
        sc = _float(row["cost_cubic_real"]); ss = _float(row["cost_svd_real"])
        if None in (cc,cl,cr,sc,ss): continue
        exp_c = cc**3; exp_s = cl*cr*min(cl,cr)
        if abs(sc-exp_c) > tol*max(1,abs(exp_c)): cubic_mm += 1
        if abs(ss-exp_s) > tol*max(1,abs(exp_s)): svd_mm += 1
    if cubic_mm: errors.append(f"COST CUBIC RECOMPUTE MISMATCH: {cubic_mm}")
    else: print("cost_cubic_real formula check: PASS")
    if svd_mm: errors.append(f"COST SVD RECOMPUTE MISMATCH: {svd_mm}")
    else: print("cost_svd_real formula check: PASS")

    # Timing range
    times = [_float(r["svd_time_ns"]) for r in rows if _float(r["svd_time_ns"]) is not None]
    print(f"svd_time_ns range: {min(times):.0f} – {max(times):.0f} ns")
    neg = sum(1 for t in times if t < 0)
    if neg: errors.append(f"NEGATIVE TIMING: {neg} rows")

    # Bond dimension range
    chic = [_float(r["chi_center_before"]) for r in rows if _float(r["chi_center_before"]) is not None]
    print(f"chi_center_before range: {min(chic):.0f} – {max(chic):.0f}")

    print(f"\n--- TRACE VERIFICATION SUMMARY ---")
    if errors:
        for e in errors: print(f"  [ERROR] {e}")
    else: print("  All checks PASSED.")
    for w in warnings: print(f"  [WARN]  {w}")
    return len(errors) == 0

def check_circuit_file(path):
    print(f"\n{'='*60}\nCHECKING: {path}\n{'='*60}")
    rows = []; errors = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        print(f"Columns: {reader.fieldnames}")
        for row in reader: rows.append(row)
    print(f"Total rows: {len(rows)}")
    nan_count = 0
    for row in rows:
        for c in ["total_svd_time_ms","total_simulation_time_ms","routing_optimization_time_ms"]:
            if _float(row.get(c,"")) is None: nan_count += 1
    print(f"NaN values: {nan_count}")
    circuits = sorted(set(r["circuit"] for r in rows))
    models   = sorted(set(r["cost_model"] for r in rows))
    print(f"Circuits: {circuits}")
    print(f"Cost models: {models}")
    rep_counts = defaultdict(set)
    for row in rows:
        rep_counts[(row["circuit"],row["cost_model"])].add(_int(row["repetition"]))
    print("\nRepetitions per (circuit, model):")
    for key, reps in sorted(rep_counts.items()):
        print(f"  {key}: {sorted(reps)}")
    svd = [_float(r["total_svd_time_ms"]) for r in rows if _float(r["total_svd_time_ms"]) is not None]
    sim = [_float(r["total_simulation_time_ms"]) for r in rows if _float(r["total_simulation_time_ms"]) is not None]
    print(f"total_svd_time_ms range: {min(svd):.3f} – {max(svd):.3f} ms")
    print(f"total_simulation_time_ms range: {min(sim):.3f} – {max(sim):.3f} ms")
    bad = sum(1 for r in rows
              if _float(r.get("total_svd_time_ms")) is not None
              and _float(r.get("total_simulation_time_ms")) is not None
              and _float(r["total_svd_time_ms"]) > _float(r["total_simulation_time_ms"]))
    if bad: errors.append(f"SVD_TIME > SIM_TIME for {bad} rows")
    print(f"\n--- CIRCUIT-LEVEL VERIFICATION SUMMARY ---")
    if errors:
        for e in errors: print(f"  [ERROR] {e}")
    else: print("  All checks PASSED.")
    return len(errors) == 0

if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")
    ok1 = check_trace(TRACE_FILE)
    ok2 = check_circuit_file(CIRCUIT_FILE)
    sys.exit(0 if (ok1 and ok2) else 1)
