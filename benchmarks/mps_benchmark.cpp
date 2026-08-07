#include <iostream>
#include <fstream>
#include <sstream>
#include <numeric>
#include <algorithm>
#include <chrono>
#include <memory>

#include "qasm/QasmCirc.h"

#include "Circuit/Circuit.h"
#include "Simulators/MPSDummySimulator.h"
#define INCLUDED_BY_FACTORY
#include "Simulators/QCSimState.h"
#include "Simulators/Factory.h"
#include "Simulators/MPSSvdCollector.h"

// Apply every gate one-by-one and sample getCurrentBondDimensions() after
// each one. Returns the highest bond dimension seen at any bond at any
// point during execution.
double applyGatesTrackingPeakBond(
    Simulators::MPSDummySimulator& sim,
    const std::vector<std::shared_ptr<Circuits::IOperation<>>>& gates)
{
    double peakBond = 1.0; // product state starts at 1

    for (const auto& gate : gates)
    {
        sim.ApplyGate(gate);

        const auto& dims = sim.getCurrentBondDimensions();
        if (!dims.empty())
        {
            const double localMax = *std::max_element(
                dims.begin(), dims.end());
            if (localMax > peakBond)
                peakBond = localMax;
        }
    }

    return peakBond;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr
            << "Usage: "
            << argv[0]
            << " circuit.qasm [--cost-model cubic|svd] [--warmups W] [--repetitions R]\n";

        return 1;
    }

    std::string filename;
    Simulators::MPSDummySimulator::CostModel costModel =
        Simulators::MPSDummySimulator::CostModel::CentralBondCubic;
    int numWarmups = 1;
    int numRepetitions = 3;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--cost-model" || arg == "-c")
        {
            if (i + 1 < argc)
            {
                std::string val = argv[++i];
                if (val == "svd" || val == "ThinSVDGeometry")
                    costModel = Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry;
                else
                    costModel = Simulators::MPSDummySimulator::CostModel::CentralBondCubic;
            }
        }
        else if (arg == "--warmups" || arg == "-w")
        {
            if (i + 1 < argc)
            {
                numWarmups = std::atoi(argv[++i]);
            }
        }
        else if (arg == "--repetitions" || arg == "-r")
        {
            if (i + 1 < argc)
            {
                numRepetitions = std::atoi(argv[++i]);
            }
        }
        else if (filename.empty() && arg[0] != '-')
        {
            filename = arg;
        }
    }

    if (filename.empty())
    {
        std::cerr << "Error: No QASM file provided.\n";
        return 1;
    }

    //--------------------------------------------------
    // Load QASM file
    //--------------------------------------------------

    std::ifstream file(filename);

    if (!file)
    {
        std::cerr
            << "Cannot open file: "
            << filename
            << "\n";

        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    //--------------------------------------------------
    // Parse QASM
    //--------------------------------------------------

    qasm::QasmToCirc<> parser;

    auto circuit =
        parser.ParseAndTranslate(
            buffer.str());

    if (parser.Failed())
    {
        std::cerr
            << parser.GetErrorMessage()
            << "\n";

        return 1;
    }

    //--------------------------------------------------
    // Determine qubit count
    //--------------------------------------------------

    const size_t nrQubits =
        circuit->GetMaxQubitIndex() + 1;

    if (nrQubits == 0)
    {
        std::cerr << "Circuit has no qubits.\n";
        return 1;
    }

    //--------------------------------------------------
    // Convert to layers
    //--------------------------------------------------

    auto layers =
        circuit->ToMultipleQubitsLayers();

    const auto optCirc =
        circuit->LayersToCircuit(layers);

    size_t twoQubitGates = 0;
    for (const auto& op : optCirc->GetOperations())
        if (op->AffectedQubits().size() >= 2)
            ++twoQubitGates;

    std::cout << "Qubits:       " << nrQubits       << "\n";
    std::cout << "Layers:       " << layers.size()   << "\n";
    std::cout << "2Q gates:     " << twoQubitGates   << "\n";

    //--------------------------------------------------
    // ORIGINAL MAPPING
    //--------------------------------------------------

    Simulators::MPSDummySimulator origSim(nrQubits);
    origSim.setCostModel(costModel);
    origSim.SetMaxBondDimension(64);

    std::vector<long long int> identity(nrQubits);
    std::iota(identity.begin(), identity.end(), 0);
    origSim.SetInitialQubitsMap(identity);

    const double origPeakBond =
        applyGatesTrackingPeakBond(origSim, optCirc->GetOperations());

    const double origCost =
        origSim.getTotalSwappingCost();

    //--------------------------------------------------
    // OPTIMIZED MAPPING
    //--------------------------------------------------

    Simulators::MPSDummySimulator optSim(nrQubits);
    optSim.setCostModel(costModel);
    optSim.SetMaxBondDimension(64);

    auto t_route_0 = std::chrono::steady_clock::now();
    const auto optimalMap =
        optSim.ComputeOptimalQubitsMap(layers);
    auto t_route_1 = std::chrono::steady_clock::now();
    double routingOptimizationTimeMs =
        std::chrono::duration<double, std::milli>(t_route_1 - t_route_0).count();

    optSim.SetInitialQubitsMap(optimalMap);

    const double optPeakBond =
        applyGatesTrackingPeakBond(optSim, optCirc->GetOperations());

    const double optCost =
        optSim.getTotalSwappingCost();

    const double improvementPct =
        origCost > 0
            ? 100.0 *
                  (origCost - optCost) /
                  origCost
            : 0.0;

    std::cout << "\n";
    std::cout << "Original cost:    " << origCost    << "\n";
    std::cout << "Optimized cost:   " << optCost     << "\n";
    std::cout << "Peak BD original: " << origPeakBond  << "\n";
    std::cout << "Peak BD optimized:" << optPeakBond   << "\n";
    std::cout << "Improvement %:    " << improvementPct << "\n";
    std::cout << "Routing time:     " << routingOptimizationTimeMs << " ms\n";

    //--------------------------------------------------
    // Write legacy benchmark_results.csv
    //--------------------------------------------------

    std::ofstream csv(
        "benchmark_results.csv",
        std::ios::app);

    csv
        << filename       << ","
        << nrQubits       << ","
        << layers.size()  << ","
        << origCost       << ","
        << optCost        << ","
        << origPeakBond   << ","
        << optPeakBond    << ","
        << improvementPct
        << "\n";

    //--------------------------------------------------
    // REAL MPS EXECUTION & EXPERIMENTAL TIMING
    //--------------------------------------------------

    std::string costModelStr =
        (costModel == Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry) ? "svd" : "cubic";

    // Warmups
    for (int w = 0; w < numWarmups; ++w)
    {
        auto realSim = Simulators::SimulatorsFactory::CreateSimulator(
            Simulators::SimulatorType::kQCSim,
            Simulators::SimulationType::kMatrixProductState);

        if (realSim)
        {
            realSim->AllocateQubits(nrQubits);
            realSim->Initialize();
            realSim->Configure("matrix_product_state_max_bond_dimension", "64");

            if (auto qcSimState = std::dynamic_pointer_cast<Simulators::Private::QCSimState>(realSim))
            {
                qcSimState->setCostModel(costModel);
                qcSimState->SetUpcomingGates(circuit->GetOperations());
            }

            realSim->SetInitialQubitsMap(optimalMap);

            Circuits::OperationState state;
            state.AllocateBits(nrQubits);
            circuit->Execute(realSim, state);
        }
    }

    // Timed repetitions
    for (int rep = 1; rep <= numRepetitions; ++rep)
    {
        auto realSim = Simulators::SimulatorsFactory::CreateSimulator(
            Simulators::SimulatorType::kQCSim,
            Simulators::SimulationType::kMatrixProductState);

        if (!realSim) break;

        realSim->AllocateQubits(nrQubits);
        realSim->Initialize();
        realSim->Configure("matrix_product_state_max_bond_dimension", "64");

        if (auto qcSimState = std::dynamic_pointer_cast<Simulators::Private::QCSimState>(realSim))
        {
            qcSimState->setCostModel(costModel);
            qcSimState->SetUpcomingGates(circuit->GetOperations());
        }

        realSim->SetInitialQubitsMap(optimalMap);

        Simulators::MPSSvdCollector::getInstance().startSession(filename, costModelStr, rep);

        auto t_sim_0 = std::chrono::steady_clock::now();

        Circuits::OperationState state;
        state.AllocateBits(nrQubits);
        circuit->Execute(realSim, state);

        auto t_sim_1 = std::chrono::steady_clock::now();
        double totalSimulationTimeMs =
            std::chrono::duration<double, std::milli>(t_sim_1 - t_sim_0).count();

        Simulators::MPSSvdCollector::getInstance().stopSession();

        double totalSvdTimeMs =
            static_cast<double>(Simulators::MPSSvdCollector::getInstance().total_svd_time_ns) / 1.0e6;
        size_t realPeakBond = Simulators::MPSSvdCollector::getInstance().real_peak_bond_dim;
        uint64_t insertedSwaps = Simulators::MPSSvdCollector::getInstance().inserted_swap_count;

        // Write operation-level CSV trace
        Simulators::MPSSvdCollector::getInstance().writeOperationTraceCSV("mps_operation_trace.csv");

        // Write circuit-level benchmark_results_costmodel.csv
        bool writeHeader = false;
        {
            std::ifstream check("benchmark_results_costmodel.csv");
            if (!check.good()) writeHeader = true;
        }

        std::ofstream resCsv("benchmark_results_costmodel.csv", std::ios::app);
        if (resCsv.is_open())
        {
            if (writeHeader)
            {
                resCsv << "circuit,repetition,cost_model,qubits,layers,two_qubit_gates,"
                       << "max_bond_dim,predicted_dummy_cost,inserted_swap_count,"
                       << "real_peak_bond_dim,total_svd_time_ms,total_simulation_time_ms,routing_optimization_time_ms\n";
            }

            resCsv << filename << ","
                   << rep << ","
                   << costModelStr << ","
                   << nrQubits << ","
                   << layers.size() << ","
                   << twoQubitGates << ","
                   << 64 << ","
                   << optCost << ","
                   << insertedSwaps << ","
                   << realPeakBond << ","
                   << totalSvdTimeMs << ","
                   << totalSimulationTimeMs << ","
                   << routingOptimizationTimeMs << "\n";
        }
    }

    return 0;
}
