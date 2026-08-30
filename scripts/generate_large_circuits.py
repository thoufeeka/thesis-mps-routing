#!/usr/bin/env python3
"""
Generates QASM 2.0 benchmark circuits at 30 and 40 qubits.
Outputs qft_N.qasm, qaoa_N.qasm, and random_N.qasm into benchmarks/.
"""

import math
import random
import os

OUTDIR = os.path.join(os.path.dirname(__file__), "..", "benchmarks")

# QFT

def qft_qasm(N: int) -> str:
    """Standard QFT on N qubits in QASM 2.0 (matches existing qft_10/20 style)."""
    lines = [
        "OPENQASM 2.0;",
        'include "qelib1.inc";',
        f"qreg q[{N}];",
        f"creg meas[{N}];",
    ]
    # QFT iterates from qubit N-1 down to 0
    for i in range(N - 1, -1, -1):
        # Hadamard via u(pi/2,0,pi)
        lines.append(f"u(pi/2,0,pi) q[{i}];")
        # Controlled-phase gates
        k = 2
        for j in range(i - 1, -1, -1):
            angle = f"pi/{2**k}" if k <= 10 else f"pi/{2**k}"
            # Decompose cp(angle, ctrl, tgt) as: cx ctrl,tgt; u(0,0,-angle) tgt; cx ctrl,tgt; u(0,0,angle) tgt
            lines.append(f"u(0,0,{angle}) q[{i}];")
            lines.append(f"cx q[{i}],q[{j}];")
            lines.append(f"u(0,0,-{angle}) q[{j}];")
            lines.append(f"cx q[{i}],q[{j}];")
            lines.append(f"u(0,0,{angle}) q[{j}];")
            k += 1
        # trailing single-qubit after controlled phases (matches existing style)
        if i > 0:
            k2 = 2
            for j2 in range(i - 1, max(i - 4, -1), -1):
                angle2 = f"pi/{2**k2}"
                lines.append(f"u(0,0,{angle2}) q[{j2}];")
                k2 += 1

    return "\n".join(lines) + "\n"


# QAOA on random 3-regular graph, p=1

def random_3regular(N: int, seed: int) -> list[tuple[int, int]]:
    """Generate edges of a random 3-regular graph on N nodes (N must be even)."""
    rng = random.Random(seed)
    # Stubs: each node has 3 stubs
    stubs = []
    for v in range(N):
        stubs.extend([v] * 3)
    rng.shuffle(stubs)

    edges = []
    seen = set()
    i = 0
    max_attempts = 10000
    attempt = 0
    while i < len(stubs) - 1 and attempt < max_attempts:
        attempt += 1
        u, v = stubs[i], stubs[i + 1]
        if u != v and (u, v) not in seen and (v, u) not in seen:
            edges.append((u, v))
            seen.add((u, v))
            i += 2
        else:
            # Try to swap with a later pair to fix the conflict
            swapped = False
            for k in range(i + 2, len(stubs) - 1, 2):
                u2, v2 = stubs[k], stubs[k + 1]
                # Try swap: (u,v2) and (u2,v)
                if (u != v2 and (u, v2) not in seen and (v2, u) not in seen and
                        u2 != v and (u2, v) not in seen and (v, u2) not in seen):
                    stubs[i + 1], stubs[k + 1] = stubs[k + 1], stubs[i + 1]
                    swapped = True
                    break
            if not swapped:
                i += 2  # skip this unresolvable pair

    return edges


def qaoa_qasm(N: int, seed: int = 42) -> str:
    """QAOA p=1 on a random 3-regular graph, matching existing qaoa style."""
    if N % 2 != 0:
        N += 1  # ensure even for 3-regular
    edges = random_3regular(N, seed)
    gamma = 10.410545461930019  # same value used in existing benchmarks

    lines = [
        "OPENQASM 2.0;",
        'include "qelib1.inc";',
        f"qreg q[{N}];",
    ]

    # Layer 1: Hadamard on all qubits (via u(pi/2,0,pi))
    for i in range(N):
        lines.append(f"u(pi/2,0,pi) q[{i}];")

    # Problem unitary: for each edge (i,j), apply ZZ rotation
    # ZZ(gamma) = cx i,j; rz(gamma) j; cx i,j
    for (u, v) in edges:
        lines.append(f"cx q[{u}],q[{v}];")
        lines.append(f"u(0,0,{gamma}) q[{v}];")
        lines.append(f"cx q[{u}],q[{v}];")

    # Mixer unitary: rx(2*beta) on all qubits (beta=pi/4 is typical)
    beta = math.pi / 4
    for i in range(N):
        lines.append(f"rx({2*beta:.6f}) q[{i}];")

    return "\n".join(lines) + "\n"


# Random brick-wall circuit

def random_circuit_qasm(N: int, n_layers: int = 20, seed: int = 42) -> str:
    """
    Brick-wall random circuit: alternating even/odd layers of CX gates
    with single-qubit rotations between layers.
    Matches the structure of random_10.qasm and random_20.qasm.
    """
    rng = random.Random(seed)

    def rand_angle():
        return round(rng.uniform(0.1, math.pi), 4)

    def rand_1q():
        return rng.choice(["rx", "ry", "rz"])

    lines = [
        "OPENQASM 2.0;",
        'include "qelib1.inc";',
        f"qreg q[{N}];",
        f"creg c[{N}];",
        "",
    ]

    # Initial single-qubit layer
    for i in range(N):
        lines.append(f"{rand_1q()}({rand_angle()}) q[{i}];")

    for layer in range(n_layers):
        # Even layer: pairs (0,1), (2,3), ...
        # Odd layer:  pairs (1,2), (3,4), ...
        offset = layer % 2
        pairs = [(i, i + 1) for i in range(offset, N - 1, 2)]

        for (u, v) in pairs:
            lines.append(f"cx q[{u}],q[{v}];")

        # Single-qubit rotations after each CX layer
        for i in range(N):
            lines.append(f"{rand_1q()}({rand_angle()}) q[{i}];")

    return "\n".join(lines) + "\n"


# Main

def write(filename: str, content: str):
    path = os.path.join(OUTDIR, filename)
    with open(path, "w") as f:
        f.write(content)
    lines = content.count("\n")
    twoq = content.count("cx ")
    print(f"  Written {filename:25s}  ({lines} lines, {twoq} CX gates)")


if __name__ == "__main__":
    os.makedirs(OUTDIR, exist_ok=True)

    print("\n=== Generating 30-qubit circuits ===")
    write("qft_30.qasm",    qft_qasm(30))
    write("qaoa_30.qasm",   qaoa_qasm(30, seed=42))
    write("random_30.qasm", random_circuit_qasm(30, n_layers=20, seed=42))

    print("\n=== Generating 40-qubit circuits ===")
    write("qft_40.qasm",    qft_qasm(40))
    write("qaoa_40.qasm",   qaoa_qasm(40, seed=42))
    write("random_40.qasm", random_circuit_qasm(40, n_layers=20, seed=42))

    print("\nDone. Files written to benchmarks/")
