#!/usr/bin/env python3
"""Run benchmark only on the two random circuits, appending to existing CSVs."""
import os, subprocess, sys

os.chdir("/home/steffi/thesis-mps-routing")

BIN = "maestro/build/bin/mps_benchmark"
CIRCUITS = [
    "benchmarks/random_10.qasm",
    "benchmarks/random_20.qasm",
]
WARMUPS = 2
REPS = 7

env = os.environ.copy()
env["OMP_NUM_THREADS"] = "1"
env["OPENBLAS_NUM_THREADS"] = "1"
env["MKL_NUM_THREADS"] = "1"
env["LD_LIBRARY_PATH"] = (
    "maestro/build:maestro/build/boost_1_89_0/lib:"
    + env.get("LD_LIBRARY_PATH", "")
)

bin_path = os.path.abspath(BIN)
print(f"Binary: {bin_path}")
print(f"Circuits: {CIRCUITS}")
print(f"Warmups: {WARMUPS}, Repetitions: {REPS}\n")

for circuit in CIRCUITS:
    if not os.path.exists(circuit):
        print(f"ERROR: {circuit} not found"); sys.exit(1)
    for model in ["cubic", "svd"]:
        cmd = [bin_path, circuit,
               "--cost-model", model,
               "--warmups", str(WARMUPS),
               "--repetitions", str(REPS)]
        taskset_cmd = ["taskset", "-c", "0"] + cmd
        label = f"{os.path.basename(circuit)}  [{model}]"
        print(f"Running: {label} ...", end="", flush=True)
        try:
            subprocess.run(taskset_cmd, env=env, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            subprocess.run(cmd, env=env, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(" done")

print("\nFinished. Results appended to mps_operation_trace.csv and benchmark_results_costmodel.csv")
