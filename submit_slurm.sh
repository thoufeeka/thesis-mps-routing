#!/bin/bash
#SBATCH --job-name=mps_routing_sim
#SBATCH --output=results/slurm_%j.out
#SBATCH --error=results/slurm_%j.err
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=32G
#SBATCH --time=04:00:00
# ------------------------------------------------------------------------------
# Optional cluster-specific settings (uncomment if required by your cluster):
# #SBATCH --partition=compute
# #SBATCH --account=your_account
# #SBATCH --gres=gpu:1
# ------------------------------------------------------------------------------
#
# Usage on the university cluster:
#   git clone https://github.com/thoufeeka/thesis-mps-routing.git
#   cd thesis-mps-routing
#   sbatch submit_slurm.sh
#
# To customize parameters without editing the file:
#   sbatch --export=ALL,REPS=5,BOND_DIMS="32 64 128 256" submit_slurm.sh
# ------------------------------------------------------------------------------

set -euo pipefail

# Move to the directory from which sbatch was submitted
cd "${SLURM_SUBMIT_DIR:-$(pwd)}"
mkdir -p results

echo "======================================================================"
echo " Slurm Job ID        : ${SLURM_JOB_ID:-local}"
echo " Slurm Job Name      : ${SLURM_JOB_NAME:-mps_routing_sim}"
echo " Node(s)             : ${SLURM_NODELIST:-$(hostname)}"
echo " CPUs per task       : ${SLURM_CPUS_PER_TASK:-$(nproc 2>/dev/null || echo 4)}"
echo " Working Directory   : $(pwd)"
echo "======================================================================"

# Load environment modules if running on a cluster that requires module load
if command -v module >/dev/null 2>&1; then
    echo "[submit_slurm] Checking cluster environment modules..."
    if ! command -v cmake >/dev/null 2>&1; then
        module load cmake 2>/dev/null || module load CMake 2>/dev/null || true
    fi
    # Load a modern GCC (C++17 required) if system g++ is older than GCC 9
    if ! command -v g++ >/dev/null 2>&1 || [[ "$(g++ -dumpversion | cut -d. -f1)" -lt 9 ]]; then
        module load gcc 2>/dev/null || module load GCC 2>/dev/null || true
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        module load python 2>/dev/null || module load Python 2>/dev/null || true
    fi
    module list 2>&1 || true
fi

echo "[submit_slurm] Tools detected:"
echo "  g++     : $(command -v g++ || echo 'NOT FOUND') ($(g++ --version 2>/dev/null | head -n1 || true))"
echo "  cmake   : $(command -v cmake || echo 'NOT FOUND') ($(cmake --version 2>/dev/null | head -n1 || true))"
echo "  python3 : $(command -v python3 || echo 'NOT FOUND') ($(python3 --version 2>/dev/null | head -n1 || true))"

# Execute the full offline build + benchmark pipeline
bash cluster/run_all_benchmarks.sh

