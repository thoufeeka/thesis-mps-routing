#!/usr/bin/env python3
"""
Benchmark runner for Maestro MPS Cost Model Validation.
Supports smoke mode (3 circuits x 3 repetitions x 1 warmup)
and full mode (8 circuits x 7 repetitions x 2 warmups).
"""

import sys
import os
import subprocess
import argparse

BENCHMARK_BIN = "maestro/build/bin/mps_benchmark"

SMOKE_CIRCUITS = [
    "benchmarks/qft_10.qasm",
    "benchmarks/qaoa_10.qasm",
    "benchmarks/vqe/vqe_real_amp_10.qasm"
]

FULL_CIRCUITS = [
    "benchmarks/qft_10.qasm",
    "benchmarks/qft_20.qasm",
    "benchmarks/qaoa_10.qasm",
    "benchmarks/qaoa_20.qasm",
    "benchmarks/vqe/vqe_real_amp_10.qasm",
    "benchmarks/vqe/vqe_real_amp_20.qasm",
    "benchmarks/vqe/vqe_two_local_10.qasm",
    "benchmarks/vqe/vqe_two_local_20.qasm"
]

def main():
    parser = argparse.ArgumentParser(description="Maestro MPS Cost Model Benchmark Runner")
    parser.add_argument("--mode", choices=["smoke", "full"], default="smoke",
                        help="Run smoke test (3 circuits, 3 reps) or full benchmark (8 circuits, 7 reps)")
    parser.add_argument("--bin", default=BENCHMARK_BIN, help="Path to mps_benchmark binary")
    args = parser.parse_args()

    bin_path = os.path.abspath(args.bin)
    if not os.path.exists(bin_path):
        print(f"Error: Executable not found at {bin_path}")
        sys.exit(1)

    circuits = SMOKE_CIRCUITS if args.mode == "smoke" else FULL_CIRCUITS
    warmups = 1 if args.mode == "smoke" else 2
    repetitions = 3 if args.mode == "smoke" else 7

    env = os.environ.copy()
    env["OMP_NUM_THREADS"] = "1"
    env["OPENBLAS_NUM_THREADS"] = "1"
    env["MKL_NUM_THREADS"] = "1"

    boost_lib = os.path.abspath("maestro/build/boost_1_89_0/lib")
    build_dir = os.path.abspath("maestro/build")
    current_ld = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = f"{build_dir}:{boost_lib}:{current_ld}"

    # Remove stale output files if starting fresh
    for csv_file in ["mps_operation_trace.csv", "benchmark_results_costmodel.csv"]:
        if os.path.exists(csv_file):
            os.remove(csv_file)

    print(f"==================================================")
    print(f"Running MPS Cost Model Benchmark ({args.mode.upper()} MODE)")
    print(f"Circuits: {len(circuits)}, Warmups: {warmups}, Repetitions: {repetitions}")
    print(f"Single-threaded execution (OMP/BLAS/MKL=1)")
    print(f"==================================================\n")

    for circuit in circuits:
        if not os.path.exists(circuit):
            print(f"Warning: Circuit file {circuit} not found. Skipping.")
            continue

        for model in ["cubic", "svd"]:
            cmd = [bin_path, circuit, "--cost-model", model, "--warmups", str(warmups), "--repetitions", str(repetitions)]
            
            # Use taskset if available
            try:
                taskset_cmd = ["taskset", "-c", "0"] + cmd
                print(f"Running: {' '.join(taskset_cmd)}")
                subprocess.run(taskset_cmd, env=env, check=True)
            except Exception:
                print(f"Running (no taskset): {' '.join(cmd)}")
                subprocess.run(cmd, env=env, check=True)

    print("\n==================================================")
    print("Benchmark Execution Complete.")
    print("Generated CSV outputs:")
    print("  - mps_operation_trace.csv (Operation-level SVD trace)")
    print("  - benchmark_results_costmodel.csv (Circuit-level metrics)")
    print("==================================================")

if __name__ == "__main__":
    main()
