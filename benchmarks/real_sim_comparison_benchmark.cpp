// real_sim_comparison_benchmark.cpp
//
// Runs the real MPS simulator (SimulatorsFactory::CreateSimulator) for both
// BuildChain and RigidSeedLocalSearch, measures wall-clock simulation time,
// and prints a side-by-side comparison.
//
// Usage:
//   ./real_sim_comparison circuit.qasm [circuit2.qasm ...]
//       [--cost-model cubic|svd] [--bond-dim N] [--reps R]

#include <chrono>
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
#define INCLUDED_BY_FACTORY
#include "Simulators/QCSimState.h"
#include "Simulators/Factory.h"
#include "Simulators/MPSSvdCollector.h"

// --------------------------------------------------------------------------
// Helper: run one real simulation and return wall-clock ms
// --------------------------------------------------------------------------
static double runRealSim(
    size_t                                    nrQubits,
    const std::vector<long long int>&         qubitMap,
    const std::shared_ptr<Circuits::Circuit<>>& circuit,
    Simulators::MPSDummySimulator::CostModel  costModel,
    int                                       maxBond)
{
    auto realSim = Simulators::SimulatorsFactory::CreateSimulator(
        Simulators::SimulatorType::kQCSim,
        Simulators::SimulationType::kMatrixProductState);

    if (!realSim) return -1.0;

    realSim->AllocateQubits(nrQubits);
    realSim->Initialize();
    realSim->Configure("matrix_product_state_max_bond_dimension",
                       std::to_string(maxBond).c_str());

    if (auto qcs = std::dynamic_pointer_cast<Simulators::Private::QCSimState>(realSim)) {
        qcs->setCostModel(costModel);
        // SetUpcomingGates takes a flat vector of IOperation pointers
        const auto& ops = circuit->GetOperations();
        qcs->SetUpcomingGates(ops);
    }

    realSim->SetInitialQubitsMap(qubitMap);

    auto t0 = std::chrono::steady_clock::now();

    Circuits::OperationState state;
    state.AllocateBits(nrQubits);
    circuit->Execute(realSim, state);

    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// --------------------------------------------------------------------------
int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " circuit.qasm [circuit2.qasm ...]"
                     " [--cost-model cubic|svd] [--bond-dim N] [--reps R]\n";
        return 1;
    }

    // --- parse args ---
    std::vector<std::string> files;
    Simulators::MPSDummySimulator::CostModel costModel =
        Simulators::MPSDummySimulator::CostModel::CentralBondCubic;
    int maxBond = 64;
    int reps    = 3;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--cost-model" && i+1 < argc) {
            ++i;
            if (std::string(argv[i]) == "svd")
                costModel = Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry;
        } else if (a == "--bond-dim" && i+1 < argc) {
            maxBond = std::atoi(argv[++i]);
        } else if (a == "--reps" && i+1 < argc) {
            reps = std::atoi(argv[++i]);
        } else if (a[0] != '-') {
            files.push_back(a);
        }
    }

    if (files.empty()) { std::cerr << "No QASM files given.\n"; return 1; }

    // --- header ---
    std::string sep(100, '-');
    std::cout << sep << "\n";
    std::cout << std::left << std::setw(20) << "Circuit"
              << std::setw(7)  << "Qubits"
              << std::setw(8)  << "Layers"
              << std::setw(9)  << "2Q gates"
              << std::setw(14) << "BC dummy cost"
              << std::setw(14) << "RS dummy cost"
              << std::setw(14) << "BC sim ms"
              << std::setw(14) << "RS sim ms"
              << std::setw(12) << "Speedup"
              << "\n" << sep << "\n";

    // --- CSV output ---
    std::ofstream csv("results/real_sim_comparison_results.csv");
    csv << "circuit,qubits,layers,two_qubit_gates,bond_dim,"
           "bc_dummy_cost,rs_dummy_cost,dummy_cost_delta_pct,"
           "bc_sim_ms_avg,rs_sim_ms_avg,sim_speedup_pct\n";

    for (const auto& fname : files) {
        // load + parse
        std::ifstream f(fname);
        if (!f) { std::cerr << "Cannot open: " << fname << "\n"; continue; }
        std::stringstream buf; buf << f.rdbuf();

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

        // ---- compute mappings via dummy simulator ----
        Simulators::MPSDummySimulator dummySim(nrQubits);
        dummySim.setCostModel(costModel);
        dummySim.SetMaxBondDimension(maxBond);

        // BuildChain mapping + dummy cost
        auto bcMap  = dummySim.ComputeOptimalQubitsMap(layers);
        dummySim.SetInitialQubitsMap(bcMap);
        auto optCirc = circuit->LayersToCircuit(layers);
        for (const auto& op : optCirc->GetOperations()) dummySim.ApplyGate(op);
        double bcDummy = dummySim.getTotalSwappingCost();

        // RigidSeedLocalSearch mapping + dummy cost
        Simulators::MPSDummySimulator dummySim2(nrQubits);
        dummySim2.setCostModel(costModel);
        dummySim2.SetMaxBondDimension(maxBond);
        auto rsMap = Simulators::ComputeInitialMapping(
            dummySim2, layers,
            Simulators::InitialMappingStrategy::RigidSeedLocalSearch);
        dummySim2.SetInitialQubitsMap(rsMap);
        for (const auto& op : optCirc->GetOperations()) dummySim2.ApplyGate(op);
        double rsDummy = dummySim2.getTotalSwappingCost();

        // ---- real simulation timing ----
        // warm-up run for each (not counted)
        runRealSim(nrQubits, bcMap, circuit, costModel, maxBond);
        runRealSim(nrQubits, rsMap, circuit, costModel, maxBond);

        double bcTotal = 0, rsTotal = 0;
        for (int r = 0; r < reps; ++r) {
            bcTotal += runRealSim(nrQubits, bcMap, circuit, costModel, maxBond);
            rsTotal += runRealSim(nrQubits, rsMap, circuit, costModel, maxBond);
        }
        double bcAvg = bcTotal / reps;
        double rsAvg = rsTotal / reps;

        double speedupPct = bcAvg > 0 ? (bcAvg - rsAvg) / bcAvg * 100.0 : 0.0;
        double dummyDelta = bcDummy > 0 ? (rsDummy - bcDummy) / bcDummy * 100.0 : 0.0;

        // short filename for display
        size_t slashPos = fname.rfind('/');
        std::string shortName = (slashPos == std::string::npos) ? fname : fname.substr(slashPos+1);

        std::cout << std::left  << std::setw(20) << shortName
                  << std::right << std::setw(6)  << nrQubits     << " "
                  << std::setw(7)  << layers.size()   << " "
                  << std::setw(8)  << nGates           << " "
                  << std::setw(13) << std::fixed << std::setprecision(1) << bcDummy   << " "
                  << std::setw(13) << rsDummy   << " "
                  << std::setw(13) << bcAvg     << " "
                  << std::setw(13) << rsAvg     << " "
                  << std::setw(11) << std::showpos << speedupPct << "%" << std::noshowpos
                  << "\n";

        csv << shortName << ","
            << nrQubits    << ","
            << layers.size() << ","
            << nGates      << ","
            << maxBond     << ","
            << bcDummy     << ","
            << rsDummy     << ","
            << dummyDelta  << ","
            << bcAvg       << ","
            << rsAvg       << ","
            << speedupPct  << "\n";
    }

    std::cout << sep << "\n";
    std::cout << "  Speedup > 0 means RigidSeedLocalSearch is faster in real simulation\n";
    std::cout << "  Results written to results/real_sim_comparison_results.csv\n";
    return 0;
}
