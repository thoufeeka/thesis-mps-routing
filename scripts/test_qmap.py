#!/usr/bin/env python3
import sys
from mqt.core import load
from mqt.qmap import sc

def test_circuit(qasm_file):
    circ = load(qasm_file)
    n_qubits = circ.num_qubits
    print(f"File: {qasm_file}, Qubits: {n_qubits}, Gates: {circ.num_ops}")
    
    # 1D linear chain
    coupling = {(i, i+1) for i in range(n_qubits - 1)}
    arch = sc.Architecture(n_qubits, coupling)
    
    config = sc.Configuration()
    config.method = sc.Method.heuristic
    config.initial_layout = sc.InitialLayout.dynamic
    
    mapped_qc, results = sc.map_(circ, arch, config)
    print("Heuristic Swaps:", results.output.swaps)
    print("Initial layout (mapping):", dict(mapped_qc.initial_layout))
    print("Final layout:", dict(mapped_qc.output_permutation))

if __name__ == "__main__":
    test_circuit("benchmarks/qft_10.qasm")
