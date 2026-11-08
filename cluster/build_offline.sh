#!/usr/bin/env bash
# ==============================================================================
# cluster/build_offline.sh
#
# Builds Maestro and all thesis MPS routing benchmark executables directly
# using g++ / gcc (NO dependency on cmake, make, git, patch, or internet).
#
# Uses:
#   - cluster/deps/maestro-base-14bf89e.tar.gz  (if maestro/ is not present)
#   - cluster/maestro_patch/                    (pre-patched thesis files)
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

CXX_BIN="${CXX:-g++}"
CC_BIN="${CC:-gcc}"

echo "======================================================================"
echo "[build_offline] Repository root : ${REPO_ROOT}"
echo "[build_offline] Hostname        : $(hostname)"
echo "[build_offline] C++ compiler    : ${CXX_BIN} ($(${CXX_BIN} --version | head -n1))"
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
        rm -rf "${BUILD_DIR}/bin" "${BUILD_DIR}/libmaestro.so" "${BUILD_DIR}/obj"
    fi
fi

# 1. Ensure maestro/ source directory exists
if [[ ! -f "${MAESTRO_DIR}/CMakeLists.txt" ]]; then
    echo "[build_offline] Extracting base Maestro source tree..."
    mkdir -p "${MAESTRO_DIR}"
    tar --no-same-owner -xzf "${REPO_ROOT}/cluster/deps/maestro-base-14bf89e.tar.gz" -C "${MAESTRO_DIR}"
fi

# 2. Apply thesis patch bundle (using cp only — no git or patch required)
echo "[build_offline] Installing thesis files from cluster/maestro_patch/..."
cp -f "${REPO_ROOT}/cluster/maestro_patch/Simulators/InitialMapping.h" "${MAESTRO_DIR}/Simulators/InitialMapping.h"
cp -f "${REPO_ROOT}/cluster/maestro_patch/Simulators/MPSSvdCollector.h" "${MAESTRO_DIR}/Simulators/MPSSvdCollector.h"
mkdir -p "${MAESTRO_DIR}/tests"
cp -f "${REPO_ROOT}/cluster/maestro_patch/tests/mpscostmodeltests.cpp" "${MAESTRO_DIR}/tests/mpscostmodeltests.cpp"
cp -rf "${REPO_ROOT}/cluster/maestro_patch/files/." "${MAESTRO_DIR}/"

if ! grep -q "real_sim_comparison_benchmark" "${MAESTRO_DIR}/CMakeLists.txt"; then
    echo "[build_offline] ERROR: pre-patched files were not installed" >&2
    exit 1
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

BOOST_DIR="${DEPS_DIR}/boost_1_89_0"
if [[ ! -f "${BOOST_DIR}/lib/libboost_json.so" ]]; then
    echo "[build_offline] Building minimal Boost 1.89.0 libraries directly with ${CXX_BIN} (no cmake needed)..."
    rm -rf "${BOOST_DIR}"
    tar --no-same-owner -xzf "${REPO_ROOT}/cluster/deps/boost-1.89.0-subset.tar.gz" -C "${DEPS_DIR}"
    mkdir -p "${BOOST_DIR}/lib" "${BOOST_DIR}/obj" "${BOOST_DIR}/include"
    ln -sfn ../boost "${BOOST_DIR}/include/boost"

    pushd "${BOOST_DIR}" >/dev/null

    # Helper to limit parallel background jobs to NPROC
     throttle_jobs() {
        while [[ "$(jobs -rp | wc -l)" -ge "${NPROC}" ]]; do
            wait -n 2>/dev/null || sleep 0.1
        done
    }

    # 3a. boost_json
    "${CXX_BIN}" -O3 -DNDEBUG -std=gnu++17 -fPIC -pthread -I. \
        -DBOOST_JSON_DYN_LINK=1 -DBOOST_ALL_NO_LIB=1 \
        -shared -Wl,-soname,libboost_json.so \
        libs/json/src/src.cpp -o lib/libboost_json.so &

    # 3b. boost_container
    mkdir -p obj/container
    "${CC_BIN}" -O3 -DNDEBUG -fPIC -pthread -I. \
        -DBOOST_CONTAINER_DYN_LINK=1 -DBOOST_ALL_NO_LIB=1 \
        -c libs/container/src/alloc_lib.c -o obj/container/alloc_lib.o &
    for src in libs/container/src/*.cpp; do
        throttle_jobs
        bname="$(basename "${src}" .cpp)"
        "${CXX_BIN}" -O3 -DNDEBUG -std=gnu++17 -fPIC -pthread -I. \
            -DBOOST_CONTAINER_DYN_LINK=1 -DBOOST_ALL_NO_LIB=1 \
            -c "${src}" -o "obj/container/${bname}.o" &
    done

    # 3c. boost_program_options
    mkdir -p obj/program_options
    for src in libs/program_options/src/*.cpp; do
        throttle_jobs
        bname="$(basename "${src}" .cpp)"
        "${CXX_BIN}" -O3 -DNDEBUG -std=gnu++17 -fPIC -pthread -I. \
            -DBOOST_PROGRAM_OPTIONS_DYN_LINK=1 -DBOOST_ALL_NO_LIB=1 \
            -c "${src}" -o "obj/program_options/${bname}.o" &
    done

    # 3d. boost_serialization (non-wide files)
    mkdir -p obj/serialization
    for src in libs/serialization/src/*.cpp; do
        case "${src}" in
            *_w*.cpp) continue ;;
        esac
        throttle_jobs
        bname="$(basename "${src}" .cpp)"
        "${CXX_BIN}" -O3 -DNDEBUG -std=gnu++17 -fPIC -pthread -I. \
            -DBOOST_SERIALIZATION_DYN_LINK=1 -DBOOST_ALL_NO_LIB=1 \
            -c "${src}" -o "obj/serialization/${bname}.o" &
    done

    wait

    "${CXX_BIN}" -shared -fPIC -pthread -Wl,-soname,libboost_container.so \
        obj/container/*.o -o lib/libboost_container.so &
    "${CXX_BIN}" -shared -fPIC -pthread -Wl,-soname,libboost_program_options.so \
        obj/program_options/*.o -o lib/libboost_program_options.so &
    "${CXX_BIN}" -shared -fPIC -pthread -Wl,-soname,libboost_serialization.so \
        obj/serialization/*.o -o lib/libboost_serialization.so &
    wait

    rm -rf obj
    popd >/dev/null
    echo "[build_offline] Boost libraries built:"
    ls -lh "${BOOST_DIR}/lib/"
fi

# 4. Compile Maestro shared library and the 3 benchmark binaries directly with g++
echo "[build_offline] Compiling Maestro and benchmark executables with ${CXX_BIN}..."
mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj"

EIGEN_INC="${DEPS_DIR}/eigen-5.0.0"
QCSIM_INC="${DEPS_DIR}/QCSim/QCSim"
BOOST_INC="${BOOST_DIR}"
BOOST_LIB="${BOOST_DIR}/lib"

COMMON_CXXFLAGS=(
    -O3 -DNDEBUG -std=gnu++17 -fPIC -fopenmp -pthread
    -mpopcnt -mfma -mavx2
    -Wall -Wno-array-bounds
    -DNO_QISKIT_AER -D_FAST_TESTS
    -DBOOST_CONTAINER_DYN_LINK -DBOOST_CONTAINER_NO_LIB
    -DBOOST_JSON_DYN_LINK -DBOOST_JSON_NO_LIB
    -DBOOST_SERIALIZATION_DYN_LINK -DBOOST_SERIALIZATION_NO_LIB
    -I"${MAESTRO_DIR}"
    -isystem "${EIGEN_INC}"
    -isystem "${BOOST_INC}"
    -isystem "${QCSIM_INC}"
)

# Compile all 5 translation units in parallel!
echo "[build_offline] Compiling 5 C++ translation units in parallel..."

"${CXX_BIN}" "${COMMON_CXXFLAGS[@]}" -Dmaestro_EXPORTS \
    -c "${MAESTRO_DIR}/maestrolib/Interface.cpp" \
    -o "${BUILD_DIR}/obj/Interface.o" &
PID_INTERFACE=$!

"${CXX_BIN}" "${COMMON_CXXFLAGS[@]}" -Dmaestro_EXPORTS \
    -c "${MAESTRO_DIR}/Simulators/Factory.cpp" \
    -o "${BUILD_DIR}/obj/Factory.o" &
PID_FACTORY=$!

"${CXX_BIN}" "${COMMON_CXXFLAGS[@]}" \
    -c "${REPO_ROOT}/benchmarks/real_sim_comparison_benchmark.cpp" \
    -o "${BUILD_DIR}/obj/real_sim_comparison_benchmark.o" &
PID_REALSIM=$!

"${CXX_BIN}" "${COMMON_CXXFLAGS[@]}" \
    -c "${REPO_ROOT}/benchmarks/mapping_comparison_benchmark.cpp" \
    -o "${BUILD_DIR}/obj/mapping_comparison_benchmark.o" &
PID_MAPSIM=$!

"${CXX_BIN}" "${COMMON_CXXFLAGS[@]}" \
    -c "${REPO_ROOT}/benchmarks/mps_benchmark.cpp" \
    -o "${BUILD_DIR}/obj/mps_benchmark.o" &
PID_MPSBENCH=$!

wait "${PID_INTERFACE}" "${PID_FACTORY}"
echo "[build_offline] Linking libmaestro.so..."
"${CXX_BIN}" -shared -fPIC -O3 -DNDEBUG -fopenmp -pthread \
    -Wl,-soname,libmaestro.so \
    -Wl,-rpath,"${BUILD_DIR}:${BOOST_LIB}" \
    "${BUILD_DIR}/obj/Interface.o" \
    "${BUILD_DIR}/obj/Factory.o" \
    -L"${BOOST_LIB}" -lboost_json -lboost_serialization -lboost_container -ldl \
    -o "${BUILD_DIR}/libmaestro.so"

wait "${PID_REALSIM}" "${PID_MAPSIM}" "${PID_MPSBENCH}"
echo "[build_offline] Linking benchmark executables..."

RPATH_FLAGS=(-Wl,-rpath,"${BUILD_DIR}:${BOOST_LIB}" -Wl,-rpath-link,"${BOOST_LIB}")

"${CXX_BIN}" -O3 -DNDEBUG -fopenmp -pthread \
    "${BUILD_DIR}/obj/real_sim_comparison_benchmark.o" \
    "${BUILD_DIR}/libmaestro.so" \
    "${RPATH_FLAGS[@]}" -L"${BOOST_LIB}" -lboost_serialization -ldl \
    -o "${BUILD_DIR}/bin/real_sim_comparison_benchmark"

"${CXX_BIN}" -O3 -DNDEBUG -fopenmp -pthread \
    "${BUILD_DIR}/obj/mapping_comparison_benchmark.o" \
    "${BUILD_DIR}/libmaestro.so" \
    "${RPATH_FLAGS[@]}" -L"${BOOST_LIB}" -lboost_serialization -ldl \
    -o "${BUILD_DIR}/bin/mps_mapping_benchmark"

"${CXX_BIN}" -O3 -DNDEBUG -fopenmp -pthread \
    "${BUILD_DIR}/obj/mps_benchmark.o" \
    "${BUILD_DIR}/libmaestro.so" \
    "${RPATH_FLAGS[@]}" -L"${BOOST_LIB}" -lboost_serialization -ldl \
    -o "${BUILD_DIR}/bin/mps_benchmark"

echo "======================================================================"
echo "[build_offline] Build succeeded! Executables ready in:"
ls -lh "${BUILD_DIR}/bin/real_sim_comparison_benchmark" \
       "${BUILD_DIR}/bin/mps_mapping_benchmark" \
       "${BUILD_DIR}/bin/mps_benchmark"
echo "======================================================================"
