#!/usr/bin/env python3
"""Probe QMAP API to understand MappingResults structure."""
from mqt.qmap import sc
from mqt.core import QuantumComputation
from qiskit import QuantumCircuit
import sys

print("=== QMAP sc module ===")
print("Method values:", [x for x in dir(sc.Method) if not x.startswith('_')])
print("InitialLayout values:", [x for x in dir(sc.InitialLayout) if not x.startswith('_')])

N = 5
arch = sc.Architecture(N, {(i, i+1) for i in range(N-1)})

qc = QuantumCircuit(N)
qc.cx(0, 1); qc.cx(1, 2); qc.cx(2, 3); qc.cx(3, 4); qc.cx(0, 4); qc.cx(1, 3)
print("\nInput circuit (Qiskit):")
print(qc)

# Convert to mqt.core QuantumComputation (required by sc.map_)
qasm2_str = qc.qasm()
mqt_qc = QuantumComputation.from_qasm_str(qasm2_str)

config = sc.Configuration()
config.method = sc.Method.heuristic
config.initial_layout = sc.InitialLayout.dynamic

mapped_qc, results = sc.map_(mqt_qc, arch, config)

print("\n=== MappingResults ===")
for attr in dir(results):
    if attr.startswith('_'):
        continue
    val = getattr(results, attr)
    if callable(val) and not isinstance(val, (int, float, str, bool)):
        print(f"  {attr}: <method>")
    else:
        print(f"  {attr}: {val!r}")

print("\n=== Mapped circuit ===")
mc = results.mapped_circuit
print(type(mc))
print(mc)

# Try to get the initial layout
print("\n=== Layout extraction ===")
if hasattr(mc, '_layout') and mc._layout is not None:
    print("Has _layout:", mc._layout)
    il = mc._layout.initial_layout
    print("initial_layout:", il)
    # Convert to permutation: il[virtual] = physical
    perm = {}
    for virt, phys in il.get_virtual_bits().items():
        perm[virt.index] = phys
    print("Permutation (logical -> physical):", perm)
elif hasattr(mc, 'layout') and mc.layout is not None:
    print("Has layout:", mc.layout)
    il = mc.layout.initial_layout
    print("initial_layout:", il)
    perm = {}
    for virt, phys in il.get_virtual_bits().items():
        perm[virt.index] = phys
    print("Permutation (logical -> physical):", perm)
else:
    print("No layout found on mapped circuit")
    print("mc attrs:", [x for x in dir(mc) if 'layout' in x.lower()])
