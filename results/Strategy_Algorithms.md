# Initial Mapping Strategies: Algorithm Descriptions

All strategies produce a qubit-to-position assignment for the MPS chain.
The baseline is **BuildChain** (the existing Maestro heuristic).

---

## Strategy 0: BuildChain (Baseline)

Scans through layers from first to last and processes every 2-qubit pair it sees:

- If neither qubit has been placed yet -- start a new group `[q1, q2]`
- If one is placed -- attach the other qubit to the nearest end of the existing group
- If both are placed in different groups -- merge those two groups, choosing the order that minimises the distance between q1 and q2
- If both already share a group -- do nothing

The final ordering of these groups becomes the proposed chain placement.

---

## Strategy 1: CommunityMapping

Build a weighted graph where each edge weight counts how many 2-qubit gates connect that pair of qubits across the whole circuit.

Run a greedy community detection algorithm (CNM / Louvain-style) on that graph to find clusters of frequently-interacting qubits.

Place each cluster as a contiguous block in the chain. Within each block, order qubits by how strongly they connect to their neighbours. Concatenate all blocks.

---

## Strategy 2: TemporalCommunityMapping

Same as CommunityMapping, but edge weights are not uniform. A gate in layer $l$ of $L$ total layers contributes weight $e^{-\alpha \cdot l / L}$ instead of 1. This makes early-layer interactions count more when building communities.

Three values of alpha were tested: 1, 2, and 4. Higher alpha means earlier layers dominate the community structure.

---

## Strategy 3: BondAwarePlacement

Start from the BuildChain result. Repeatedly propose a random swap of two qubits in the placement and score the new placement by actually running the circuit through the MPS cost model (using the first 20% of layers as a proxy).

Accept the swap if it reduces cost. Accept it with a small probability even if it increases cost (simulated annealing). Cool the acceptance probability over time. Return the best placement seen across all iterations.

---

## Strategy 4: LayerPriorityMapping

Score every layer by summing a priority value for each 2-qubit gate in that layer. The priority of a gate is: (1 + |q1 - q2|) / frequency(q1, q2). Rare pairs between distant qubits score highest.

Reorder layers from highest to lowest total score. Feed the reordered layers into BuildChain's standard algorithm.

The idea: rare, long-range pairs are seen first, so BuildChain makes adjacency decisions for the hardest cases before the chain fills up.

---

## Strategy 5: NewPairsFirstMapping

Score every layer by counting how many qubit pairs in that layer have not yet appeared in any previously seen layer (pairs that introduce genuinely new qubits if placed now).

Process layers in descending order of that count. Feed the reordered layers into BuildChain's standard algorithm.

The idea: the most information-rich layers (those introducing the most new qubits) are processed first.

---

## Strategy 6: FreqSeededChain

Count how many times each qubit pair appears across the whole circuit.

Sort all pairs in descending order of frequency. Feed the sorted list of pairs into BuildChain's placement logic (same Cases A/B/C/D rules) instead of layer-ordered pairs.

The idea: highly repeated pairs are likely the most important adjacencies, so they seed the chain first.

---

## Strategy 7: WeightedMergeChain

Run BuildChain exactly as normal for the new-group and single-qubit-attachment decisions.

Change only the merge decision (both qubits in different groups): instead of minimising the distance for the single triggering pair, score both merge orientations by summing across all pairs of qubits that interact across the two groups, weighting each pair by its global frequency. Pick the orientation with the lower total weighted distance.

Fall back to the original single-pair formula if no cross-group pair has any recorded interaction.

---

## Strategy 8: LookaheadGroupW3 / LookaheadGroupW5

Run BuildChain's layer-by-layer scan. For the attachment and merge decisions, instead of using only the triggering pair, look ahead into the next W layers (W = 3 or W = 5).

Score both candidate orientations by summing over all qubit pairs that appear in those upcoming layers, weighting each by exp(-delta) where delta is how many layers ahead that pair appears. Pick the orientation with the lower total lookahead cost.

---

## Strategy 9: RigidSeedLocalSearch

**Phase 1 -- frozen initial state.** Run BuildChain to get an initial chain placement. Find all 2-qubit gates that appear in Layer 0. Bundle each such pair into an atomic 2-qubit block [u, v]. Every qubit not adjacent to a Layer-0 partner becomes a 1-qubit block [w]. The chain is now a sequence of blocks instead of individual qubits.

**Phase 2 -- block-level local search.** Repeatedly apply one of three moves to the block sequence:
- Swap two blocks
- Reverse a contiguous sub-sequence of blocks (2-opt)
- Move a single-qubit block to the front or back of the chain

After each move, evaluate the full circuit cost using the MPS cost model. Accept the new ordering if it is cheaper. Accept it with a small probability if it is slightly more expensive (simulated annealing, cooling over time). If no improvement over BuildChain is ever found, return BuildChain's original placement unchanged.

The key property: blocks are never broken, so Layer-0 adjacent pairs always stay adjacent.

---

## Strategy 10: PeripheralPinning

Profile the circuit: for each qubit, compute the range of layers it appears in and how many gates it participates in. A qubit is "isolated" if its active range spans less than 25% of all layers and it has at most 4 total gates.

Run BuildChain's standard scan with one change: when an isolated qubit is being attached to an existing group, push it to the far end of the group (the boundary) rather than the nearest end.

After the chain is assembled, any isolated qubit still in the interior is swapped to the nearest boundary position occupied by a non-isolated qubit.

The idea: isolated qubits that finish interacting early should sit at the chain boundary where bond dimensions are lowest, so they do not block traffic through the high-cost interior bonds later.

*(Had no effect on any of the 12 benchmark circuits because all circuits are fully dense -- no qubit satisfied the isolation threshold.)*

---

## Strategy 11: QMAP_Heuristic (External Tool Baseline)

QMAP (MQT QMAP, Munich Quantum Toolkit) is an external quantum circuit mapping tool. It is not a placement heuristic written for Maestro -- it is an independent compiler pass run as an external baseline for comparison.

QMAP is given the circuit and a linear-chain coupling map (matching the MPS topology). It then:

- Finds an initial placement by matching circuit qubit interactions to physical positions on the chain
- Applies a heuristic SWAP-insertion pass that greedily routes each 2-qubit gate to an adjacent pair, using a lookahead function to estimate future routing cost
- Returns the compiled circuit (with SWAPs inserted) and the initial qubit assignment

The key distinction: QMAP's objective is to minimise the number of SWAP gates on a generic topology. Maestro's cost (`predicted_cost`) is a bond-dimension-weighted MPS cost, which is not the same thing -- a placement that needs fewer SWAPs can still be more expensive in MPS terms if those SWAPs cross interior bonds where the bond dimension is high.

**Outcome:** QMAP was worse than BuildChain on every circuit (+18.7% average). The most extreme case was `qaoa_30` (+70.4%), where QMAP inserted many SWAPs through the high-cost interior of the chain. This confirms that minimising raw SWAP count is misaligned with minimising MPS routing cost, and that domain-specific heuristics are necessary.
