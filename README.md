# Optimising Qubit Routing in Matrix Product State Quantum Circuit Simulators

> **Thesis**

## Current Status

1. **Environment setup** — Completed. Maestro builds successfully.

2. **SWAP cost benchmarking** — Completed. Results in `results/mapping_comparison_results.csv` and `benchmark_results.csv`.

3. **Code analysis**
   - **3a.** Initial logic analysis — Completed. Documented in `Existing Logics/swap_cost_pseudocode.md`
   - **3b.** Validation of existing cost computation — Completed. Documented in `SWAP cost model Analysis/Validation_of_Existing_SWAP_Cost_Model.md` and `results/MPS_Cost_Model_Validation_Report.md`
   - **3c.** Improving the cost model — Proposals completed. Documented in `SWAP cost model Analysis/Surrogate_Cost_Model_Proposal.md`

4. **Literature survey on mapping techniques** — Completed.

5. **Initial mapping strategy experiments** — Completed. 12 strategies benchmarked on 12 circuits (QAOA / QFT / Random at 10–40 qubits), cost model = cubic, bond dim = 64.

   | # | Strategy | Avg Δ vs BuildChain |
   |---|---|---:|
   | 0 | BuildChain (baseline) | 0.0% |
   | 1 | CommunityMapping | +16.1% |
   | 2 | TemporalCommunityMapping | +24.9% |
   | 3 | BondAwarePlacement | +8.7% |
   | 4 | LayerPriorityMapping | +16.3% |
   | 5 | NewPairsFirstMapping | +8.8% |
   | 6 | FreqSeededChain | +30.2% |
   | 7 | WeightedMergeChain | +8.9% |
   | 8 | LookaheadGroup (W=3/5) | +10.5% |
   | 9 | **RigidSeedLocalSearch** ✓ | **−6.1%** |
   | 10 | PeripheralPinning | +14.3% |
   | 11 | QMAP_Heuristic (external) | +18.7% |

   Full results, per-circuit tables, root cause analysis, and plain-language algorithm descriptions:
   - `results/Mapping_Strategy_Research_Log.md`
   - `results/Strategy_Algorithms.md`
   - All raw CSVs in `results/`

6. **Next** — Improve `RigidSeedLocalSearch` (only strategy to beat the baseline).
