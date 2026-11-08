#!/usr/bin/env python3
"""
Validate that RigidSeedLocalSearch proxy cost reductions translate into real
MPS simulation wall-clock speedups (Month 5 Thesis Deliverable M5).

Runs real_sim_comparison_benchmark on the circuits where RigidSeedLocalSearch
achieved proxy cost reduction vs BuildChain.
"""

import os
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(REPO_ROOT)

BIN = os.path.join(REPO_ROOT, "maestro/build/bin/real_sim_comparison_benchmark")

# The 10 circuits from the 12-circuit suite where RigidSeedLocalSearch found
# a proxy cost reduction (excluding random_30 and random_40 where delta = 0.0%),
# ordered from fastest/smallest to largest so progress is visible immediately.
RIGIDSEED_CIRCUITS = [
    "benchmarks/qaoa_10.qasm",
    "benchmarks/qft_10.qasm",
    "benchmarks/random_10.qasm",
    "benchmarks/qaoa_30.qasm",
    "benchmarks/qaoa_40.qasm",
    "benchmarks/qft_20.qasm",
    "benchmarks/random_20.qasm",
    "benchmarks/qaoa_20.qasm",
    "benchmarks/qft_30.qasm",
    "benchmarks/qft_40.qasm",
]


def build_env(threads=1):
    env = os.environ.copy()
    env["OMP_NUM_THREADS"] = str(threads)
    env["OPENBLAS_NUM_THREADS"] = str(threads)
    env["MKL_NUM_THREADS"] = str(threads)

    ld_paths = [
        os.path.join(REPO_ROOT, "maestro/build"),
        os.path.join(REPO_ROOT, "maestro/build/bin"),
        os.path.join(REPO_ROOT, "maestro/build/deps_offline/boost_1_89_0/lib"),
        os.path.join(REPO_ROOT, "maestro/build/boost_1_89_0/lib"),
    ]
    existing_ld = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = ":".join(ld_paths + ([existing_ld] if existing_ld else []))
    return env


def run_suite(cost_model="cubic", bond_dim=64, reps=5, threads=1, std_out=None, det_out=None):
    if std_out is None:
        std_out = f"results/real_sim_rigidseed_{cost_model}_chi{bond_dim}_{threads}T.csv"
    if det_out is None:
        det_out = f"results/real_sim_rigidseed_{cost_model}_chi{bond_dim}_{threads}T_detailed.csv"

    for p in (std_out, det_out):
        if os.path.exists(p):
            os.remove(p)

    env = build_env(threads=threads)
    print(f"\n======================================================================")
    print(f"Running Real MPS Simulation Validation (RigidSeedLocalSearch vs BuildChain)")
    print(f"Cost model: {cost_model} | Bond dim: {bond_dim} | Reps: {reps} | Threads: {threads}")
    print(f"Standard CSV: {std_out}")
    print(f"Detailed CSV: {det_out}")
    print(f"======================================================================\n", flush=True)

    for idx, circuit in enumerate(RIGIDSEED_CIRCUITS, 1):
        t0 = time.time()
        print(f"[{idx}/{len(RIGIDSEED_CIRCUITS)}] Running {circuit} ...", flush=True)
        cmd = [
            BIN,
            circuit,
            "--cost-model", cost_model,
            "--bond-dim", str(bond_dim),
            "--reps", str(reps),
            "--backend", "auto",
            "--output", std_out,
            "--detailed-output", det_out,
            "--append",
        ]
        res = subprocess.run(cmd, env=env)
        elapsed = time.time() - t0
        if res.returncode != 0:
            print(f"  [WARN] {circuit} exited with code {res.returncode} after {elapsed:.2f}s", flush=True)
        else:
            print(f"  -> Completed {circuit} in {elapsed:.2f}s\n", flush=True)


if __name__ == "__main__":
    # 1. Primary canonical validation: cubic cost model, bond_dim=64, 1 thread, 5 reps
    run_suite(
        cost_model="cubic",
        bond_dim=64,
        reps=5,
        threads=1,
        std_out="results/real_sim_comparison_results.csv",
        det_out="results/real_sim_detailed_results.csv",
    )
    # 2. Secondary validation: ThinSVDGeometry (svd) cost model, bond_dim=64, 1 thread, 5 reps
    run_suite(
        cost_model="svd",
        bond_dim=64,
        reps=5,
        threads=1,
        std_out="results/real_sim_rigidseed_svd_chi64_1T.csv",
        det_out="results/real_sim_rigidseed_svd_chi64_1T_detailed.csv",
    )

