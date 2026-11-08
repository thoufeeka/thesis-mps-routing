#!/bin/bash
#SBATCH --job-name=mps_routing_sim
#SBATCH --output=results/slurm_%j.out
#SBATCH --error=results/slurm_%j.err
#SBATCH --partition=cpu
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=32G
#SBATCH --time=12:00:00
# ------------------------------------------------------------------------------
# Usage on the university cluster:
#   cd ~/thesis/thesis-mps-routing
#   git pull
#   mkdir -p results
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

# Load GCC / Python modules only if needed (no cmake/git/patch required!)
if command -v module >/dev/null 2>&1; then
    if ! command -v g++ >/dev/null 2>&1 || [[ "$(g++ -dumpversion | cut -d. -f1)" -lt 9 ]]; then
        module load gcc 2>/dev/null || module load GCC 2>/dev/null || true
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        module load python 2>/dev/null || module load Python 2>/dev/null || true
    fi
fi

echo "[submit_slurm] Tools detected:"
echo "  g++     : $(command -v g++ || echo 'NOT FOUND') ($(g++ --version 2>/dev/null | head -n1 || true))"
echo "  python3 : $(command -v python3 || echo 'NOT FOUND') ($(python3 --version 2>/dev/null | head -n1 || true))"

# Execute the full offline build + benchmark pipeline
bash cluster/run_all_benchmarks.sh
