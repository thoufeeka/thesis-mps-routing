#!/usr/bin/env bash
# ==============================================================================
# cluster/build_offline.sh
#
# Builds Maestro and all thesis MPS routing benchmark executables completely
# offline (zero internet access required on compute nodes).
#
# Uses:
#   - cluster/deps/maestro-base-14bf89e.tar.gz  (if maestro/ is not present)
#   - cluster/maestro_patch/                    (thesis routing & SVD patches)
#   - cluster/deps/eigen-5.0.0-headers.tar.gz   (Eigen 5.0.0 headers)
#   - cluster/deps/qcsim-headers.tar.gz         (QCSim headers + SVD collector)
#   - cluster/deps/boost-1.89.0-subset.tar.gz   (Minimal Boost 1.89.0 subset)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

FORCE_REBUILD=0
if [[ "${1:-}" == "--force" ]]; then
    FORCE_REBUILD=1
fi

# Determine parallel build jobs (respect Slurm allocation if present)
if [[ -n "${SLURM_CPUS_PER_TASK:-}" ]]; then
    NPROC="${SLURM_CPUS_PER_TASK}"
elif command -v nproc >/dev/null 2>&1; then
    NPROC="$(nproc)"
else
    NPROC=4
fi

echo "======================================================================"
echo "[build_offline] Repository root : ${REPO_ROOT}"
echo "[build_offline] Hostname        : $(hostname)"
echo "[build_offline] Parallel jobs   : ${NPROC}"
echo "======================================================================"

MAESTRO_DIR="${REPO_ROOT}/maestro"
BUILD_DIR="${MAESTRO_DIR}/build"
DEPS_DIR="${BUILD_DIR}/deps_offline"

# Determine Boost library directory (either offline build or pre-existing local build)
get_boost_libdir() {
    if [[ -d "${DEPS_DIR}/boost_1_89_0/lib" ]]; then
        echo "${DEPS_DIR}/boost_1_89_0/lib"
    elif [[ -d "${BUILD_DIR}/boost_1_89_0/lib" ]]; then
        echo "${BUILD_DIR}/boost_1_89_0/lib"
    else
        echo ""
    fi
}

# Check if binaries are already built and runnable on this machine
if [[ "${FORCE_REBUILD}" -eq 0 ]] && \
   [[ -x "${BUILD_DIR}/bin/real_sim_comparison_benchmark" ]] && \
   [[ -x "${BUILD_DIR}/bin/mps_mapping_benchmark" ]] && \
   [[ -x "${BUILD_DIR}/bin/mps_benchmark" ]]; then
    BOOST_LIBDIR="$(get_boost_libdir)"
    export LD_LIBRARY_PATH="${BUILD_DIR}:${BOOST_LIBDIR}:${LD_LIBRARY_PATH:-}"
    # Run with no args (exits with usage code 1, or 127/134/139 if shared libs/GLIBC mismatch)
    set +e
    "${BUILD_DIR}/bin/real_sim_comparison_benchmark" >/dev/null 2>&1
    RC=$?
    set -e
    if [[ "${RC}" -eq 1 ]]; then
        echo "[build_offline] Existing binaries in ${BUILD_DIR}/bin are valid for this node. Skipping rebuild."
        echo "                (Pass --force to rebuild from scratch.)"
        exit 0
    else
        echo "[build_offline] Existing binaries cannot run on this node (rc=${RC}); rebuilding for cluster environment..."
        rm -rf "${BUILD_DIR}/CMakeCache.txt" "${BUILD_DIR}/CMakeFiles" "${BUILD_DIR}/bin" "${BUILD_DIR}/libmaestro.so"
    fi
fi

# 1. Ensure maestro/ source directory exists
if [[ ! -f "${MAESTRO_DIR}/CMakeLists.txt" ]]; then
    echo "[build_offline] Extracting base Maestro source tree..."
    mkdir -p "${MAESTRO_DIR}"
    tar --no-same-owner -xzf "${REPO_ROOT}/cluster/deps/maestro-base-14bf89e.tar.gz" -C "${MAESTRO_DIR}"
fi

# 2. Apply thesis patch bundle (idempotent)
echo "[build_offline] Applying thesis patch bundle from cluster/maestro_patch/..."
cp -f "${REPO_ROOT}/cluster/maestro_patch/Simulators/InitialMapping.h" "${MAESTRO_DIR}/Simulators/InitialMapping.h"
cp -f "${REPO_ROOT}/cluster/maestro_patch/Simulators/MPSSvdCollector.h" "${MAESTRO_DIR}/Simulators/MPSSvdCollector.h"
mkdir -p "${MAESTRO_DIR}/tests"
cp -f "${REPO_ROOT}/cluster/maestro_patch/tests/mpscostmodeltests.cpp" "${MAESTRO_DIR}/tests/mpscostmodeltests.cpp"

if ! grep -q "real_sim_comparison_benchmark" "${MAESTRO_DIR}/CMakeLists.txt"; then
    # No dependency on git/patch (missing on compute nodes): overwrite the 5 modified
    # files with their pre-patched versions (base commit 14bf89e + thesis changes).
    echo "[build_offline] Installing pre-patched Maestro files..."
    cp -rf "${REPO_ROOT}/cluster/maestro_patch/files/." "${MAESTRO_DIR}/"
    if ! grep -q "real_sim_comparison_benchmark" "${MAESTRO_DIR}/CMakeLists.txt"; then
        echo "[build_offline] ERROR: pre-patched files were not installed" >&2
        exit 1
    fi
else
    echo "[build_offline] Maestro already patched."
fi

# 3. Unpack offline C++ dependencies (Eigen 5, QCSim, Boost 1.89 subset)
mkdir -p "${DEPS_DIR}"

if [[ ! -d "${DEPS_DIR}/eigen-5.0.0/Eigen" ]]; then
    echo "[build_offline] Extracting Eigen 5.0.0 headers..."
    tar --no-same-owner -xzf "${REPO_ROOT}/cluster/deps/eigen-5.0.0-headers.tar.gz" -C "${DEPS_DIR}"
fi

if [[ ! -d "${DEPS_DIR}/QCSim/QCSim" ]]; then
    echo "[build_offline] Extracting QCSim headers (with MPSSvdCollector hook)..."
    tar --no-same-owner -xzf "${REPO_ROOT}/cluster/deps/qcsim-headers.tar.gz" -C "${DEPS_DIR}"
fi

if [[ ! -f "${DEPS_DIR}/boost_1_89_0/lib/libboost_json.so" ]]; then
    echo "[build_offline] Building minimal Boost 1.89.0 libraries offline..."
    rm -rf "${DEPS_DIR}/boost_1_89_0"
    tar --no-same-owner -xzf "${REPO_ROOT}/cluster/deps/boost-1.89.0-subset.tar.gz" -C "${DEPS_DIR}"
    mkdir -p "${DEPS_DIR}/boost_1_89_0/build_cmake"
    pushd "${DEPS_DIR}/boost_1_89_0/build_cmake" >/dev/null
    cmake -DCMAKE_BUILD_TYPE=Release ..
    make -j"${NPROC}"
    popd >/dev/null
    rm -rf "${DEPS_DIR}/boost_1_89_0/build_cmake"
fi

# 4. Configure and build Maestro + thesis benchmarks with CMake (100% offline)
echo "[build_offline] Configuring and compiling Maestro & benchmark binaries..."
mkdir -p "${BUILD_DIR}"
pushd "${BUILD_DIR}" >/dev/null

# If CMakeCache.txt came from another machine/directory, clean it
if [[ -f CMakeCache.txt ]]; then
    CACHED_DIR="$(grep -E '^CMAKE_HOME_DIRECTORY:INTERNAL=' CMakeCache.txt | cut -d= -f2 || true)"
    if [[ -n "${CACHED_DIR}" && "${CACHED_DIR}" != "${MAESTRO_DIR}" ]]; then
        echo "[build_offline] Cleaning stale CMake cache from ${CACHED_DIR}..."
        rm -rf CMakeCache.txt CMakeFiles
    fi
fi

export EIGEN5_INCLUDE_DIR="${DEPS_DIR}/eigen-5.0.0"
export QCSIM_INCLUDE_DIR="${DEPS_DIR}/QCSim/QCSim"
export BOOST_ROOT="${DEPS_DIR}/boost_1_89_0"
export NO_QISKIT_AER=1
export NO_TESTS=1

cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_PYTHON_BINDINGS=OFF \
    -DEIGEN5_INCLUDE_DIR="${EIGEN5_INCLUDE_DIR}" \
    -DQCSIM_INCLUDE_DIR="${QCSIM_INCLUDE_DIR}" \
    -DBOOST_ROOT="${BOOST_ROOT}" \
    ..

make -j"${NPROC}" maestro real_sim_comparison_benchmark mps_mapping_benchmark mps_benchmark
popd >/dev/null

echo "======================================================================"
echo "[build_offline] Build succeeded! Executables ready in:"
ls -lh "${BUILD_DIR}/bin/real_sim_comparison_benchmark" \
       "${BUILD_DIR}/bin/mps_mapping_benchmark" \
       "${BUILD_DIR}/bin/mps_benchmark"
echo "======================================================================"
