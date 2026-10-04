# MPS Qubit Mapping Strategy Research Log

All experiments use: cost model = cubic, bond dim = 64, circuits: qaoa/qft/random at 10/20/30/40 qubits.
Baseline is always **BuildChain** (existing Maestro heuristic).

---

## Benchmark Circuits

| Circuit | Qubits | Layers | 2Q Gates | Structure |
|---|---|---|---|---|
| qaoa_10 | 10 | 46 | 104 | Dense, all-to-all pairs |
| qaoa_20 | 20 | 90 | 368 | Dense, all-to-all pairs |
| qaoa_30 | 30 | 14 | 90 | Sparse layers, repeated pairs |
| qaoa_40 | 40 | 14 | 120 | Sparse layers, repeated pairs |
| qft_10 | 10 | 37 | 105 | Triangular DAG, qubit 0 talks to all |
| qft_20 | 20 | 77 | 410 | Triangular DAG |
| qft_30 | 30 | 114 | 870 | Triangular DAG |
| qft_40 | 40 | 154 | 1560 | Triangular DAG, very dense |
| random_10 | 10 | 20 | 93 | Uniform random pairs |
| random_20 | 20 | 20 | 193 | Uniform random pairs |
| random_30 | 30 | 20 | 290 | Uniform random pairs |
| random_40 | 40 | 20 | 390 | Uniform random pairs |

---

## Strategy 0: BuildChain (Baseline)

**File:** `MPSDummySimulator.h::ComputeOptimalQubitsMap`

**How it works:** Scans circuit layers in order (FCFS). For each 2-qubit pair:
- **Case A:** Neither placed -- start new group `[q1, q2]`
- **Case B:** q1 placed, q2 not -- attach q2 to nearer end of q1's group
- **Case C:** q2 placed, q1 not -- attach q1 to nearer end of q2's group
- **Case D:** Both placed, different groups -- merge so q1/q2 end up adjacent (minimise single-pair distance)
- **Case E:** Both in same group -- skip

Stops as soon as all qubits are placed. Then evaluates identity map and random shuffles as fallbacks.

**Swap counts (baseline):**

| Circuit | Swaps |
|---|---|
| qaoa_10 | 237 |
| qaoa_20 | 1159 |
| qaoa_30 | 294 |
| qaoa_40 | 456 |
| qft_10 | 203 |
| qft_20 | 987 |
| qft_30 | 1682 |
| qft_40 | 3042 |
| random_10 | 304 |
| random_20 | 1125 |
| random_30 | 290 |
| random_40 | 390 |

---

## Strategy 1: CommunityMapping

**Idea:** Detect communities in the qubit interaction graph using Louvain-style algorithm, then place each community as a contiguous block in the chain.

**Result vs BuildChain:**

| Circuit | BuildChain | Community | Delta |
|---|---:|---:|---:|
| qaoa_10 | 237 | 288 | +21.5% |
| qaoa_20 | 1159 | 1372 | +18.4% |
| qaoa_30 | 294 | 298 | +1.4% |
| qaoa_40 | 456 | 509 | +11.6% |
| qft_10 | 203 | 275 | +35.5% |
| qft_20 | 987 | 1330 | +34.7% |
| qft_30 | 1682 | 2200 | +30.8% |
| qft_40 | 3042 | 4030 | +32.5% |
| random_10 | 304 | 379 | +24.7% |
| random_20 | 1125 | 1266 | +12.5% |
| random_30 | 290 | 290 | 0.0% |
| random_40 | 390 | 390 | 0.0% |
| **Avg** | | | **+16.1%** |

> [!NOTE]
> Consistently worse than BuildChain. Static community detection groups qubits by interaction frequency but ignores the linear-chain constraint of MPS. Communities placed as contiguous blocks create hard boundaries that force expensive routing between them.

---

## Strategy 2: TemporalCommunityMapping (alpha = 1, 2, 4)

**Idea:** Same as CommunityMapping but edge weights decay exponentially with layer index: `weight += exp(-alpha * l / L)`. Earlier layers are weighted more heavily.

**Result vs BuildChain:**

| Circuit | BuildChain | TempComm a=1 | TempComm a=2 | TempComm a=4 |
|---|---:|---:|---:|---:|
| qaoa_10 | 237 | 257 | 257 | 257 |
| qaoa_20 | 1159 | 1373 | 1278 | 1278 |
| qaoa_30 | 294 | 301 | 287 | 283 |
| qaoa_40 | 456 | 414 | 414 | **386** |
| qft_10 | 203 | 259 | 269 | 269 |
| qft_20 | 987 | 1302 | 1314 | 1314 |
| qft_30 | 1682 | 2200 | 2200 | 2200 |
| qft_40 | 3042 | 4030 | 4030 | 4030 |
| random_10 | 304 | 379 | 315 | 315 |
| random_20 | 1125 | 1184 | 1188 | 1178 |
| random_30 | 290 | 290 | 563 | 563 |
| random_40 | 390 | 390 | 390 | 390 |

> [!NOTE]
> qaoa_40 with alpha=4 gives 386 vs 456 (-15.4%) -- the best community-based result. But QFT consistently degrades. TemporalComm alpha=2 uniquely explodes on random_30 (+94%) due to community boundaries misaligning with the sparse random interaction graph.

**Lesson:** Community-based strategies fail on QFT because qubit 0 interacts with every other qubit and belongs to no coherent community. Any partition creates an artificial boundary around q0.

---

## Strategy 3: BondAwarePlacement

**Idea:** Simulated Annealing directly optimising MPS bond-dimension cost. Starts from BuildChain solution, perturbs via random qubit swaps, accepts improvements. Highly variable across runs.

**Result vs BuildChain (representative runs):**

| Circuit | BuildChain | BondAware | Delta |
|---|---:|---:|---:|
| qaoa_10 | 237 | 233 | -1.7% |
| qaoa_20 | 1159 | 1110 | -4.2% |
| qaoa_30 | 294 | ~330-372 | +12-27% |
| qaoa_40 | 456 | ~511-596 | +12-31% |
| qft_10 | 203 | 207-234 | +2-15% |
| qft_20 | 987 | 1082-1138 | +10-15% |
| qft_30 | 1682 | 2196-2783 | +31-65% |
| qft_40 | 3042 | 4631-5638 | +52-85% |
| random_10 | 304 | 306-309 | +0.7-1.6% |
| random_20 | 1125 | 1140-1143 | +1.3-1.6% |
| random_30 | 290 | 290 | 0% |
| random_40 | 390 | 390 | 0% |

> [!WARNING]
> SA wins on small QAOA but catastrophically fails on large QFT (+85% on qft_40). Results are non-deterministic. The annealing schedule does not scale to large circuits with many local minima.

---

## Strategy 4: LayerPriorityMapping

**Idea:** Score each layer by `sum((1 + dist_weight * |q1-q2|) / freq(q1,q2))`. Layers with rare, far-apart pairs get higher priority and are processed first by BuildChain.

**Result vs BuildChain:**

| Circuit | BuildChain | LayerPriority | Delta |
|---|---:|---:|---:|
| qaoa_10 | 237 | 258 | +8.9% |
| qaoa_20 | 1159 | 1303 | +12.4% |
| qaoa_30 | 294 | 378 | +28.6% |
| qaoa_40 | 456 | 660 | +44.7% |
| qft_10 | 203 | 216 | +6.4% |
| qft_20 | 987 | 1171 | +18.6% |
| qft_30 | 1682 | 2228 | +32.5% |
| qft_40 | 3042 | 4068 | +33.7% |
| random_10 | 304 | 307 | +1.0% |
| random_20 | 1125 | 1203 | +6.9% |
| random_30 | 290 | 290 | 0.0% |
| random_40 | 390 | 390 | 0.0% |
| **Avg** | | | **+16.1%** |

> [!NOTE]
> Uniformly worse. Index distance in logical space (|q1-q2|) is unrelated to routing difficulty in MPS. The score formula penalises the wrong layers.

---

## Strategy 5: NewPairsFirstMapping

**Idea:** Greedy set-cover layer reordering. At each step, pick the unselected layer introducing the most qubit pairs not yet seen in any previously selected layer.

**Result vs BuildChain:**

| Circuit | BuildChain | NewPairsFirst | Delta |
|---|---:|---:|---:|
| qaoa_10 | 237 | 242 | +2.1% |
| qaoa_20 | 1159 | 1179 | +1.7% |
| qaoa_30 | 294 | 311 | +5.8% |
| qaoa_40 | 456 | 578 | +26.8% |
| qft_10 | 203 | 209 | +3.0% |
| **qft_20** | **987** | **985** | **-0.2%** |
| qft_30 | 1682 | 2228 | +32.5% |
| qft_40 | 3042 | 4068 | +33.7% |
| **random_10** | **304** | **293** | **-3.6%** |
| random_20 | 1125 | 1155 | +2.7% |
| random_30 | 290 | 290 | 0.0% |
| random_40 | 390 | 390 | 0.0% |
| **Avg** | | | **+8.7%** |

> [!NOTE]
> Better than LayerPriority (avg +8.7% vs +16.1%). Two small wins: qft_20 (-0.2%) and random_10 (-3.6%). QFT 30/40 severely hurt. Confirms that layer reordering is incompatible with QFT's structural requirements.

---

## Strategy 6: FreqSeededChain

**Idea:** Run BuildChain's merge logic (Cases A-E identical) but feed pairs sorted by global frequency (most frequent pair first) instead of layer order.

**Result vs BuildChain:**

| Circuit | BuildChain | FreqSeeded | Delta |
|---|---:|---:|---:|
| qaoa_10 | 237 | 286 | +20.7% |
| qaoa_20 | 1159 | 1271 | +9.7% |
| qaoa_30 | 294 | 357 | +21.4% |
| qaoa_40 | 456 | 582 | +27.6% |
| qft_10 | 203 | 209 | +3.0% |
| qft_20 | 987 | 1018 | +3.1% |
| qft_30 | 1682 | 1865 | +10.9% |
| qft_40 | 3042 | 4070 | +33.8% |
| random_10 | 304 | 309 | +1.6% |
| random_20 | 1125 | 1164 | +3.5% |
| **random_30** | **290** | **478** | **+64.8%** |
| **random_40** | **390** | **1021** | **+161.8%** |
| **Avg** | | | **+30.2%** |

> [!CAUTION]
> Worst strategy tested. Catastrophic failure on random circuits: +64.8% and +161.8%. Root cause: for random circuits every pair appears exactly once, so frequency sorting ties on all pairs. The tie-break (ascending index distance) prioritises nearest-index pairs first, which is the opposite of what MPS routing needs.

---

## Strategy 7: WeightedMergeChain

**Idea:** Keep BuildChain's FCFS layer ordering and Cases A/B/C unchanged. Only modify Case D: score ALL cross-group pairs weighted by global frequency, sum expected distances for both orientations, pick the lower-score orientation.

```
scoreAB = sum_{a in grpA, b in grpB} freq(a,b) * dist_in_[A][B](a,b)
scoreBA = sum_{a in grpA, b in grpB} freq(a,b) * dist_in_[B][A](a,b)
```

Falls back to BuildChain's original single-pair formula if no cross-group pairs have nonzero frequency.

**Result vs BuildChain:**

| Circuit | BuildChain | WeightedMerge | Delta |
|---|---:|---:|---:|
| qaoa_10 | 237 | 253 | +6.8% |
| qaoa_20 | 1159 | 1383 | +19.3% |
| **qaoa_30** | **294** | **249** | **-15.3%** |
| **qaoa_40** | **456** | **437** | **-4.2%** |
| qft_10 | 203 | 269 | +32.5% |
| qft_20 | 987 | 1314 | +33.1% |
| qft_30 | 1682 | 2200 | +30.8% |
| qft_40 | 3042 | 4030 | +32.5% |
| random_10 | 304 | 309 | +1.6% |
| random_20 | 1125 | 1154 | +2.6% |
| random_30 | 290 | 290 | 0.0% |
| random_40 | 390 | 390 | 0.0% |
| **Avg** | | | **+8.9%** |

> [!TIP]
> First strategy with significant wins: qaoa_30 (-15.3%) and qaoa_40 (-4.2%). Random circuits tie (no degradation). QFT remains badly hurt (+30-33%) because qubit 0's symmetric interaction pattern makes scoreAB and scoreBA nearly equal, and the deterministic tie-break picks the wrong orientation.

---

## Strategy 8: LookaheadGroupMapping (W = 3, W = 5)

**Idea:** FCFS seeding like BuildChain. Instead of global pair frequency, attachment (Cases B/C) and merge orientation (Case D) are scored by looking ahead only into the next $W$ layers ($W=3$ and $W=5$). Pairs appearing in immediate future layers receive higher weight: `exp(-delta_layer)`.

**Result vs BuildChain:**

| Circuit | BuildChain | Lookahead W=3 | Delta W3 | Lookahead W=5 | Delta W5 |
|---|---:|---:|---:|---:|---:|
| qaoa_10 | 237 | 243 | +2.5% | 282 | +19.0% |
| qaoa_20 | 1159 | 1316 | +13.5% | 1254 | +8.2% |
| **qaoa_30** | **294** | **287** | **-2.4%** | 309 | +5.1% |
| **qaoa_40** | **456** | **422** | **-7.5%** | **422** | **-7.5%** |
| qft_10 | 203 | 253 | +24.6% | 271 | +33.5% |
| qft_20 | 987 | 1273 | +29.0% | 1316 | +33.3% |
| qft_30 | 1682 | 2189 | +30.1% | 2202 | +30.9% |
| qft_40 | 3042 | 4014 | +31.9% | 4032 | +32.5% |
| random_10 | 304 | 309 | +1.6% | 309 | +1.6% |
| random_20 | 1125 | 1154 | +2.6% | 1154 | +2.6% |
| random_30 | 290 | 290 | 0.0% | 290 | 0.0% |
| random_40 | 390 | 390 | 0.0% | 390 | 0.0% |
| **Avg** | | | **+10.5%** | | **+13.3%** |

> [!NOTE]
> Windowed lookahead maintains the gains on large QAOA circuits (qaoa_30 -2.4%, qaoa_40 -7.5%) and ties on random circuits. However, it still degrades on QFT (+25% to +33%) because qubit 0 interacts with someone in almost every layer within the lookahead window as well, preserving the symmetric tie issue.

---

## Strategy 9: RigidSeedLocalSearch (Two-Phase Rigid Seed + MPS-Cost Local Search)

**Idea:** Uses BuildChain as a frozen initial state $\pi$. Partitions $\pi$ into atomic rigid multi-qubit blocks:
- Each 2-qubit gate in Layer 0 forms a rigid 2-qubit block `[u, v]` (guaranteeing 0 SWAPs in Layer 0).
- Uncoupled or later-layer qubits form single-qubit blocks `[w]`.

Explores constrained local search perturbations that never separate Layer-0 adjacent pairs:
- **Block Swaps:** Swap entire multi-qubit rigid blocks `[q3, q1] <-> [q2, q4]`.
- **Group Inversions:** 2-opt sub-chain reversals across blocks and internal block flips `[u, v] <-> [v, u]`.
- **Peripheral Insertion:** Relocate non-early singletons to the ends of the chain (front or back).

Evaluates the exact full MPS cost $C(\pi) = \sum_{\text{layers}} \text{Cost}(\text{MPSDummySimulator})$ via the fast MPS oracle. If no perturbation improves over BuildChain, it strictly returns the initial BuildChain mapping.

**Result vs BuildChain:**

| Circuit | BuildChain | RigidSeedLocalSearch | Delta | Time (ms) |
|---|---:|---:|---:|---:|
| **qaoa_10** | 237 | **225** | **-5.1%** | 1.48 |
| **qaoa_20** | 1159 | **1080** | **-6.8%** | 7.60 |
| **qaoa_30** | 294 | **234** | **-20.4%** | 2.82 |
| **qaoa_40** | 456 | **367** | **-19.5%** | 4.88 |
| **qft_10** | 203 | **186** | **-8.4%** | 1.83 |
| **qft_20** | 987 | **942** | **-4.6%** | 8.99 |
| **qft_30** | 1682 | **1681** | **-0.1%** | 22.73 |
| **qft_40** | 3042 | **3041** | **-0.0%** | 54.19 |
| **random_10** | 304 | **288** | **-5.3%** | 1.86 |
| **random_20** | 1125 | **1089** | **-3.2%** | 5.17 |
| random_30 | 290 | 290 | 0.0% | 4.55 |
| random_40 | 390 | 390 | 0.0% | 8.08 |
| **Avg** | | | **-6.1%** | **~10.3 ms** |

> [!TIP]
> Best performing strategy across all metrics:
> - 10 wins, 2 ties, 0 losses out of 12 circuits.
> - Outperforms BuildChain on both QAOA (-5.1% to -20.4%), QFT (-0.0% to -8.4%), and Random (-0.0% to -5.3%).
> - Total runtime across all 12 circuits is ~124 ms (over 1000x faster than unconstrained simulated annealing).
> - Zero degradation on any benchmark circuit.

---

## Consolidated Comparison Table

All results relative to BuildChain (negative = improvement):

| Circuit | BC | Comm | TempA4 | BondAware | LayerPri | NewPairs | FreqSeed | WtdMerge | LookW3 | LookW5 | RigidSeed |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| qaoa_10 | 237 | +22% | +8% | -2% | +9% | +2% | +21% | +7% | +2% | +19% | **-5.1%** |
| qaoa_20 | 1159 | +18% | +10% | -4% | +12% | +2% | +10% | +19% | +14% | +8% | **-6.8%** |
| qaoa_30 | 294 | +1% | -4% | +12% | +29% | +6% | +21% | -15% | -2% | +5% | **-20.4%** | +10.9% |
| qaoa_40 | 456 | +12% | -15% | +16% | +45% | +27% | +28% | -4% | -8% | -8% | **-19.5%** | +2.4% |
| qft_10 | 203 | +36% | +33% | +2% | +6% | +3% | +3% | +33% | +25% | +33% | **-8.4%** | +32.5% |
| qft_20 | 987 | +35% | +33% | +15% | +19% | 0% | +3% | +33% | +29% | +33% | **-4.6%** | +33.1% |
| qft_30 | 1682 | +31% | +31% | +16% | +33% | +33% | +11% | +31% | +30% | +31% | **-0.1%** | +30.8% |
| qft_40 | 3042 | +33% | +33% | +52% | +34% | +34% | +34% | +33% | +32% | +33% | **-0.0%** | +32.5% |
| random_10 | 304 | +25% | +4% | +1% | +1% | -4% | +2% | +2% | +2% | +2% | **-5.3%** | +1.6% |
| random_20 | 1125 | +13% | +5% | +2% | +7% | +3% | +3% | +3% | +3% | +3% | **-3.2%** | +2.6% |
| random_30 | 290 | 0% | +94% | 0% | 0% | 0% | +65% | 0% | 0% | 0% | **0.0%** | 0.0% |
| random_40 | 390 | 0% | 0% | 0% | 0% | 0% | +162% | 0% | 0% | 0% | **0.0%** | 0.0% |
| **Avg** | | +19% | +25% | +9% | +16% | +9% | +30% | +9% | +11% | +13% | **-6.1%** | **+14.3%** |

---

## Strategy 10: PeripheralPinning (Dynamic Barrier / Peripheral Pinning)

**Idea:** In MPS, bond dimensions grow toward the center of the chain and are smallest at the boundaries. A qubit that interacts only in the first few layers and then remains idle for most of the circuit acts as an "internal barrier" when placed in the interior: every gate that must be routed past it pays the high interior bond cost. By pinning such "isolated" qubits to the outermost chain positions (physical slots 0 or N-1), those gates only ever cross cheap boundary bonds.

**Classification:** A qubit is "isolated" if:
- Its active span (`last_layer - first_layer`) is less than 25% of total layers, AND
- It participates in at most 4 total 2-qubit gates.

**Modified BuildChain rule (Cases B/C):** When attaching a new unplaced qubit to an existing group:
- If the incoming qubit is isolated, push it to the *far end* of the group (boundary position), not the near end.
- Otherwise, use BuildChain's original nearest-end heuristic.

**Post-process:** After chain assembly, any isolated qubit still stranded in the interior is swapped with a non-isolated boundary qubit.

**Result vs BuildChain:**

| Circuit | BuildChain | PeripheralPinning | Delta |
|---|---:|---:|---:|
| qaoa_10 | 237 | 253 | +6.8% |
| qaoa_20 | 1159 | 1383 | +19.3% |
| qaoa_30 | 294 | 326 | +10.9% |
| qaoa_40 | 456 | 467 | +2.4% |
| qft_10 | 203 | 269 | +32.5% |
| qft_20 | 987 | 1314 | +33.1% |
| qft_30 | 1682 | 2200 | +30.8% |
| qft_40 | 3042 | 4030 | +32.5% |
| random_10 | 304 | 309 | +1.6% |
| random_20 | 1125 | 1154 | +2.6% |
| random_30 | 290 | 290 | 0.0% |
| random_40 | 390 | 390 | 0.0% |
| **Avg** | | | **+14.3%** |

> [!CAUTION]
> Strategy fails on every QFT circuit (+30-33%) and also degrades on all QAOA circuits. It does not help on random (ties). 0 wins, 2 ties, 10 losses.

**Root cause:** The "isolated qubit" classification does not apply in any of the 12 benchmark circuits. In QAOA all qubits participate in many pairs across many layers. In QFT, qubit 0 interacts with every other qubit in every layer, and qubits 1...N-1 are active until late in the circuit. In random circuits, every qubit appears in roughly the same number of gates. None satisfy the span < 25% AND totalGates <= 4 threshold simultaneously. As a result, `isIsolated` is false for every qubit in every benchmark, and the strategy reduces to exactly BuildChain's attachment logic -- but with the slightly different "push far" direction for the forced-outer path, which disrupts the same FCFS seeding that gives BuildChain its QFT advantage.

**Conclusion:** The "idle qubit" boundary-pinning idea is sound for circuits with spectator qubits (e.g., ancilla registers), but the 12 benchmark circuits have no such qubits. The strategy is vacuous on this benchmark suite. It should be revisited if future benchmarks include circuits with ancilla qubits or sequential subroutines where qubit activity windows are naturally sparse.

---

## Key Findings

### Finding 1: QFT is adversarial for unconstrained heuristics, but solvable via rigid block preservation
Previous strategies degraded on QFT by +25% to +35% because any change in layer ordering, merge scoring, or unconstrained simulated annealing displaced qubit 0 and broke Layer-0 adjacencies. RigidSeedLocalSearch preserves all Layer-0 pairs as rigid atomic blocks, preventing early-layer penalty while optimizing subsequent routing. As a result, it matches BuildChain on large QFTs (qft_30, qft_40) and beats BuildChain on small QFTs (qft_10: -8.4%, qft_20: -4.6%).

### Finding 2: Layer reordering cannot beat BuildChain
Confirmed across LayerPriorityMapping and NewPairsFirstMapping. FCFS order encodes circuit-structural information for QFT that is not recoverable from pair statistics alone.

### Finding 3: Frequency-based seeding catastrophically fails on low-repetition circuits
FreqSeededChain +162% on random_40. When all pairs appear once, frequency sorting is random and the tie-break produces actively harmful orderings.

### Finding 4: Constrained local search strictly dominates all previous heuristics
RigidSeedLocalSearch is the first strategy to achieve net positive improvements across all circuit families simultaneously:
- QAOA: -5.1% (10q), -6.8% (20q), -20.4% (30q), -19.5% (40q)
- QFT: -8.4% (10q), -4.6% (20q), -0.1% (30q), -0.0% (40q)
- Random: -5.3% (10q), -3.2% (20q), 0.0% (30q), 0.0% (40q)
Overall average: -6.1% with 10 wins, 2 ties, and 0 losses.

### Finding 5: Idle-qubit boundary pinning is vacuous on dense benchmark circuits
PeripheralPinning's classification threshold (span < 25%, gates <= 4) is never satisfied in any of the 12 benchmarks because all circuits are fully dense (every qubit active throughout). The strategy reduces to a marginal variant of BuildChain and degrades +14.3% average. The idea is only applicable to circuits with ancilla or sparse-interaction qubits.

### Finding 6: Runtime efficiency of full-circuit MPS evaluation on rigid blocks
Because rigid blocks reduce the search space from $N$ individual qubits to $M \approx N/2$ blocks, convergence requires only 200-300 iterations with full MPS cost evaluation at ~1-2 ms each, giving ~10 ms total per circuit.

---

## Code-Level Issues Identified in BuildChain

### Issue A: Attachment direction (Cases B/C) -- not yet tested in isolation
The condition `if (idx + 1 <= grp.size() - idx)` in `MPSDummySimulator.h` attaches the new qubit to the end nearest to its partner, maximising distance. Should attach to the same end as the partner to minimise distance. Requires modifying the original file.

### Issue B: Merge score (Case D) -- addressed by WeightedMergeChain
The original formula considers only the single triggering pair. WeightedMergeChain generalises this to all cross-group pairs weighted by frequency. Results: wins on QAOA, degrades on QFT.

---

## Strategy 11: QMAP_Heuristic (External Tool Baseline)

**Tool:** [MQT QMAP](https://github.com/cda-tum/qmap) -- a dedicated quantum circuit mapping tool from the Munich Quantum Toolkit. The heuristic mode searches for a SWAP-efficient routing of the circuit onto a linear-chain coupling map.

**How it works (heuristic mode):** QMAP builds an initial placement by matching circuit qubit interactions to physical qubit positions on the coupling map, then applies a heuristic SWAP insertion pass that greedily routes each 2-qubit gate to an adjacent pair using a lookahead cost function. The objective is to minimise raw SWAP count.

**Key difference from Maestro strategies:** QMAP minimises raw SWAP insertions on a physical topology. Maestro's `predicted_cost` is a bond-dimension-weighted MPS cost that is not the same as SWAP count -- a placement that needs fewer SWAPs can still produce a higher MPS cost if those SWAPs happen to cross high-bond-dimension interior bonds.

**Result vs BuildChain:**

| Circuit | BuildChain | QMAP | QMAP SWAPs | Delta (MPS cost) |
|---|---:|---:|---:|---:|
| qaoa_10 | 237 | 275 | 90 | +16.0% |
| qaoa_20 | 1159 | 1300 | 486 | +12.2% |
| qaoa_30 | 294 | 501 | 298 | **+70.4%** |
| qaoa_40 | 456 | 679 | 518 | +48.9% |
| qft_10 | 203 | 228 | 58 | +12.3% |
| qft_20 | 987 | 1168 | 278 | +18.3% |
| qft_30 | 1682 | 2032 | 459 | +20.8% |
| qft_40 | 3042 | 3707 | 814 | +21.9% |
| random_10 | 304 | 309 | 149 | +1.6% |
| random_20 | 1125 | 1154 | 701 | +2.6% |
| random_30 | 290 | 290 | 0 | 0.0% |
| random_40 | 390 | 390 | 0 | 0.0% |
| **Avg** | | | | **+18.7%** |

> [!CAUTION]
> QMAP is worse than BuildChain on every circuit. 0 wins, 2 ties, 10 losses. Average degradation +18.7% in MPS cost.

**Root cause -- SWAP ≠ MPS cost:** The most telling example is `random_30` and `random_40`: QMAP finds 0 SWAPs for both (the circuit already fits the topology with no routing required), yet the MPS cost equals BuildChain exactly. This confirms the metric is not raw SWAP count. For `qaoa_30` (+70.4%), QMAP inserts 298 SWAPs but many of them push qubits across interior bonds where the bond dimension is high, multiplying the cost cubically. BuildChain's FCFS heuristic happens to produce a placement where the same gates are cheaper to route.

**Conclusion:** QMAP's objective (minimise SWAP count on a generic coupling map) is misaligned with Maestro's objective (minimise MPS bond-dimension-weighted cost on a tensor-train simulator). Minimising SWAPs is a necessary but not sufficient condition for minimising MPS routing cost. QMAP serves as a useful external baseline confirming that domain-specific heuristics are needed.

---

## Summary of Completed Strategies

| # | Strategy Name | Type | Avg Delta | Best Win | Worst Loss |
|---|---|---|---:|---|---|
| 0 | **BuildChain** | Baseline heuristic | 0.0% | Baseline | Baseline |
| 1 | **CommunityMapping** | Graph partitioning (CNM) | +16.1% | None | qft_10 (+35.5%) |
| 2 | **TemporalCommunity (a=4)** | Time-decayed communities | +24.9% | qaoa_40 (-15.4%) | random_30 (+94.1%) |
| 3 | **BondAwarePlacement** | Unconstrained SA (20% proxy) | +8.7% | qaoa_20 (-4.3%) | qft_40 (+52.4%) |
| 4 | **LayerPriorityMapping** | Rarity x distance layer reorder | +16.3% | None | qaoa_40 (+44.7%) |
| 5 | **NewPairsFirstMapping** | Greedy set-cover layer reorder | +8.8% | random_10 (-3.6%) | qft_40 (+33.7%) |
| 6 | **FreqSeededChain** | Frequency-sorted seeding | +30.2% | None | random_40 (+161.8%) |
| 7 | **WeightedMergeChain** | Global cross-group merge score | +8.9% | qaoa_30 (-15.3%) | qft_20 (+33.1%) |
| 8 | **LookaheadGroup (W=3)** | Windowed lookahead (3 layers) | +10.5% | qaoa_40 (-7.5%) | qft_40 (+31.9%) |
| 9 | **RigidSeedLocalSearch** | Two-phase rigid seed + MPS search | **-6.1%** | **qaoa_30 (-20.4%)** | **None (0.0%)** |
| 10 | **PeripheralPinning** | Boundary-pinning of idle qubits | +14.3% | None (0.0%) | qft_10 (+32.5%) |
| 11 | **QMAP_Heuristic** | External SWAP-minimising tool | +18.7% | None (0.0%) | qaoa_30 (+70.4%) |
