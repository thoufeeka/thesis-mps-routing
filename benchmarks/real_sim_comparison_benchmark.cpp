// real_sim_comparison_benchmark.cpp
//
// Runs the real MPS simulator (SimulatorsFactory::CreateSimulator) for both
// BuildChain and RigidSeedLocalSearch, measures wall-clock simulation time,
// SVD time, real swap count, and real peak bond dimension, and writes both
// standard and detailed CSV outputs.
//
// Usage:
//   ./real_sim_comparison_benchmark circuit.qasm [circuit2.qasm ...]
//       [--cost-model cubic|svd] [--bond-dim N] [--reps R]
//       [--backend cpu|gpu|auto]
//       [--output FILE] [--detailed-output FILE]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "qasm/QasmCirc.h"
#include "Circuit/Circuit.h"
#include "Simulators/MPSDummySimulator.h"
#include "Simulators/InitialMapping.h"
#include "Simulators/Factory.h"
#define INCLUDED_BY_FACTORY
#include "Simulators/QCSimState.h"
#include "Simulators/GpuState.h"
#include "Simulators/MPSSvdCollector.h"

namespace fs = std::filesystem;

static double applyGatesTrackingPeakBond(
    Simulators::MPSDummySimulator& sim,
    const std::vector<std::shared_ptr<Circuits::IOperation<>>>& gates)
{
    double peak = 1.0;
    for (const auto& op : gates) {
        sim.ApplyGate(op);
        const auto& dims = sim.getCurrentBondDimensions();
        if (!dims.empty()) {
            const double localMax = *std::max_element(dims.begin(), dims.end());
            if (localMax > peak) peak = localMax;
        }
    }
    return peak;
}

struct RealSimMetrics {
    double wallTimeMs     = -1.0;
    double svdTimeMs      = 0.0;
    uint64_t realSwaps    = 0;
    size_t realPeakBond   = 1;
};

// --------------------------------------------------------------------------
// Helper: run one real simulation and return wall-clock ms (+ optional SVD stats)
// --------------------------------------------------------------------------
static RealSimMetrics runRealSim(
    size_t                                      nrQubits,
    const std::vector<long long int>&           qubitMap,
    const std::shared_ptr<Circuits::Circuit<>>& circuit,
    Simulators::MPSDummySimulator::CostModel    costModel,
    int                                         maxBond,
    Simulators::SimulatorType                   simType,
    bool                                        collectSvdStats,
    const std::string&                          circuitName,
    const std::string&                          costModelStr)
{
    RealSimMetrics metrics;

    auto realSim = Simulators::SimulatorsFactory::CreateSimulator(
        simType,
        Simulators::SimulationType::kMatrixProductState);

    if (!realSim) return metrics;

    realSim->AllocateQubits(nrQubits);
    realSim->Initialize();
    realSim->Configure("matrix_product_state_max_bond_dimension",
                       std::to_string(maxBond).c_str());

    const auto& ops = circuit->GetOperations();
    if (auto qcs = std::dynamic_pointer_cast<Simulators::Private::QCSimState>(realSim)) {
        qcs->setCostModel(costModel);
        qcs->SetUpcomingGates(ops);
    }
#ifdef __linux__
    else if (auto gpus = std::dynamic_pointer_cast<Simulators::Private::GpuState>(realSim)) {
        gpus->setCostModel(costModel);
        gpus->SetUpcomingGates(ops);
    }
#endif

    realSim->SetInitialQubitsMap(qubitMap);

    if (collectSvdStats) {
        Simulators::MPSSvdCollector::getInstance().startSession(
            circuitName, costModelStr, 0);
    }

    auto t0 = std::chrono::steady_clock::now();

    Circuits::OperationState state;
    state.AllocateBits(nrQubits);
    circuit->Execute(realSim, state);

    auto t1 = std::chrono::steady_clock::now();
    metrics.wallTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (collectSvdStats) {
        Simulators::MPSSvdCollector::getInstance().stopSession();
        metrics.svdTimeMs =
            static_cast<double>(Simulators::MPSSvdCollector::getInstance().total_svd_time_ns) / 1.0e6;
        metrics.realSwaps =
            Simulators::MPSSvdCollector::getInstance().inserted_swap_count;
        metrics.realPeakBond =
            Simulators::MPSSvdCollector::getInstance().real_peak_bond_dim;
    }

    return metrics;
}

static void computeStats(const std::vector<double>& vals,
                         double& avgOut, double& minOut, double& stdOut)
{
    if (vals.empty()) {
        avgOut = minOut = stdOut = 0.0;
        return;
    }
    double sum = std::accumulate(vals.begin(), vals.end(), 0.0);
    avgOut = sum / static_cast<double>(vals.size());
    minOut = *std::min_element(vals.begin(), vals.end());
    double sqSum = 0.0;
    for (double v : vals) {
        double d = v - avgOut;
        sqSum += d * d;
    }
    stdOut = (vals.size() > 1)
                 ? std::sqrt(sqSum / static_cast<double>(vals.size() - 1))
                 : 0.0;
}

// --------------------------------------------------------------------------
int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " circuit.qasm [circuit2.qasm ...]"
                     " [--cost-model cubic|svd] [--bond-dim N] [--reps R]"
                     " [--backend cpu|gpu|auto]"
                     " [--output FILE] [--detailed-output FILE]\n";
        return 1;
    }

    // --- parse args ---
    std::vector<std::string> files;
    Simulators::MPSDummySimulator::CostModel costModel =
        Simulators::MPSDummySimulator::CostModel::CentralBondCubic;
    int maxBond = 64;
    int reps    = 3;
    std::string backendArg     = "cpu";
    std::string csvPath        = "results/real_sim_comparison_results.csv";
    std::string detailedPath   = "results/real_sim_detailed_results.csv";
    bool appendMode            = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--cost-model" || a == "-c") && i + 1 < argc) {
            ++i;
            std::string val = argv[i];
            if (val == "svd" || val == "ThinSVDGeometry")
                costModel = Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry;
            else
                costModel = Simulators::MPSDummySimulator::CostModel::CentralBondCubic;
        } else if ((a == "--bond-dim" || a == "-b") && i + 1 < argc) {
            maxBond = std::atoi(argv[++i]);
        } else if ((a == "--reps" || a == "-r") && i + 1 < argc) {
            reps = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--backend" && i + 1 < argc) {
            backendArg = argv[++i];
        } else if ((a == "--output" || a == "-o") && i + 1 < argc) {
            csvPath = argv[++i];
        } else if (a == "--detailed-output" && i + 1 < argc) {
            detailedPath = argv[++i];
        } else if (a == "--append") {
            appendMode = true;
        } else if (a[0] != '-') {
            files.push_back(a);
        }
    }

    if (files.empty()) {
        std::cerr << "No QASM files given.\n";
        return 1;
    }

    const std::string costModelStr =
        (costModel == Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry)
            ? "svd" : "cubic";

    // Determine thread count from environment for reporting
    int ompThreads = 1;
    if (const char* envOmp = std::getenv("OMP_NUM_THREADS")) {
        int parsed = std::atoi(envOmp);
        if (parsed > 0) ompThreads = parsed;
    }

    // Determine backends to run
    std::vector<std::pair<Simulators::SimulatorType, std::string>> backends;
    if (backendArg == "cpu" || backendArg == "auto") {
        backends.push_back({Simulators::SimulatorType::kQCSim, "cpu"});
    }
    if (backendArg == "gpu" || backendArg == "auto") {
#ifdef __linux__
        Simulators::SimulatorsFactory::InitGpuLibraryWithMute();
        if (Simulators::SimulatorsFactory::IsGpuLibraryAvailable()) {
            backends.push_back({Simulators::SimulatorType::kGpuSim, "gpu"});
        } else if (backendArg == "gpu") {
            std::cerr << "[INFO] GPU simulator library (libmaestro_gpu_simulators.so) "
                      << "not found in LD_LIBRARY_PATH. Skipping GPU backend.\n";
            return 0;
        } else {
            std::cout << "[INFO] GPU library (libmaestro_gpu_simulators.so) not detected; "
                      << "running CPU (QCSim) backend.\n";
        }
#else
        if (backendArg == "gpu") {
            std::cerr << "[INFO] GPU backend only supported on Linux. Skipping.\n";
            return 0;
        }
#endif
    }

    // Ensure parent directories exist
    if (fs::path(csvPath).has_parent_path())
        fs::create_directories(fs::path(csvPath).parent_path());
    if (fs::path(detailedPath).has_parent_path())
        fs::create_directories(fs::path(detailedPath).parent_path());

    // Open standard CSV
    const bool writeStdHeader = !appendMode || !fs::exists(csvPath) || fs::file_size(csvPath) == 0;
    std::ofstream csv(csvPath, appendMode ? std::ios::app : std::ios::trunc);
    if (writeStdHeader) {
        csv << "circuit,qubits,layers,two_qubit_gates,bond_dim,"
               "bc_dummy_cost,rs_dummy_cost,dummy_cost_delta_pct,"
               "bc_sim_ms_avg,rs_sim_ms_avg,sim_speedup_pct\n";
    }

    // Open detailed CSV
    const bool writeDetHeader = !appendMode || !fs::exists(detailedPath) || fs::file_size(detailedPath) == 0;
    std::ofstream detCsv(detailedPath, appendMode ? std::ios::app : std::ios::trunc);
    if (writeDetHeader) {
        detCsv << "circuit,qubits,layers,two_qubit_gates,backend,cost_model,bond_dim,reps,threads,"
               << "bc_map_ms,rs_map_ms,"
               << "bc_dummy_cost,rs_dummy_cost,dummy_cost_delta_pct,"
               << "bc_dummy_peak_bond,rs_dummy_peak_bond,"
               << "bc_real_swaps,rs_real_swaps,real_swaps_delta_pct,"
               << "bc_real_peak_bond,rs_real_peak_bond,"
               << "bc_svd_ms,rs_svd_ms,svd_speedup_pct,"
               << "bc_sim_ms_avg,bc_sim_ms_min,bc_sim_ms_std,"
               << "rs_sim_ms_avg,rs_sim_ms_min,rs_sim_ms_std,"
               << "sim_speedup_pct,sim_speedup_x\n";
    }

    for (const auto& [simType, backendName] : backends) {
        // --- header ---
        std::string sep(118, '-');
        std::cout << sep << "\n";
        std::cout << "Backend: " << backendName
                  << " | Cost model: " << costModelStr
                  << " | Max bond dim: " << maxBond
                  << " | Reps: " << reps
                  << " | Threads: " << ompThreads << "\n";
        std::cout << sep << "\n";
        std::cout << std::left  << std::setw(22) << "Circuit"
                  << std::right << std::setw(6)  << "Qubits"
                  << std::setw(7)  << "Layers"
                  << std::setw(8)  << "2QGate"
                  << std::setw(13) << "BC dummy"
                  << std::setw(13) << "RS dummy"
                  << std::setw(9)  << "BC swaps"
                  << std::setw(9)  << "RS swaps"
                  << std::setw(11) << "BC sim ms"
                  << std::setw(11) << "RS sim ms"
                  << std::setw(9)  << "Speedup"
                  << "\n" << sep << "\n" << std::flush;

        for (const auto& fname : files) {
            // load + parse
            std::ifstream f(fname);
            if (!f) {
                std::cerr << "Cannot open: " << fname << "\n";
                continue;
            }
            std::stringstream buf;
            buf << f.rdbuf();

            qasm::QasmToCirc<> parser;
            auto circuit = parser.ParseAndTranslate(buf.str());
            if (parser.Failed()) {
                std::cerr << "Parse error in " << fname << ": " << parser.GetErrorMessage() << "\n";
                continue;
            }

            const size_t nrQubits = circuit->GetMaxQubitIndex() + 1;
            auto layers = circuit->ToMultipleQubitsLayers();
            size_t nGates = 0;
            for (const auto& op : circuit->GetOperations())
                if (op->AffectedQubits().size() >= 2) ++nGates;

            auto optCirc = circuit->LayersToCircuit(layers);

            // ---- compute mappings via dummy simulator ----
            Simulators::MPSDummySimulator dummySim(nrQubits);
            dummySim.setCostModel(costModel);
            dummySim.SetMaxBondDimension(maxBond);

            // BuildChain mapping + dummy cost
            auto tMapBc0 = std::chrono::steady_clock::now();
            auto bcMap   = dummySim.ComputeOptimalQubitsMap(layers);
            auto tMapBc1 = std::chrono::steady_clock::now();
            double bcMapMs = std::chrono::duration<double, std::milli>(tMapBc1 - tMapBc0).count();

            dummySim.SetInitialQubitsMap(bcMap);
            double bcDummyPeak = applyGatesTrackingPeakBond(dummySim, optCirc->GetOperations());
            double bcDummy     = dummySim.getTotalSwappingCost();

            // RigidSeedLocalSearch mapping + dummy cost
            Simulators::MPSDummySimulator dummySim2(nrQubits);
            dummySim2.setCostModel(costModel);
            dummySim2.SetMaxBondDimension(maxBond);

            auto tMapRs0 = std::chrono::steady_clock::now();
            auto rsMap   = Simulators::ComputeInitialMapping(
                dummySim2, layers,
                Simulators::InitialMappingStrategy::RigidSeedLocalSearch);
            auto tMapRs1 = std::chrono::steady_clock::now();
            double rsMapMs = std::chrono::duration<double, std::milli>(tMapRs1 - tMapRs0).count();

            dummySim2.SetInitialQubitsMap(rsMap);
            double rsDummyPeak = applyGatesTrackingPeakBond(dummySim2, optCirc->GetOperations());
            double rsDummy     = dummySim2.getTotalSwappingCost();

            // short filename for display
            size_t slashPos = fname.rfind('/');
            std::string shortName = (slashPos == std::string::npos) ? fname : fname.substr(slashPos + 1);

            // ---- real simulation timing ----
            // Warm-up run for each (not counted in wall-clock avg, used to collect exact SVD & swap counts)
            RealSimMetrics bcWarm = runRealSim(
                nrQubits, bcMap, circuit, costModel, maxBond,
                simType, true, shortName, costModelStr);
            RealSimMetrics rsWarm = runRealSim(
                nrQubits, rsMap, circuit, costModel, maxBond,
                simType, true, shortName, costModelStr);

            if (bcWarm.wallTimeMs < 0 || rsWarm.wallTimeMs < 0) {
                std::cerr << "Simulator creation failed for " << shortName
                          << " on backend " << backendName << "\n";
                continue;
            }

            std::vector<double> bcTimes;
            std::vector<double> rsTimes;
            bcTimes.reserve(reps);
            rsTimes.reserve(reps);

            for (int r = 0; r < reps; ++r) {
                auto mBc = runRealSim(
                    nrQubits, bcMap, circuit, costModel, maxBond,
                    simType, false, shortName, costModelStr);
                auto mRs = runRealSim(
                    nrQubits, rsMap, circuit, costModel, maxBond,
                    simType, false, shortName, costModelStr);
                bcTimes.push_back(mBc.wallTimeMs);
                rsTimes.push_back(mRs.wallTimeMs);
            }

            double bcAvg = 0, bcMin = 0, bcStd = 0;
            double rsAvg = 0, rsMin = 0, rsStd = 0;
            computeStats(bcTimes, bcAvg, bcMin, bcStd);
            computeStats(rsTimes, rsAvg, rsMin, rsStd);

            double speedupPct   = bcAvg > 0 ? (bcAvg - rsAvg) / bcAvg * 100.0 : 0.0;
            double speedupX     = rsAvg > 0 ? bcAvg / rsAvg : 1.0;
            double dummyDelta   = bcDummy > 0 ? (rsDummy - bcDummy) / bcDummy * 100.0 : 0.0;
            double swapsDelta   = bcWarm.realSwaps > 0
                                      ? (static_cast<double>(rsWarm.realSwaps) - static_cast<double>(bcWarm.realSwaps))
                                            / static_cast<double>(bcWarm.realSwaps) * 100.0
                                      : 0.0;
            double svdSpeedup   = bcWarm.svdTimeMs > 0
                                      ? (bcWarm.svdTimeMs - rsWarm.svdTimeMs) / bcWarm.svdTimeMs * 100.0
                                      : 0.0;

            std::cout << std::left  << std::setw(22) << shortName
                      << std::right << std::setw(6)  << nrQubits
                      << std::setw(7)  << layers.size()
                      << std::setw(8)  << nGates
                      << std::setw(13) << std::fixed << std::setprecision(1) << bcDummy
                      << std::setw(13) << rsDummy
                      << std::setw(9)  << bcWarm.realSwaps
                      << std::setw(9)  << rsWarm.realSwaps
                      << std::setw(11) << bcAvg
                      << std::setw(11) << rsAvg
                      << std::setw(8)  << std::showpos << speedupPct << "%" << std::noshowpos
                      << "\n" << std::flush;

            csv << shortName     << ","
                << nrQubits      << ","
                << layers.size() << ","
                << nGates        << ","
                << maxBond       << ","
                << bcDummy       << ","
                << rsDummy       << ","
                << dummyDelta    << ","
                << bcAvg         << ","
                << rsAvg         << ","
                << speedupPct    << "\n";
            csv.flush();

            detCsv << shortName             << ","
                   << nrQubits              << ","
                   << layers.size()         << ","
                   << nGates                << ","
                   << backendName           << ","
                   << costModelStr          << ","
                   << maxBond               << ","
                   << reps                  << ","
                   << ompThreads            << ","
                   << std::fixed << std::setprecision(4)
                   << bcMapMs               << ","
                   << rsMapMs               << ","
                   << bcDummy               << ","
                   << rsDummy               << ","
                   << dummyDelta            << ","
                   << bcDummyPeak           << ","
                   << rsDummyPeak           << ","
                   << bcWarm.realSwaps      << ","
                   << rsWarm.realSwaps      << ","
                   << swapsDelta            << ","
                   << bcWarm.realPeakBond   << ","
                   << rsWarm.realPeakBond   << ","
                   << bcWarm.svdTimeMs      << ","
                   << rsWarm.svdTimeMs      << ","
                   << svdSpeedup            << ","
                   << bcAvg                 << ","
                   << bcMin                 << ","
                   << bcStd                 << ","
                   << rsAvg                 << ","
                   << rsMin                 << ","
                   << rsStd                 << ","
                   << speedupPct            << ","
                   << speedupX              << "\n";
            detCsv.flush();
        }

        std::cout << sep << "\n";
        std::cout << "  Speedup > 0 means RigidSeedLocalSearch is faster in real simulation\n";
        std::cout << "  Standard results written to: " << csvPath << "\n";
        std::cout << "  Detailed results written to: " << detailedPath << "\n" << std::flush;
    }

    return 0;
}
