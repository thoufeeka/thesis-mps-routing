// mapping_comparison_benchmark.cpp
//
// Compares initial qubit mapping strategies using Maestro's MPS dummy-simulator
// cost model (no real simulation).
//
// Strategies:
//   BuildChain           - existing Maestro heuristic (baseline)
//   CommunityMapping     - CNM greedy modularity community placement
//   TemporalComm_a1/a2/a4 - temporal community mapping with various decay rates
//   BondAwarePlacement   - SA that minimises MPS bond cost directly
//
// Usage: ./mps_mapping_benchmark [OPTIONS] circuit1.qasm [circuit2.qasm ...]
//
// Options:
//   --cost-model cubic|svd   cost model for dummy simulator  (default: cubic)
//   --bond-dim N             max bond dimension              (default: 64)
//   --output FILE            CSV output file
//   --verbose                print detected communities per circuit
//
// CSV columns:
//   circuit, qubits, layers, two_qubit_gates,
//   cost_model, bond_dim, strategy, communities,
//   predicted_cost, peak_bond_dim, mapping_time_ms

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

// Maestro headers (include path set by CMake)
#include "qasm/QasmCirc.h"
#include "Circuit/Circuit.h"
#include "Simulators/MPSDummySimulator.h"
#include "Simulators/InitialMapping.h"

// Apply all gates to the dummy sim and track the peak bond dimension seen.
static double ApplyGatesTrackingPeakBond(
    Simulators::MPSDummySimulator&                                  sim,
    const std::vector<std::shared_ptr<Circuits::IOperation<>>>&     gates)
{
    double peak = 1.0;
    for (const auto& gate : gates) {
        sim.ApplyGate(gate);
        const auto& dims = sim.getCurrentBondDimensions();
        if (!dims.empty()) {
            const double localMax =
                *std::max_element(dims.begin(), dims.end());
            if (localMax > peak) peak = localMax;
        }
    }
    return peak;
}

// Per-circuit result structs

struct StrategyResult {
    std::string  strategyName;
    int          numCommunities = 1;
    double       predictedCost  = 0.0;
    double       peakBondDim    = 1.0;
    double       mappingTimeMs  = 0.0;
    std::vector<long long int>              map;
    std::vector<std::vector<long long int>> communityGroups;  // non-empty for CommunityMapping
};

struct CircuitResult {
    std::string              circuitName;
    size_t                   nrQubits        = 0;
    size_t                   nrLayers        = 0;
    size_t                   nrTwoQubitGates = 0;
    std::vector<StrategyResult> strategies;
};

// Evaluate one strategy on one circuit and return the results.

static StrategyResult EvaluateStrategy(
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>&    layers,
    const std::vector<std::shared_ptr<Circuits::IOperation<>>>& gateSeq,
    size_t                                                       nrQubits,
    int                                                          bondDim,
    Simulators::MPSDummySimulator::CostModel                     costModel,
    Simulators::InitialMappingStrategy                           strategy,
    double                                                       temporalAlpha = 2.0,
    const std::string&                                           displayName = "")
{
    StrategyResult res;
    res.strategyName = displayName.empty()
                           ? Simulators::GetStrategyName(strategy)
                           : displayName;

    // fresh dummy simulator for the mapping phase
    Simulators::MPSDummySimulator mapSim(nrQubits);
    mapSim.setCostModel(costModel);
    mapSim.SetMaxBondDimension(bondDim);

    // compute mapping, timed
    const auto t0 = std::chrono::steady_clock::now();
    const Simulators::MappingResult mr =
        Simulators::ComputeMappingWithInfo(mapSim, layers, strategy, temporalAlpha);
    const auto t1 = std::chrono::steady_clock::now();

    res.mappingTimeMs   = std::chrono::duration<double, std::milli>(t1 - t0).count();
    res.map             = mr.map;
    res.numCommunities  = mr.numCommunities;
    res.communityGroups = mr.communityGroups;

    // evaluate cost with a separate fresh simulator
    Simulators::MPSDummySimulator evalSim(nrQubits);
    evalSim.setCostModel(costModel);
    evalSim.SetMaxBondDimension(bondDim);
    evalSim.SetInitialQubitsMap(res.map);

    res.peakBondDim   = ApplyGatesTrackingPeakBond(evalSim, gateSeq);
    res.predictedCost = evalSim.getTotalSwappingCost();

    return res;
}

// main

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr
            << "Usage: " << argv[0]
            << " [--cost-model cubic|svd] [--bond-dim N] [--output FILE]"
               " [--verbose]"
               " [--eval-mapping q0,q1,...,qN --strategy-name NAME]"
               " circuit1.qasm ...\n";
        return 1;
    }

    // parse command-line arguments
    Simulators::MPSDummySimulator::CostModel costModel =
        Simulators::MPSDummySimulator::CostModel::CentralBondCubic;
    int         bondDim      = 64;
    std::string outputFile   = "mapping_comparison_results.csv";
    bool        verbose      = false;
    std::string evalMapping;   // non-empty → external mapping mode
    std::string strategyName;  // display name for external mapping
    std::vector<std::string> qasmFiles;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--cost-model" || arg == "-c") && i + 1 < argc) {
            const std::string val = argv[++i];
            if (val == "svd" || val == "ThinSVDGeometry")
                costModel = Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry;
        } else if ((arg == "--bond-dim" || arg == "-b") && i + 1 < argc) {
            bondDim = std::atoi(argv[++i]);
        } else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            outputFile = argv[++i];
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--eval-mapping" && i + 1 < argc) {
            evalMapping = argv[++i];   // comma-separated permutation
        } else if (arg == "--strategy-name" && i + 1 < argc) {
            strategyName = argv[++i];
        } else if (!arg.empty() && arg[0] != '-') {
            qasmFiles.push_back(arg);
        }
    }

    if (qasmFiles.empty()) {
        std::cerr << "Error: No QASM files provided.\n";
        return 1;
    }

    const std::string costModelStr =
        (costModel == Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry)
            ? "svd" : "cubic";

    // external mapping mode: evaluate a pre-computed mapping from Python and exit
    if (!evalMapping.empty()) {
        if (strategyName.empty()) strategyName = "ExternalMapping";

        // parse the comma-separated permutation
        std::vector<long long int> extMap;
        {
            std::stringstream ss(evalMapping);
            std::string token;
            while (std::getline(ss, token, ',')) {
                if (!token.empty())
                    extMap.push_back(std::stoll(token));
            }
        }

        // write CSV header only if the file is new
        const bool writeHdr = [&] {
            std::ifstream chk(outputFile); return !chk.good();
        }();
        std::ofstream csv(outputFile, std::ios::app);
        if (!csv.is_open()) {
            std::cerr << "Cannot open output file: " << outputFile << "\n";
            return 1;
        }
        if (writeHdr)
            csv << "circuit,qubits,layers,two_qubit_gates,"
                   "cost_model,bond_dim,strategy,communities,"
                   "predicted_cost,peak_bond_dim,mapping_time_ms\n";

        for (const auto& filename : qasmFiles) {
            std::ifstream file(filename);
            if (!file) { std::cerr << "Cannot open " << filename << "\n"; continue; }
            std::stringstream buf; buf << file.rdbuf();
            qasm::QasmToCirc<> parser;
            auto circuit = parser.ParseAndTranslate(buf.str());
            if (parser.Failed()) { std::cerr << "Parse error in " << filename << "\n"; continue; }

            const size_t nrQubits = circuit->GetMaxQubitIndex() + 1;
            const auto layers     = circuit->ToMultipleQubitsLayers();
            const auto optCirc    = circuit->LayersToCircuit(layers);
            const auto& gateSeq   = optCirc->GetOperations();

            size_t nrTwoQ = 0;
            for (const auto& op : gateSeq)
                if (op->AffectedQubits().size() >= 2) ++nrTwoQ;

            // strip path, keep basename
            std::string cname = filename;
            {
                auto slash = cname.find_last_of("/\\");
                if (slash != std::string::npos) cname = cname.substr(slash + 1);
            }

            // make sure the provided mapping matches this circuit
            if (extMap.size() != nrQubits) {
                std::cerr << "Mapping size " << extMap.size()
                          << " != circuit qubits " << nrQubits
                          << " for " << cname << " — skipping.\n";
                continue;
            }

            // run cost evaluation with the external mapping
            Simulators::MPSDummySimulator evalSim(nrQubits);
            evalSim.setCostModel(costModel);
            evalSim.SetMaxBondDimension(bondDim);
            evalSim.SetInitialQubitsMap(extMap);

            const double peakBond   = ApplyGatesTrackingPeakBond(evalSim, gateSeq);
            const double cost       = evalSim.getTotalSwappingCost();

            // console and CSV output
            std::cout << cname << " | " << strategyName
                      << " | cost=" << std::fixed << std::setprecision(2) << cost
                      << " | peak_bond=" << peakBond << "\n";

            // CSV output
            csv << cname        << ","
                << nrQubits     << ","
                << layers.size()<< ","
                << nrTwoQ       << ","
                << costModelStr << ","
                << bondDim      << ","
                << strategyName << ","
                << 1            << ","   // communities = 1 (not applicable)
                << std::fixed << std::setprecision(4)
                << cost         << ","
                << peakBond     << ","
                << 0.0          << "\n"; // mapping time handled in Python
        }
        return 0;
    }

    // strategies to run — add new variants here
    // {strategy enum, temporal alpha, display name for CSV}
    struct StrategyConfig {
        Simulators::InitialMappingStrategy strategy;
        double      alpha;       // only meaningful for TemporalCommunityMapping
        std::string displayName; // shown in CSV and console
    };
    const std::vector<StrategyConfig> strategies = {
        {Simulators::InitialMappingStrategy::BuildChain,               2.0, "BuildChain"},
        {Simulators::InitialMappingStrategy::CommunityMapping,         0.0, "CommunityMapping"},
        {Simulators::InitialMappingStrategy::TemporalCommunityMapping, 1.0, "TemporalComm_a1"},
        {Simulators::InitialMappingStrategy::TemporalCommunityMapping, 2.0, "TemporalComm_a2"},
        {Simulators::InitialMappingStrategy::TemporalCommunityMapping, 4.0, "TemporalComm_a4"},
        {Simulators::InitialMappingStrategy::BondAwarePlacement,       0.0, "BondAwarePlacement"},
        {Simulators::InitialMappingStrategy::LayerPriorityMapping,     0.0, "LayerPriorityMapping"},
        {Simulators::InitialMappingStrategy::NewPairsFirstMapping,     0.0, "NewPairsFirstMapping"},
        {Simulators::InitialMappingStrategy::FreqSeededChain,          0.0, "FreqSeededChain"},
        {Simulators::InitialMappingStrategy::WeightedMergeChain,       0.0, "WeightedMergeChain"},
        {Simulators::InitialMappingStrategy::LookaheadGroupW3,         0.0, "LookaheadGroupW3"},
        {Simulators::InitialMappingStrategy::LookaheadGroupW5,         0.0, "LookaheadGroupW5"},
        {Simulators::InitialMappingStrategy::RigidSeedLocalSearch,     0.0, "RigidSeedLocalSearch"},
        {Simulators::InitialMappingStrategy::PeripheralPinning,        0.0, "PeripheralPinning"},
    };

    // open (or append to) CSV output
    const bool writeHeader = [&] {
        std::ifstream check(outputFile);
        return !check.good();
    }();

    std::ofstream csv(outputFile, std::ios::app);
    if (!csv.is_open()) {
        std::cerr << "Cannot open output file: " << outputFile << "\n";
        return 1;
    }
    if (writeHeader) {
        csv << "circuit,qubits,layers,two_qubit_gates,"
               "cost_model,bond_dim,"
               "strategy,communities,"
               "predicted_cost,peak_bond_dim,mapping_time_ms\n";
    }

    std::vector<CircuitResult> allResults;

    // process each QASM file
    for (const auto& filename : qasmFiles) {
        // Load
        std::ifstream file(filename);
        if (!file) {
            std::cerr << "Warning: Cannot open " << filename << " – skipping.\n";
            continue;
        }
        std::stringstream buf;
        buf << file.rdbuf();

        // parse
        qasm::QasmToCirc<> parser;
        auto circuit = parser.ParseAndTranslate(buf.str());
        if (parser.Failed()) {
            std::cerr << "Warning: QASM parse error in " << filename
                      << ": " << parser.GetErrorMessage() << " – skipping.\n";
            continue;
        }

        const size_t nrQubits = circuit->GetMaxQubitIndex() + 1;
        if (nrQubits == 0) {
            std::cerr << "Warning: Circuit " << filename
                      << " has no qubits – skipping.\n";
            continue;
        }

        // decompose into layers once (shared across all strategies)
        const auto layers  = circuit->ToMultipleQubitsLayers();
        const auto optCirc = circuit->LayersToCircuit(layers);
        const auto& gateSeq = optCirc->GetOperations();

        size_t nrTwoQ = 0;
        for (const auto& op : gateSeq)
            if (op->AffectedQubits().size() >= 2) ++nrTwoQ;

        // strip path, keep basename
        std::string circName = filename;
        {
            const auto slash = circName.find_last_of("/\\");
            if (slash != std::string::npos) circName = circName.substr(slash + 1);
        }

        CircuitResult cr;
        cr.circuitName     = circName;
        cr.nrQubits        = nrQubits;
        cr.nrLayers        = layers.size();
        cr.nrTwoQubitGates = nrTwoQ;

        // Header for this circuit
        std::cout << "\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        std::cout << " Circuit : " << circName << "\n";
        std::cout << " Qubits  : " << nrQubits << "   Layers: " << layers.size()
                  << "   2Q-gates: " << nrTwoQ << "\n";
        std::cout << " Model   : " << costModelStr << "   BondDim: " << bondDim << "\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        std::cout << std::left
                  << std::setw(20) << "Strategy"
                  << std::setw(8)  << "Comms"
                  << std::setw(16) << "Cost"
                  << std::setw(14) << "PeakBond"
                  << std::setw(14) << "MapTimeMs"
                  << "\n";
        std::cout << std::string(72, '-') << "\n";

        // evaluate each strategy
        for (const auto& strat : strategies) {
            StrategyResult sr = EvaluateStrategy(
                layers, gateSeq, nrQubits, bondDim, costModel,
                strat.strategy, strat.alpha, strat.displayName);
            cr.strategies.push_back(sr);

            // console row
            std::cout << std::left
                      << std::setw(22) << sr.strategyName
                      << std::setw(8)  << sr.numCommunities
                      << std::setw(16) << std::fixed << std::setprecision(2)
                      << sr.predictedCost
                      << std::setw(14) << sr.peakBondDim
                      << std::setw(14) << sr.mappingTimeMs
                      << "\n";

            // CSV row
            csv << circName          << ","
                << nrQubits          << ","
                << layers.size()     << ","
                << nrTwoQ            << ","
                << costModelStr      << ","
                << bondDim           << ","
                << sr.strategyName   << ","
                << sr.numCommunities << ","
                << std::fixed << std::setprecision(4)
                << sr.predictedCost  << ","
                << sr.peakBondDim    << ","
                << sr.mappingTimeMs  << "\n";
        }

        // print improvement vs baseline
        if (cr.strategies.size() >= 2) {
            const double baseCost = cr.strategies[0].predictedCost;  // BuildChain
            for (size_t si = 1; si < cr.strategies.size(); ++si) {
                const double improvPct =
                    baseCost > 1e-9
                        ? 100.0 * (baseCost - cr.strategies[si].predictedCost) / baseCost
                        : 0.0;
                std::cout << "  → " << cr.strategies[si].strategyName
                          << " vs BuildChain: ";
                if (improvPct > 0)
                    std::cout << "+" << std::fixed << std::setprecision(1)
                              << improvPct << "% improvement\n";
                else
                    std::cout << std::fixed << std::setprecision(1)
                              << improvPct << "% (worse / neutral)\n";
            }
        }

        // verbose community output
        if (verbose) {
            for (const auto& sr : cr.strategies) {
                if (sr.communityGroups.empty()) continue;
                std::cout << "  Communities (" << sr.strategyName << "):\n";
                for (size_t ci = 0; ci < sr.communityGroups.size(); ++ci) {
                    std::cout << "    C" << ci << ": {";
                    bool first = true;
                    for (long long int q : sr.communityGroups[ci]) {
                        if (!first) std::cout << ", ";
                        std::cout << q;
                        first = false;
                    }
                    std::cout << "}\n";
                }
            }
        }

        allResults.push_back(std::move(cr));
    }

    csv.close();

    // aggregate summary across all circuits
    if (allResults.size() > 1) {
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════╗\n";
        std::cout << "║              AGGREGATE SUMMARY                       ║\n";
        std::cout << "╠══════════════════════════════════════════════════════╣\n";

        // For each non-baseline strategy, compute average improvement over BuildChain
        const size_t numStrat = strategies.size();
        if (numStrat >= 2) {
            // collect improvements per non-baseline strategy
            std::vector<std::vector<double>> improvements(numStrat - 1);

            for (const auto& cr : allResults) {
                if (cr.strategies.empty()) continue;
                const double baseCost = cr.strategies[0].predictedCost;
                for (size_t si = 1; si < cr.strategies.size(); ++si) {
                    if (baseCost > 1e-9) {
                        const double imp =
                            100.0 * (baseCost - cr.strategies[si].predictedCost)
                            / baseCost;
                        improvements[si - 1].push_back(imp);
                    }
                }
            }

            for (size_t si = 1; si < numStrat; ++si) {
                const auto& imps = improvements[si - 1];
                if (imps.empty()) continue;
                const double avg =
                    std::accumulate(imps.begin(), imps.end(), 0.0) / imps.size();
                const double mn = *std::min_element(imps.begin(), imps.end());
                const double mx = *std::max_element(imps.begin(), imps.end());

                // wins/ties/losses with 0.1% threshold
                int wins = 0, ties = 0, losses = 0;
                for (double v : imps) {
                    if (v > 0.1) ++wins;
                    else if (v < -0.1) ++losses;
                    else ++ties;
                }

                std::cout << "║  " << strategies[si].displayName
                          << " vs BuildChain:\n";
                std::cout << "║    Avg improvement : "
                          << std::fixed << std::setprecision(1) << avg << "%\n";
                std::cout << "║    Range           : ["
                          << std::fixed << std::setprecision(1) << mn << "%, "
                          << mx << "%]\n";
                std::cout << "║    Wins/Ties/Losses: "
                          << wins << "/" << ties << "/" << losses << "\n";
            }
        }

        std::cout << "╚══════════════════════════════════════════════════════╝\n";
    }

    std::cout << "\nResults written to: " << outputFile << "\n";
    return 0;
}
