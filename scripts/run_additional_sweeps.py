#!/usr/bin/env python3
"""
Additional sweeps for the Month 5 Validation Report:
1. Check VQE circuits (vqe_two_local_10, vqe_two_local_20, vqe_real_amp_10, vqe_real_amp_20)
2. Bond dimension scaling (chi = 32, 128) on the core RigidSeed circuits
3. Multi-threaded execution (8 threads on i7-12650H P-cores) at chi = 64
"""

import os
import subprocess
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(REPO_ROOT)

BIN = os.path.join(REPO_ROOT, "maestro/build/bin/real_sim_comparison_benchmark")

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

VQE_CIRCUITS = [
    "benchmarks/vqe/vqe_real_amp_10.qasm",
    "benchmarks/vqe/vqe_real_amp_20.qasm",
    "benchmarks/vqe/vqe_two_local_10.qasm",
    "benchmarks/vqe/vqe_two_local_20.qasm",
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


def run_batch(circuits, cost_model, bond_dim, reps, threads, std_out, det_out):
    for p in (std_out, det_out):
        if os.path.exists(p):
            os.remove(p)

    env = build_env(threads=threads)
    print(f"\n=== Running: model={cost_model}, chi={bond_dim}, reps={reps}, threads={threads} ===", flush=True)
    t0 = time.time()
    cmd = [
        BIN,
        *circuits,
        "--cost-model", cost_model,
        "--bond-dim", str(bond_dim),
        "--reps", str(reps),
        "--backend", "cpu",
        "--output", std_out,
        "--detailed-output", det_out,
    ]
    subprocess.run(cmd, env=env, check=True)
    print(f"=== Completed in {time.time() - t0:.2f}s ===", flush=True)


if __name__ == "__main__":
    # 1. Check VQE circuits at chi=64, 1T
    run_batch(
        VQE_CIRCUITS,
        cost_model="cubic",
        bond_dim=64,
        reps=5,
        threads=1,
        std_out="results/real_sim_vqe_cubic_chi64_1T.csv",
        det_out="results/real_sim_vqe_cubic_chi64_1T_detailed.csv",
    )

    # 2. Bond dimension chi=32 (1T, 5 reps) on RigidSeed circuits
    run_batch(
        RIGIDSEED_CIRCUITS,
        cost_model="cubic",
        bond_dim=32,
        reps=5,
        threads=1,
        std_out="results/real_sim_rigidseed_cubic_chi32_1T.csv",
        det_out="results/real_sim_rigidseed_cubic_chi32_1T_detailed.csv",
    )

    # 3. Multi-threaded (8 threads) at chi=64 (5 reps) on RigidSeed circuits
    run_batch(
        RIGIDSEED_CIRCUITS,
        cost_model="cubic",
        bond_dim=64,
        reps=5,
        threads=8,
        std_out="results/real_sim_rigidseed_cubic_chi64_8T.csv",
        det_out="results/real_sim_rigidseed_cubic_chi64_8T_detailed.csv",
    )

