#include <iostream>
#include <fstream>
#include <sstream>
#include <numeric>
#include <algorithm>

#include "qasm/QasmCirc.h"

#include "Circuit/Circuit.h"
#include "Simulators/MPSDummySimulator.h"

double getPeakBondDimension(
    const Simulators::MPSDummySimulator& sim)
{
    const auto& dims = sim.getCurrentBondDimensions();

    if (dims.empty())
        return 0.0;

    return *std::max_element(
        dims.begin(),
        dims.end());
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr
            << "Usage: "
            << argv[0]
            << " circuit.qasm\n";

        return 1;
    }

    const std::string filename = argv[1];

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
    // GetMaxQubitIndex() scans all operations for the
    // highest qubit index; +1 gives the qubit count.
    // This is identical to what the manual loop did,
    // but uses the built-in Circuit API.
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

    // Diagnostic: count recognised 2-qubit gates so that
    // a missing rzz/custom gate registration is visible.
    size_t twoQubitGates = 0;
    for (const auto& op : optCirc->GetOperations())
        if (op->AffectedQubits().size() >= 2)
            ++twoQubitGates;

    std::cout << "Qubits:       " << nrQubits       << "\n";
    std::cout << "Layers:       " << layers.size()   << "\n";
    std::cout << "2Q gates:     " << twoQubitGates   << "\n";

    //--------------------------------------------------
    // ORIGINAL MAPPING
    // SetInitialQubitsMap is NOT redundant: it resets
    // bondCost[i] = 1, representing the initial product
    // state (bond dimension = 1 everywhere).  Without
    // it the simulator would start with bondCost values
    // set by SetMaxBondDimension, which are wrong for
    // the initial state.
    //--------------------------------------------------

    Simulators::MPSDummySimulator origSim(nrQubits);
    origSim.SetMaxBondDimension(64);

    std::vector<long long int> identity(nrQubits);
    std::iota(identity.begin(), identity.end(), 0);
    origSim.SetInitialQubitsMap(identity);

    origSim.ApplyGates(
        optCirc->GetOperations());

    const double origCost =
        origSim.getTotalSwappingCost();

    const double origPeakBond =
        getPeakBondDimension(origSim);

    //--------------------------------------------------
    // OPTIMIZED MAPPING
    //--------------------------------------------------

    Simulators::MPSDummySimulator optSim(nrQubits);
    optSim.SetMaxBondDimension(64);

    const auto optimalMap =
        optSim.ComputeOptimalQubitsMap(layers);

    optSim.SetInitialQubitsMap(optimalMap);

    optSim.ApplyGates(
        optCirc->GetOperations());

    const double optCost =
        optSim.getTotalSwappingCost();

    const double optPeakBond =
        getPeakBondDimension(optSim);

    //--------------------------------------------------
    // REPORT
    //--------------------------------------------------

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

    //--------------------------------------------------
    // CSV output  (ofstream closes automatically on
    // destruction, no explicit close() needed)
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

    return 0;
}