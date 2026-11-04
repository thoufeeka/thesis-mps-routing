#!/bin/bash
#SBATCH --job-name=mps_routing_gpu
#SBATCH --output=results/slurm_gpu_%j.out
#SBATCH --error=results/slurm_gpu_%j.err
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --gres=gpu:1
#SBATCH --mem=32G
#SBATCH --time=04:00:00
# ------------------------------------------------------------------------------
# Optional cluster-specific settings (uncomment if required by your cluster):
# #SBATCH --partition=gpu
# #SBATCH --account=your_account
# ------------------------------------------------------------------------------
#
# GPU Partition Slurm Script:
# Requests 1 GPU + 16 CPU cores. Automatically runs GPU simulation if
# libmaestro_gpu_simulators.so is present, plus full CPU MPS benchmarks.
#
# Usage:
#   sbatch cluster/slurm_gpu.sh
# ------------------------------------------------------------------------------

set -euo pipefail

cd "${SLURM_SUBMIT_DIR:-$(pwd)}"
mkdir -p results

if command -v module >/dev/null 2>&1; then
    module load cuda 2>/dev/null || module load CUDA 2>/dev/null || true
    if ! command -v cmake >/dev/null 2>&1; then
        module load cmake 2>/dev/null || module load CMake 2>/dev/null || true
    fi
    if ! command -v g++ >/dev/null 2>&1 || [[ "$(g++ -dumpversion | cut -d. -f1)" -lt 9 ]]; then
        module load gcc 2>/dev/null || module load GCC 2>/dev/null || true
    fi
fi

bash cluster/run_all_benchmarks.sh

