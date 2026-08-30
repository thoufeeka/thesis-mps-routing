#!/usr/bin/env python3
"""
QMAP Benchmark Orchestration  (Heuristic only)
===============================================
Runs QMAP heuristic on all benchmark circuits using a 1D linear chain topology,
then evaluates the MPS cost via the Maestro C++ dummy simulator.

Output: results/qmap_benchmark_results.csv
"""

import subprocess
import time
import csv
import sys
from pathlib import Path

ROOT      = Path(__file__).parent.parent
BENCHMARK = ROOT / "maestro" / "build" / "bin" / "mps_mapping_benchmark"
CIRCUITS  = sorted((ROOT / "benchmarks").glob("*.qasm"))
OUTPUT    = ROOT / "results" / "qmap_benchmark_results.csv"

COST_MODEL = "cubic"
BOND_DIM   = 64

from mqt.core import load
from mqt.qmap import sc


def make_1d_arch(n):
    return sc.Architecture(n, {(i, i + 1) for i in range(n - 1)})


def qmap_layout_to_maestro(initial_layout):
    """QMAP: physical->logical  |  Maestro: map[logical]=physical"""
    perm = {}
    for phys, log in initial_layout.items():
        perm[int(log)] = int(phys)
    return [perm[l] for l in range(len(perm))]


def run_qmap_heuristic(circuit_file):
    circ = load(str(circuit_file))
    arch = make_1d_arch(circ.num_qubits)
    cfg  = sc.Configuration()
    cfg.method         = sc.Method.heuristic
    cfg.initial_layout = sc.InitialLayout.dynamic
    t0 = time.perf_counter()
    mapped, results = sc.map_(circ, arch, cfg)
    ms = (time.perf_counter() - t0) * 1000.0
    return qmap_layout_to_maestro(mapped.initial_layout), results.output.swaps, ms


def eval_mapping_cpp(circuit_file, mapping, strategy_name, qmap_swaps, wall_ms):
    mapping_str = ",".join(str(x) for x in mapping)
    cmd = [
        str(BENCHMARK),
        "--cost-model", COST_MODEL,
        "--bond-dim",   str(BOND_DIM),
        "--output",     str(OUTPUT),
        "--eval-mapping", mapping_str,
        "--strategy-name", strategy_name,
        str(circuit_file),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  [C++ ERROR] {strategy_name}: {result.stderr.strip()}")
        return

    # Patch last row: fill real mapping time and qmap_swaps
    with open(OUTPUT, "r", newline="") as f:
        rows = list(csv.reader(f))
    if len(rows) >= 2:
        last = rows[-1]
        if len(last) > 10:
            last[10] = f"{wall_ms:.4f}"
        while len(last) < 12:
            last.append("")
        last[11] = str(qmap_swaps)
        with open(OUTPUT, "w", newline="") as f:
            csv.writer(f).writerows(rows)

    if result.stdout.strip():
        print(f"  {result.stdout.strip()}")


def main():
    if not BENCHMARK.exists():
        print(f"ERROR: {BENCHMARK} not found")
        sys.exit(1)

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)

    with open(OUTPUT, "w", newline="") as f:
        csv.writer(f).writerow([
            "circuit", "qubits", "layers", "two_qubit_gates",
            "cost_model", "bond_dim", "strategy", "communities",
            "predicted_cost", "peak_bond_dim", "mapping_time_ms", "qmap_swaps",
        ])

    sep = "=" * 65
    print(f"\n{sep}")
    print(f"  QMAP Heuristic Benchmark  |  {len(CIRCUITS)} circuits")
    print(f"  Output: {OUTPUT}")
    print(f"{sep}\n")

    for cf in CIRCUITS:
        circ_info = load(str(cf))
        n = circ_info.num_qubits
        print(f"\n--- {cf.name}  ({n} qubits) ---")
        try:
            h_map, h_swaps, h_ms = run_qmap_heuristic(cf)
            print(f"  QMAP-Heuristic: {h_swaps} SWAPs  ({h_ms:.1f} ms)")
            eval_mapping_cpp(cf, h_map, "QMAP_Heuristic", h_swaps, h_ms)
        except Exception as e:
            print(f"  FAILED: {e}")

    print(f"\n{sep}")
    print(f"  Done.  Results -> {OUTPUT}")
    print(f"{sep}\n")


if __name__ == "__main__":
    main()
