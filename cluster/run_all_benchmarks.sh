#!/usr/bin/env bash
# ==============================================================================
# cluster/run_all_benchmarks.sh
#
# End-to-end benchmark runner for the university cluster.
# Runs:
#   Stage 1: Real MPS simulation comparison (BuildChain vs RigidSeedLocalSearch)
#            across bond dimensions (32, 64, 128, 256), cost models (cubic, svd),
#            single-thread & multi-thread CPU (and GPU automatically if present).
#   Stage 2: All 14 initial mapping strategies benchmark (mps_mapping_benchmark).
#   Stage 3: Cost-model validation & SVD operation trace (mps_benchmark + analysis).
#   Stage 4: Aggregated CSVs + Markdown report (Cluster_Real_Simulation_Report.md).
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

RUN_ID="${SLURM_JOB_ID:-$(date +%Y%m%d_%H%M%S)}"
OUT_DIR="${REPO_ROOT}/results/cluster_run_${RUN_ID}"
mkdir -p "${OUT_DIR}"

# Configurable parameters via environment variables
REPS="${REPS:-5}"
BOND_DIMS="${BOND_DIMS:-32 64 128 256}"
COST_MODELS="${COST_MODELS:-cubic svd}"
RUN_COST_MODEL_TRACE="${RUN_COST_MODEL_TRACE:-1}"

if [[ -n "${SLURM_CPUS_PER_TASK:-}" ]]; then
    MAX_THREADS="${SLURM_CPUS_PER_TASK}"
elif command -v nproc >/dev/null 2>&1; then
    MAX_THREADS="$(nproc)"
else
    MAX_THREADS=4
fi

echo "======================================================================"
echo " Thesis MPS Routing — Cluster Benchmark Suite"
echo " Run ID       : ${RUN_ID}"
echo " Output Dir   : ${OUT_DIR}"
echo " Repetitions  : ${REPS}"
echo " Bond Dims    : ${BOND_DIMS}"
echo " Cost Models  : ${COST_MODELS}"
echo " Max Threads  : ${MAX_THREADS}"
echo " Start Time   : $(date --iso-8601=seconds 2>/dev/null || date)"
echo "======================================================================"

# 0. Record cluster node hardware & environment details
{
    echo "=== Host & Date ==="
    hostname
    date
    echo ""
    echo "=== Slurm Environment ==="
    env | grep -E '^SLURM_' || echo "Not running inside Slurm"
    echo ""
    echo "=== CPU Info ==="
    lscpu 2>/dev/null || cat /proc/cpuinfo | head -40
    echo ""
    echo "=== Memory Info ==="
    free -h 2>/dev/null || true
    echo ""
    echo "=== GPU Info ==="
    nvidia-smi 2>/dev/null || echo "No NVIDIA GPU / nvidia-smi not present on this node"
} > "${OUT_DIR}/hardware_info.txt" 2>&1

# 1. Build binaries offline if needed
bash "${REPO_ROOT}/cluster/build_offline.sh"

BUILD_DIR="${REPO_ROOT}/maestro/build"
BIN_DIR="${BUILD_DIR}/bin"

BOOST_LIBDIR=""
if [[ -d "${BUILD_DIR}/deps_offline/boost_1_89_0/lib" ]]; then
    BOOST_LIBDIR="${BUILD_DIR}/deps_offline/boost_1_89_0/lib"
elif [[ -d "${BUILD_DIR}/boost_1_89_0/lib" ]]; then
    BOOST_LIBDIR="${BUILD_DIR}/boost_1_89_0/lib"
fi

export LD_LIBRARY_PATH="${REPO_ROOT}:${REPO_ROOT}/cluster:${BUILD_DIR}:${BIN_DIR}:${BOOST_LIBDIR}:${LD_LIBRARY_PATH:-}"

# Benchmark circuit lists
MAIN_CIRCUITS=(
    benchmarks/qaoa_10.qasm
    benchmarks/qaoa_20.qasm
    benchmarks/qaoa_30.qasm
    benchmarks/qaoa_40.qasm
    benchmarks/qft_10.qasm
    benchmarks/qft_20.qasm
    benchmarks/qft_30.qasm
    benchmarks/qft_40.qasm
    benchmarks/random_10.qasm
    benchmarks/random_20.qasm
    benchmarks/random_30.qasm
    benchmarks/random_40.qasm
    benchmarks/vqe/vqe_real_amp_10.qasm
    benchmarks/vqe/vqe_real_amp_20.qasm
    benchmarks/vqe/vqe_two_local_10.qasm
    benchmarks/vqe/vqe_two_local_20.qasm
)

# ------------------------------------------------------------------------------
# STAGE 1: Real MPS Simulation Comparison (BuildChain vs RigidSeedLocalSearch)
# ------------------------------------------------------------------------------
echo ""
echo "======================================================================"
echo " STAGE 1: Real MPS Simulation Benchmark (real_sim_comparison_benchmark)"
echo "======================================================================"

# 1a. Primary sweep across bond dimensions and cost models (single-threaded for
#     clean, noise-free algorithmic timing + auto GPU detection)
export OMP_NUM_THREADS=1
export OPENBLAS_NUM_THREADS=1
export MKL_NUM_THREADS=1

for CM in ${COST_MODELS}; do
    for CHI in ${BOND_DIMS}; do
        STD_CSV="${OUT_DIR}/real_sim_chi${CHI}_${CM}_1T.csv"
        DET_CSV="${OUT_DIR}/real_sim_chi${CHI}_${CM}_1T_detailed.csv"

        echo ""
        echo ">>> Running Real Sim: cost_model=${CM}, bond_dim=${CHI}, threads=1, reps=${REPS}"
        "${BIN_DIR}/real_sim_comparison_benchmark" \
            "${MAIN_CIRCUITS[@]}" \
            --cost-model "${CM}" \
            --bond-dim "${CHI}" \
            --reps "${REPS}" \
            --backend auto \
            --output "${STD_CSV}" \
            --detailed-output "${DET_CSV}"

        # Copy the canonical (chi=64, cubic) run to the top-level results/ files
        if [[ "${CHI}" == "64" && "${CM}" == "cubic" ]]; then
            cp -f "${STD_CSV}" "${REPO_ROOT}/results/real_sim_comparison_results.csv"
            cp -f "${DET_CSV}" "${REPO_ROOT}/results/real_sim_detailed_results.csv"
        fi
    done
done

# 1b. Multi-threaded CPU run (if cluster allocation has > 1 core) at chi=64 and chi=128
if [[ "${MAX_THREADS}" -gt 1 ]]; then
    export OMP_NUM_THREADS="${MAX_THREADS}"
    export OPENBLAS_NUM_THREADS="${MAX_THREADS}"
    export MKL_NUM_THREADS="${MAX_THREADS}"

    for CHI in 64 128; do
        # Only run if CHI is in BOND_DIMS or default
        STD_CSV="${OUT_DIR}/real_sim_chi${CHI}_cubic_${MAX_THREADS}T.csv"
        DET_CSV="${OUT_DIR}/real_sim_chi${CHI}_cubic_${MAX_THREADS}T_detailed.csv"

        echo ""
        echo ">>> Running Multi-Threaded Real Sim: cost_model=cubic, bond_dim=${CHI}, threads=${MAX_THREADS}, reps=${REPS}"
        "${BIN_DIR}/real_sim_comparison_benchmark" \
            "${MAIN_CIRCUITS[@]}" \
            --cost-model cubic \
            --bond-dim "${CHI}" \
            --reps "${REPS}" \
            --backend cpu \
            --output "${STD_CSV}" \
            --detailed-output "${DET_CSV}"
    done
fi

# ------------------------------------------------------------------------------
# STAGE 2: All-Strategies Dummy-Simulator Comparison (mps_mapping_benchmark)
# ------------------------------------------------------------------------------
echo ""
echo "======================================================================"
echo " STAGE 2: All 14 Mapping Strategies Benchmark (mps_mapping_benchmark)"
echo "======================================================================"

export OMP_NUM_THREADS=1
for CM in ${COST_MODELS}; do
    for CHI in 64 128 256; do
        MAP_CSV="${OUT_DIR}/mapping_comparison_chi${CHI}_${CM}.csv"
        rm -f "${MAP_CSV}"
        echo ">>> Running mps_mapping_benchmark: cost_model=${CM}, bond_dim=${CHI}"
        "${BIN_DIR}/mps_mapping_benchmark" \
            --cost-model "${CM}" \
            --bond-dim "${CHI}" \
            --output "${MAP_CSV}" \
            "${MAIN_CIRCUITS[@]}"

        if [[ "${CHI}" == "64" && "${CM}" == "cubic" ]]; then
            cp -f "${MAP_CSV}" "${REPO_ROOT}/results/mapping_comparison_results.csv"
        fi
    done
done

# ------------------------------------------------------------------------------
# STAGE 3: Cost-Model Validation & SVD Trace (mps_benchmark + analysis scripts)
# ------------------------------------------------------------------------------
if [[ "${RUN_COST_MODEL_TRACE}" == "1" ]]; then
    echo ""
    echo "======================================================================"
    echo " STAGE 3: Cost-Model Validation & SVD Operation Trace (mps_benchmark)"
    echo "======================================================================"

    python3 "${REPO_ROOT}/scripts/run_benchmarks.py" --mode full --bin "${BIN_DIR}/mps_benchmark"
    python3 "${REPO_ROOT}/scripts/run_random_benchmarks.py" || true

    cp -f "${REPO_ROOT}/benchmark_results_costmodel.csv" "${OUT_DIR}/" 2>/dev/null || true
    cp -f "${REPO_ROOT}/mps_operation_trace.csv" "${OUT_DIR}/" 2>/dev/null || true

    python3 "${REPO_ROOT}/scripts/analyze_results.py" | tee "${OUT_DIR}/cost_model_svd_analysis.txt"
    python3 "${REPO_ROOT}/scripts/full_analysis.py" | tee "${OUT_DIR}/cost_model_full_analysis.txt"
    cp -rf "${REPO_ROOT}/results/tables" "${OUT_DIR}/" 2>/dev/null || true
    cp -f "${REPO_ROOT}/results/analysis_summary.json" "${OUT_DIR}/" 2>/dev/null || true
fi

# ------------------------------------------------------------------------------
# STAGE 4: Consolidate & Generate Cluster Summary Report
# ------------------------------------------------------------------------------
echo ""
echo "======================================================================"
echo " STAGE 4: Generating Consolidated CSV & Markdown Report"
echo "======================================================================"

python3 "${REPO_ROOT}/scripts/summarize_cluster_results.py" \
    --dirs "${OUT_DIR}" \
    --csv-out "${REPO_ROOT}/results/real_sim_all_sweeps.csv" \
    --report-out "${REPO_ROOT}/results/Cluster_Real_Simulation_Report.md"

cp -f "${REPO_ROOT}/results/real_sim_all_sweeps.csv" "${OUT_DIR}/"
cp -f "${REPO_ROOT}/results/Cluster_Real_Simulation_Report.md" "${OUT_DIR}/"

echo ""
echo "================ CLUSTER REPORT PREVIEW ================"
cat "${REPO_ROOT}/results/Cluster_Real_Simulation_Report.md"
echo "========================================================"

# Package all outputs into a single tar.gz archive for easy scp / download
ARCHIVE_PATH="${REPO_ROOT}/results/cluster_results_${RUN_ID}.tar.gz"
tar -czf "${ARCHIVE_PATH}" \
    -C "${REPO_ROOT}/results" \
    "cluster_run_${RUN_ID}" \
    "real_sim_comparison_results.csv" \
    "real_sim_detailed_results.csv" \
    "real_sim_all_sweeps.csv" \
    "Cluster_Real_Simulation_Report.md" \
    "mapping_comparison_results.csv"

echo ""
echo "======================================================================"
echo " ALL BENCHMARKS COMPLETED SUCCESSFULLY!"
echo " Archive ready for download: ${ARCHIVE_PATH}"
echo " End Time: $(date --iso-8601=seconds 2>/dev/null || date)"
echo "======================================================================"

