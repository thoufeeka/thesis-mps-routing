# Initial Qubit Mapping - Progress Report

The existing baseline is **BuildChain** - a greedy pair-merging heuristic that orders qubit interactions by first occurrence and constructs a linear placement. All new strategies are evaluated against this baseline using Maestro's `CentralBondCubic` cost model (bond dim = 64) across 12 benchmark circuits:

- **QFT** (10–40 qubits) - dense, hierarchical butterfly pattern  
- **QAOA** (10–40 qubits) - sparse, repetitive two-qubit layer pattern  
- **Random** (10–40 qubits) - brick-wall nearest-neighbour structure

---

## 2. Strategies Investigated

| Strategy | Approach |
|---|---|
| **BuildChain** *(baseline)* | Greedy pair-merging in gate-occurrence order |
| **CommunityMapping** | Newman-Moore modularity community detection; places communities contiguously |
| **TemporalCommunityMapping** | Community detection with exponential time-decay weighting (α = 1, 2, 4) |
| **QMAP Heuristic** | A\*-based routing (MQT QMAP, TU Munich); minimises total SWAP *count* on 1D chain |
| **BondAwarePlacement (SA)** | Simulated Annealing; warm-starts from BuildChain, directly minimises Maestro's bond-dimension-weighted cost |

---

## 3. Consolidated Results

**Predicted MPS cost** (lower = better; BuildChain = reference; **bold** = best per row)

| Circuit | BuildChain | CommunityMap | TemporalComm (best α) | QMAP-H | BondAware SA |
|---|---|---|---|---|---|
| qaoa\_10 (10q) | **237** | 288 | 257 | 275 | 238 |
| qaoa\_20 (20q) | 1159 | 1372 | 1278 | 1300 | **1109** ✓ |
| qaoa\_30 (30q) | 294 | 298 | **283** ✓ | 501 | 303 |
| qaoa\_40 (40q) | 456 | 509 | **386** ✓ | 679 | 566 |
| qft\_10 (10q) | **203** | 275 | 259 | 228 | 210 |
| qft\_20 (20q) | **987** | 1330 | 1302 | 1168 | 1060 |
| qft\_30 (30q) | **1682** | 2200 | 2200 | 2032 | 2532 |
| qft\_40 (40q) | **3042** | 4030 | 4030 | 3707 | 4762 |
| random\_10 (10q) | **304** | 379 | 315 | 309 | 325 |
| random\_20 (20q) | **1125** | 1266 | 1178 | 1154 | 1145 |
| random\_30 (30q) | **290** | 290 | 290 | 290 | 290 |
| random\_40 (40q) | **390** | 390 | 390 | 390 | 390 |

> ✓ = beats BuildChain. TemporalComm shows result for best α (α = 4 on QAOA; α = 1 on QFT).

**Win/Tie/Loss summary vs BuildChain**

| Strategy | Wins | Ties | Losses | Avg Δ% vs BC |
|---|---|---|---|---|
| CommunityMapping | 0 | 2 | 10 | −28.5% |
| TemporalComm (best α) | **2** | 2 | 8 | −12.4% |
| QMAP Heuristic | 0 | 2 | 10 | −18.7% |
| BondAwarePlacement SA | **1** | 2 | 9 | −9.1% |

---

## 4. Key Findings

### 4.1 TemporalCommunityMapping improves sparse (QAOA) circuits

Exponential time-decay weighting (α = 4) reduces MPS cost by **+3.7% on qaoa\_30** and **+15.4% on qaoa\_40** compared to BuildChain. The intuition is that QAOA's early layers contain the most structurally informative gates for determining a good 1D ordering. No improvement was observed on QFT or random circuits.

### 4.2 Minimum SWAP count does not minimise MPS cost

QMAP (MQT tool, TU Munich) uses A\*-search to find the minimum number of SWAP gates on a 1D chain. Despite achieving fewer SWAPs than BuildChain, QMAP consistently produces higher MPS simulation cost (0 wins, avg −18.7% vs BuildChain).

This suggests that bond-dimension weighting changes the routing objective in a meaningful way: a SWAP at the chain centre costs O(χ³) more than a SWAP at the chain edge, but QMAP treats all SWAPs uniformly. It indicates that an MPS-aware routing objective may be needed to improve on BuildChain.

### 4.3 BondAwarePlacement SA - initial results

The SA directly optimises Maestro's cost function, achieving **+4.3% on qaoa\_20** and near-ties on random circuits. Regressions on QFT and large QAOA circuits suggest that the random 2-qubit swap neighbourhood is insufficient to escape BuildChain's greedy local optimum on dense circuits. Improved neighbourhood operators and a larger evaluation window are planned as follow-up.

---

## 5. Implementation

All strategies are implemented in `maestro/Simulators/InitialMapping.h` as non-intrusive additions - no existing Maestro simulator code was modified. A unified benchmarking framework (`benchmarks/mapping_comparison_benchmark.cpp`) evaluates all strategies on any QASM circuit and outputs CSV results for analysis.
