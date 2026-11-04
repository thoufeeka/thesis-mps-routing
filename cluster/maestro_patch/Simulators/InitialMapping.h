// InitialMapping.h
//
// Strategy interface for initial qubit mapping in MPS simulation.
//
// Add new strategies by extending the enum and handling them in
// ComputeMappingWithInfo(). Nothing else in the codebase needs to change.
//
// Strategies:
//   BuildChain              - wraps MPSDummySimulator::ComputeOptimalQubitsMap
//   CommunityMapping        - community-detection-guided placement
//   TemporalCommunityMapping - community detection with exponential layer-decay weights
//   BondAwarePlacement      - SA that directly minimises MPS bond-dimension cost
//   LayerPriorityMapping    - reorders layers by (rarity x distance) before BuildChain
//   NewPairsFirstMapping    - greedy set-cover layer reorder: most-new-pairs layer first
//   FreqSeededChain         - feeds pairs to BuildChain sorted by global frequency (most-frequent first)
//   WeightedMergeChain      - FCFS seeding like BuildChain, but merge orientation scores all future cross-group pairs
//   LookaheadGroupW3        - FCFS seeding; attachment+merge decisions use windowed lookahead (W=3 layers)
//   LookaheadGroupW5        - same with W=5 layers
//   RigidSeedLocalSearch    - BuildChain frozen rigid seed + constrained MPS-cost local search
//   PeripheralPinning       - BuildChain variant: isolated qubits pinned to chain boundaries, hubs kept interior

#pragma once

#ifndef _INITIALMAPPING_H_
#define _INITIALMAPPING_H_

#include <algorithm>
#include <cassert>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

#include "MPSDummySimulator.h"
#include "../Circuit/Circuit.h"

namespace Simulators {

// --- Strategy enum ---

// Add new strategies here; handle them in ComputeMappingWithInfo().
enum class InitialMappingStrategy {
    BuildChain,               // existing Maestro heuristic (baseline)
    CommunityMapping,         // static community-detection placement
    TemporalCommunityMapping, // community detection with exponential layer-decay weights
    BondAwarePlacement,       // SA: minimises bond-dimension-weighted cost directly
    LayerPriorityMapping,     // reorders layers by rarity+distance before BuildChain
    NewPairsFirstMapping,     // greedy set-cover: most-new-pairs layer gets priority
    FreqSeededChain,           // BuildChain seeded by global pair frequency (most common first)
    WeightedMergeChain,        // FCFS seeding like BuildChain; merge orientation uses frequency-weighted global score
    LookaheadGroupW3,         // FCFS seeding; attachment+merge scored by windowed lookahead (W=3)
    LookaheadGroupW5,         // FCFS seeding; attachment+merge scored by windowed lookahead (W=5)
    RigidSeedLocalSearch,     // Two-Phase: BuildChain frozen rigid seed + MPS-cost preservation search
    PeripheralPinning         // BuildChain variant: isolated qubits pinned to chain boundary; hubs interior
};

// Human-readable name for CSV output and logging.
inline std::string GetStrategyName(InitialMappingStrategy s) {
    switch (s) {
        case InitialMappingStrategy::BuildChain:               return "BuildChain";
        case InitialMappingStrategy::CommunityMapping:         return "CommunityMapping";
        case InitialMappingStrategy::TemporalCommunityMapping: return "TemporalCommunityMapping";
        case InitialMappingStrategy::BondAwarePlacement:       return "BondAwarePlacement";
        case InitialMappingStrategy::LayerPriorityMapping:     return "LayerPriorityMapping";
        case InitialMappingStrategy::NewPairsFirstMapping:     return "NewPairsFirstMapping";
        case InitialMappingStrategy::FreqSeededChain:           return "FreqSeededChain";
        case InitialMappingStrategy::WeightedMergeChain:        return "WeightedMergeChain";
        case InitialMappingStrategy::LookaheadGroupW3:          return "LookaheadGroupW3";
        case InitialMappingStrategy::LookaheadGroupW5:          return "LookaheadGroupW5";
        case InitialMappingStrategy::RigidSeedLocalSearch:     return "RigidSeedLocalSearch";
        case InitialMappingStrategy::PeripheralPinning:         return "PeripheralPinning";
    }
    return "Unknown";
}

// --- Result struct ---

// Holds the output of ComputeMappingWithInfo().
// map[logicalQubit] = physPos (Maestro convention).
struct MappingResult {
    std::vector<long long int>              map;
    int                                     numCommunities = 1;
    std::vector<std::vector<long long int>> communityGroups;  // non-empty for community strategies
};

// --- Internal helpers (not part of the public API) ---
namespace detail {

using IndexType = MPSDummySimulator::IndexType;

// Build the weighted qubit interaction graph from circuit layers.
// w[i][j] = total number of 2-qubit gates between qubits i and j.
// Single-qubit gates are ignored.
inline std::vector<std::vector<double>> BuildInteractionGraph(
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>& layers,
    size_t nrQubits)
{
    std::vector<std::vector<double>> w(nrQubits,
                                       std::vector<double>(nrQubits, 0.0));
    for (const auto& layer : layers) {
        for (const auto& op : layer->GetOperations()) {
            const auto qbits = op->AffectedQubits();
            if (qbits.size() >= 2) {
                const size_t q1 = qbits[0];
                const size_t q2 = qbits[1];
                if (q1 < nrQubits && q2 < nrQubits && q1 != q2) {
                    w[q1][q2] += 1.0;
                    w[q2][q1] += 1.0;
                }
            }
        }
    }
    return w;
}

// Build a temporally-weighted qubit interaction graph.
//
// Each 2-qubit gate in layer l (out of L total) contributes:
//   edge_weight += exp(-alpha * l / L)
//
// Layer 0 contributes 1.0; the last layer contributes exp(-alpha).
// With alpha=2 the ratio is ~7x, so early interactions dominate.
//
// Effect on community detection:
//   QFT  : early layers have nearest-neighbour pairs → communities form
//           around contiguous blocks, matching BuildChain better.
//   QAOA : interactions are spread across layers → modest effect.
//   Random: brick-wall structure is already contiguous → robust to alpha.
inline std::vector<std::vector<double>> BuildTemporalInteractionGraph(
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>& layers,
    size_t nrQubits,
    double alpha = 2.0)
{
    std::vector<std::vector<double>> w(nrQubits,
                                       std::vector<double>(nrQubits, 0.0));
    const double L = static_cast<double>(layers.size());
    if (L < 1.0) return w;

    for (size_t li = 0; li < layers.size(); ++li) {
        // layer 0 → weight 1.0, layer L-1 → weight exp(-alpha)
        const double layerWeight = std::exp(-alpha * static_cast<double>(li) / L);

        for (const auto& op : layers[li]->GetOperations()) {
            const auto qbits = op->AffectedQubits();
            if (qbits.size() >= 2) {
                const size_t q1 = qbits[0];
                const size_t q2 = qbits[1];
                if (q1 < nrQubits && q2 < nrQubits && q1 != q2) {
                    w[q1][q2] += layerWeight;
                    w[q2][q1] += layerWeight;
                }
            }
        }
    }
    return w;
}

// Order a subset of qubits into a linear chain using the same greedy
// pair-merging logic as buildChain inside ComputeOptimalQubitsMap,
// but restricted to a single community's qubit set.
//
// orderedPairs  - 2-qubit gate pairs in layer/gate order (earlier = higher priority)
// qubitSet      - qubits in this community
// totalNrQubits - full qubit index space size (for vector allocation)
//
// Returns chain where chain[physPos] = logicalQubit.
inline std::vector<IndexType> BuildChainRestricted(
    const std::vector<std::pair<IndexType, IndexType>>& orderedPairs,
    const std::vector<IndexType>&                       qubitSet,
    IndexType                                           totalNrQubits)
{
    const size_t n = qubitSet.size();
    if (n == 0) return {};
    if (n == 1) return qubitSet;

    // membership lookup O(1)
    std::vector<bool> inCommunity(totalNrQubits, false);
    for (IndexType q : qubitSet) inCommunity[q] = true;

    std::vector<int>                     qubitGroup(totalNrQubits, -1);
    std::vector<std::deque<IndexType>>   groups;
    std::unordered_set<IndexType>        placed;

    for (const auto& p : orderedPairs) {
        const IndexType q1 = p.first;
        const IndexType q2 = p.second;

        // skip pairs that cross community boundaries
        if (!inCommunity[q1] || !inCommunity[q2]) continue;

        const int g1 = qubitGroup[q1];
        const int g2 = qubitGroup[q2];

        if (g1 < 0 && g2 < 0) {
            // neither placed: start a new group
            const int gIdx = static_cast<int>(groups.size());
            groups.push_back({q1, q2});
            qubitGroup[q1] = gIdx;
            qubitGroup[q2] = gIdx;
            placed.insert(q1);
            placed.insert(q2);

        } else if (g1 >= 0 && g2 < 0) {
            // q1 placed, q2 not: attach q2 at the closer end of q1's group
            auto& grp = groups[g1];
            size_t idx = 0;
            for (size_t i = 0; i < grp.size(); ++i)
                if (grp[i] == q1) { idx = i; break; }
            if (idx + 1 <= grp.size() - idx)
                grp.push_front(q2);
            else
                grp.push_back(q2);
            qubitGroup[q2] = g1;
            placed.insert(q2);

        } else if (g1 < 0 && g2 >= 0) {
            // q2 placed, q1 not: attach q1 at the closer end of q2's group
            auto& grp = groups[g2];
            size_t idx = 0;
            for (size_t i = 0; i < grp.size(); ++i)
                if (grp[i] == q2) { idx = i; break; }
            if (idx + 1 <= grp.size() - idx)
                grp.push_front(q1);
            else
                grp.push_back(q1);
            qubitGroup[q1] = g2;
            placed.insert(q1);

        } else if (g1 >= 0 && g2 >= 0 && g1 != g2) {
            // both in different groups: merge so q1 and q2 end up adjacent
            auto& grpA = groups[g1];
            auto& grpB = groups[g2];
            size_t idxA = 0, idxB = 0;
            for (size_t i = 0; i < grpA.size(); ++i)
                if (grpA[i] == q1) { idxA = i; break; }
            for (size_t i = 0; i < grpB.size(); ++i)
                if (grpB[i] == q2) { idxB = i; break; }

            // [A][B]: dist = |A| + idxB - idxA
            // [B][A]: dist = |B| + idxA - idxB
            const size_t distAB = grpA.size() + idxB - idxA;
            const size_t distBA = grpB.size() + idxA - idxB;

            int mergedGroup;
            if (distAB <= distBA) {
                for (const auto q : grpB) grpA.push_back(q);
                grpB.clear();
                mergedGroup = g1;
            } else {
                for (const auto q : grpA) grpB.push_back(q);
                grpA.clear();
                mergedGroup = g2;
            }
            for (const auto q : groups[mergedGroup])
                qubitGroup[q] = mergedGroup;
        }
        // if g1 == g2: both already in the same group, do nothing

        if (placed.size() == n) break;
    }

    // assemble chain from groups in creation order
    std::vector<IndexType> chain;
    chain.reserve(n);
    for (const auto& grp : groups)
        for (const auto q : grp) chain.push_back(q);

    // append any community qubits with no 2-qubit interactions
    for (const auto q : qubitSet)
        if (placed.find(q) == placed.end()) chain.push_back(q);

    return chain;
}

// Greedy Newman-Moore modularity community detection (CNM agglomeration).
//
// Starts with N singletons and repeatedly merges the pair (A, B) with
// the highest modularity gain ΔQ = 2*(e_AB - a_A * a_B).
// Only merges when e_AB > 0 and ΔQ > 0. Stops when no beneficial merge remains.
// Complexity O(N^3) – fine for N ≤ ~50.
//
// Returns a vector of communities, each sorted by qubit index.
inline std::vector<std::vector<IndexType>> DetectCommunities(
    const std::vector<std::vector<double>>& w,
    size_t nrQubits)
{
    std::vector<double> deg(nrQubits, 0.0);
    double totalWeight = 0.0;
    for (size_t i = 0; i < nrQubits; ++i)
        for (size_t j = 0; j < nrQubits; ++j)
            deg[i] += w[i][j];
    for (size_t i = 0; i < nrQubits; ++i)
        totalWeight += deg[i];
    totalWeight /= 2.0;  // each edge counted twice

    // no edges → return singletons
    if (totalWeight < 1e-12) {
        std::vector<std::vector<IndexType>> result;
        result.reserve(nrQubits);
        for (size_t i = 0; i < nrQubits; ++i)
            result.push_back({static_cast<IndexType>(i)});
        return result;
    }

    const double inv2m = 1.0 / (2.0 * totalWeight);

    struct Community {
        std::vector<IndexType> members;
        double a;    // fraction of edge-endpoints in this community = k_C/(2m)
        double eii;  // fraction of edges fully inside this community
        bool   alive;
    };

    std::vector<Community> comms(nrQubits);
    for (size_t i = 0; i < nrQubits; ++i) {
        comms[i].members = {static_cast<IndexType>(i)};
        comms[i].a       = deg[i] * inv2m;
        comms[i].eii     = 0.0;
        comms[i].alive   = true;
    }

    // e[i][j] = fraction of total edge weight between communities i and j
    std::vector<std::vector<double>> e(nrQubits,
                                       std::vector<double>(nrQubits, 0.0));
    for (size_t i = 0; i < nrQubits; ++i)
        for (size_t j = 0; j < nrQubits; ++j)
            if (i != j)
                e[i][j] = w[i][j] * inv2m;

    int numAlive = static_cast<int>(nrQubits);

    while (numAlive > 1) {
        double bestDQ  = 0.0;
        int    bestA   = -1;
        int    bestB   = -1;

        for (size_t i = 0; i < nrQubits; ++i) {
            if (!comms[i].alive) continue;
            for (size_t j = i + 1; j < nrQubits; ++j) {
                if (!comms[j].alive) continue;
                if (e[i][j] < 1e-12)  continue;

                const double dQ = 2.0 * (e[i][j] - comms[i].a * comms[j].a);

                // ties broken by lower community index for determinism
                if (dQ > bestDQ ||
                    (dQ == bestDQ && static_cast<int>(i) < bestA)) {
                    bestDQ = dQ;
                    bestA  = static_cast<int>(i);
                    bestB  = static_cast<int>(j);
                }
            }
        }

        if (bestA < 0) break;  // no merge improves modularity

        const int A = bestA;
        const int B = bestB;

        // redistribute B's connections to A
        for (size_t k = 0; k < nrQubits; ++k) {
            if (!comms[k].alive) continue;
            if (static_cast<int>(k) == A || static_cast<int>(k) == B) continue;
            e[A][k] += e[B][k];
            e[k][A] += e[k][B];
            e[B][k] = 0.0;
            e[k][B] = 0.0;
        }

        comms[A].eii += comms[B].eii + 2.0 * e[A][B];
        e[A][B] = 0.0;
        e[B][A] = 0.0;

        comms[A].a += comms[B].a;

        for (IndexType q : comms[B].members)
            comms[A].members.push_back(q);

        comms[B].alive = false;
        --numAlive;
    }

    // collect alive communities; sort members for determinism
    std::vector<std::vector<IndexType>> result;
    result.reserve(numAlive);
    for (size_t i = 0; i < nrQubits; ++i) {
        if (!comms[i].alive) continue;
        auto& m = comms[i].members;
        std::sort(m.begin(), m.end());
        result.push_back(std::move(m));
    }

    // largest community first; tie-break by smallest first-qubit index
    std::stable_sort(result.begin(), result.end(),
        [](const std::vector<IndexType>& a, const std::vector<IndexType>& b) {
            if (a.size() != b.size()) return a.size() > b.size();
            return a[0] < b[0];
        });

    return result;
}

// Order a list of communities into a linear chain by greedily appending
// the community most connected to the already-placed ones.
// Returns indices into the communities vector in placement order.
inline std::vector<size_t> OrderCommunities(
    const std::vector<std::vector<IndexType>>&  communities,
    const std::vector<std::vector<double>>&     w,
    size_t                                      nrQubits)
{
    const size_t nc = communities.size();
    if (nc <= 1) {
        std::vector<size_t> order(nc);
        std::iota(order.begin(), order.end(), 0);
        return order;
    }

    std::vector<int> qubitToCommunity(nrQubits, -1);
    for (size_t ci = 0; ci < nc; ++ci)
        for (IndexType q : communities[ci])
            qubitToCommunity[q] = static_cast<int>(ci);

    // inter-community edge weights
    std::vector<std::vector<double>> interW(nc, std::vector<double>(nc, 0.0));
    for (size_t i = 0; i < nrQubits; ++i)
        for (size_t j = i + 1; j < nrQubits; ++j) {
            const int ci = qubitToCommunity[i];
            const int cj = qubitToCommunity[j];
            if (ci >= 0 && cj >= 0 && ci != cj && w[i][j] > 0.0) {
                interW[ci][cj] += w[i][j];
                interW[cj][ci] += w[i][j];
            }
        }

    // intra-community weights — used to pick the starting community
    std::vector<double> intraW(nc, 0.0);
    for (size_t ci = 0; ci < nc; ++ci) {
        const auto& members = communities[ci];
        for (size_t a = 0; a < members.size(); ++a)
            for (size_t b = a + 1; b < members.size(); ++b)
                intraW[ci] += w[members[a]][members[b]];
    }

    // start with the densest community (ties → largest)
    std::vector<bool>   placed(nc, false);
    std::vector<size_t> order;
    order.reserve(nc);

    {
        size_t best = 0;
        for (size_t ci = 1; ci < nc; ++ci) {
            if (intraW[ci] > intraW[best] ||
                (intraW[ci] == intraW[best] &&
                 communities[ci].size() > communities[best].size()))
                best = ci;
        }
        order.push_back(best);
        placed[best] = true;
    }

    // greedily append the unplaced community most connected to any placed one
    while (order.size() < nc) {
        double bestConn   = -1.0;
        size_t bestNext   = nc;

        for (size_t ci = 0; ci < nc; ++ci) {
            if (placed[ci]) continue;
            double conn = 0.0;
            for (size_t pi : order) conn += interW[pi][ci];

            // tie-break by smaller first qubit index
            if (bestNext == nc || conn > bestConn ||
                (conn == bestConn &&
                 communities[ci][0] < communities[bestNext][0])) {
                bestConn = conn;
                bestNext = ci;
            }
        }
        order.push_back(bestNext);
        placed[bestNext] = true;
    }

    return order;
}

// Fast MPS cost oracle used by BondAwarePlacement.
// Creates a fresh MPSDummySimulator, sets the given mapping, runs gates
// through maxLayers (0 = all), and returns the total swapping cost.
// Typical runtime: <1 ms for K=10 layers on a 40-qubit circuit.
inline double EvaluateMappingCost(
    size_t                                                             nrQubits,
    MPSDummySimulator::CostModel                                       costModel,
    MPSDummySimulator::IndexType                                       maxBondDim,
    const std::vector<long long int>&                                  mapping,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>&           layers,
    size_t                                                             maxLayers = 0)
{
    MPSDummySimulator scorer(nrQubits);
    scorer.setCostModel(costModel);
    scorer.SetMaxBondDimension(maxBondDim);
    scorer.SetInitialQubitsMap(mapping);

    const size_t limit = (maxLayers == 0 || maxLayers >= layers.size())
                             ? layers.size()
                             : maxLayers;
    for (size_t li = 0; li < limit; ++li) {
        for (const auto& op : layers[li]->GetOperations())
            scorer.ApplyGate(op);
    }
    return scorer.getTotalSwappingCost();
}

// --------------------------------------------------------------------------
// ReorderLayersByPriority
//
// Reorders layers so that BuildChain's FCFS logic sees the most
// placement-critical pairs first.
//
// Scoring per pair (q1, q2):
//
//   priority(q1, q2) = (1 + distanceWeight * |q1 - q2|) / freq[q1][q2]
//
//   - Rare pairs (low freq) score high  → must be captured early; they won't
//     appear again to give BuildChain a second chance.
//   - Distant pairs (large |q1-q2|) score high → placing them adjacent avoids
//     the most expensive long-range SWAPs.
//   - Frequent adjacent pairs score low → captured naturally by FCFS anyway.
//
// Layer score = sum of pair scores for all 2-qubit gates in that layer.
// Layers are sorted descending by score.
// --------------------------------------------------------------------------
inline std::vector<std::shared_ptr<Circuits::Circuit<>>> ReorderLayersByPriority(
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>& layers,
    size_t                                                    nrQubits,
    double                                                    distanceWeight = 1.0)
{
    if (layers.size() <= 1) return layers;

    // Step 1: Build global interaction frequency matrix
    const auto freq = BuildInteractionGraph(layers, nrQubits);

    // Step 2: Score each layer
    std::vector<double> scores(layers.size(), 0.0);
    for (size_t li = 0; li < layers.size(); ++li) {
        double s = 0.0;
        for (const auto& op : layers[li]->GetOperations()) {
            const auto qbits = op->AffectedQubits();
            if (qbits.size() >= 2) {
                const size_t q1 = qbits[0];
                const size_t q2 = qbits[1];
                if (q1 < nrQubits && q2 < nrQubits && q1 != q2) {
                    const double f = freq[q1][q2];
                    if (f > 0.0) {
                        const double dist = std::abs(
                            static_cast<double>(q1) - static_cast<double>(q2));
                        s += (1.0 + distanceWeight * dist) / f;
                    }
                }
            }
        }
        scores[li] = s;
    }

    // Step 3: Sort layer indices by score descending (stable for determinism)
    std::vector<size_t> order(layers.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
        [&scores](size_t a, size_t b) { return scores[a] > scores[b]; });

    // Step 4: Build reordered vector
    std::vector<std::shared_ptr<Circuits::Circuit<>>> reordered;
    reordered.reserve(layers.size());
    for (size_t idx : order) reordered.push_back(layers[idx]);
    return reordered;
}

// --------------------------------------------------------------------------
// ReorderLayersByNewPairs  (Greedy Set-Cover)
//
// Reorders layers so that BuildChain sees the most informative pairs first.
//
// At each step, pick the unselected layer that introduces the greatest number
// of qubit pairs NOT yet seen in any already-selected layer.  Ties are broken
// by original layer index (deterministic).
//
// Why this is better than LayerPriorityMapping
// ---------------------------------------------
// LayerPriorityMapping used qubit-index distance as a proxy for "hard to
// route" -- but index distance does not reflect circuit structure.  This
// version is purely structure-driven: it prioritises layers that contain
// pairs BuildChain would never see under FCFS ordering, regardless of where
// those qubits sit in the index space.
//
// Complexity: O(L^2 * gates_per_layer) -- microseconds for N <= 40.
// --------------------------------------------------------------------------
inline std::vector<std::shared_ptr<Circuits::Circuit<>>> ReorderLayersByNewPairs(
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>& layers,
    size_t                                                    nrQubits)
{
    if (layers.size() <= 1) return layers;

    const size_t L = layers.size();

    // Precompute canonical pair keys per layer  (q1 < q2 => key = q1*N + q2)
    std::vector<std::vector<size_t>> layerKeys(L);
    for (size_t li = 0; li < L; ++li) {
        for (const auto& op : layers[li]->GetOperations()) {
            const auto qbits = op->AffectedQubits();
            if (qbits.size() >= 2) {
                size_t q1 = qbits[0], q2 = qbits[1];
                if (q1 < nrQubits && q2 < nrQubits && q1 != q2) {
                    if (q1 > q2) std::swap(q1, q2);
                    layerKeys[li].push_back(q1 * nrQubits + q2);
                }
            }
        }
        // Deduplicate within layer (a pair may appear multiple times)
        std::sort(layerKeys[li].begin(), layerKeys[li].end());
        layerKeys[li].erase(
            std::unique(layerKeys[li].begin(), layerKeys[li].end()),
            layerKeys[li].end());
    }

    // Greedy selection
    std::unordered_set<size_t> seen;
    seen.reserve(nrQubits * nrQubits);
    std::vector<bool> selected(L, false);
    std::vector<std::shared_ptr<Circuits::Circuit<>>> reordered;
    reordered.reserve(L);

    while (reordered.size() < L) {
        size_t bestLayer = L;   // sentinel (invalid)
        size_t bestNew   = 0;

        for (size_t li = 0; li < L; ++li) {
            if (selected[li]) continue;
            size_t newCount = 0;
            for (size_t key : layerKeys[li])
                if (seen.find(key) == seen.end()) ++newCount;

            // Prefer more new pairs; tie-break by original index (stable)
            if (bestLayer == L || newCount > bestNew) {
                bestNew   = newCount;
                bestLayer = li;
            }
        }

        selected[bestLayer] = true;
        reordered.push_back(layers[bestLayer]);
        for (size_t key : layerKeys[bestLayer]) seen.insert(key);
    }

    return reordered;
}

}  // namespace detail

// --- CommunityMapping ---

// Compute an initial qubit mapping using community detection.
//
// Steps:
//   1. Build weighted qubit interaction graph from all 2-qubit gates.
//   2. Run CNM greedy modularity community detection.
//   3. Order communities greedily by inter-community connectivity.
//   4. Within each community, order qubits with buildChain logic.
//   5. Concatenate community chains → full chain → qubitsMap.
//
// detectedCommunities (optional): receives community groups for verbose output.
// Returns mapping in Maestro convention: map[logicalQubit] = physPos.
inline std::vector<long long int> ComputeCommunityMapping(
    MPSDummySimulator&                                         sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>&  layers,
    std::vector<std::vector<long long int>>*                   detectedCommunities = nullptr)
{
    using IndexType = MPSDummySimulator::IndexType;
    const size_t nrQubits = sim.getNrQubits();

    // trivial fallback
    if (layers.empty() || nrQubits <= 2) {
        std::vector<long long int> identity(nrQubits);
        std::iota(identity.begin(), identity.end(), 0LL);
        if (detectedCommunities) {
            detectedCommunities->clear();
            for (size_t i = 0; i < nrQubits; ++i)
                detectedCommunities->push_back({static_cast<long long int>(i)});
        }
        return identity;
    }

    const auto w = detail::BuildInteractionGraph(layers, nrQubits);
    const auto communities = detail::DetectCommunities(w, nrQubits);

    if (detectedCommunities) {
        detectedCommunities->clear();
        for (const auto& c : communities) {
            std::vector<long long int> group(c.begin(), c.end());
            detectedCommunities->push_back(std::move(group));
        }
    }

    // flat ordered pair list for within-community ordering
    using Pair = std::pair<IndexType, IndexType>;
    std::vector<Pair> allPairsOrdered;
    for (const auto& layer : layers) {
        for (const auto& op : layer->GetOperations()) {
            auto qbits = op->AffectedQubits();
            if (qbits.size() >= 2) {
                IndexType q1 = static_cast<IndexType>(qbits[0]);
                IndexType q2 = static_cast<IndexType>(qbits[1]);
                if (q1 > q2) std::swap(q1, q2);
                allPairsOrdered.push_back({q1, q2});
            }
        }
    }

    const auto communityOrder = detail::OrderCommunities(communities, w, nrQubits);

    std::vector<IndexType> fullChain;
    fullChain.reserve(nrQubits);

    for (size_t ci : communityOrder) {
        const auto& members = communities[ci];
        auto localChain = detail::BuildChainRestricted(
            allPairsOrdered, members, static_cast<IndexType>(nrQubits));
        for (IndexType q : localChain)
            fullChain.push_back(q);
    }

    // safety: append any missed qubit (shouldn't happen)
    {
        std::unordered_set<IndexType> seen(fullChain.begin(), fullChain.end());
        for (size_t q = 0; q < nrQubits; ++q)
            if (seen.find(static_cast<IndexType>(q)) == seen.end())
                fullChain.push_back(static_cast<IndexType>(q));
    }

    assert(fullChain.size() == nrQubits);

    // chain[physPos] = logicalQubit  →  result[logicalQubit] = physPos
    std::vector<long long int> result(nrQubits);
    for (size_t i = 0; i < fullChain.size(); ++i)
        result[fullChain[i]] = static_cast<long long int>(i);

    return result;
}

// --- TemporalCommunityMapping ---

// Same pipeline as ComputeCommunityMapping, but the interaction graph is built
// with BuildTemporalInteractionGraph(alpha) instead of BuildInteractionGraph().
// This biases community formation toward pairs that interact in early layers,
// fixing the butterfly-grouping issue seen in QFT circuits.
//
// alpha=0 → same as CommunityMapping.
// alpha=2 (default) → early layers weighted ~7x over the last layer.
// alpha=4 → ~55x ratio (very aggressive temporal emphasis).
inline std::vector<long long int> ComputeTemporalCommunityMapping(
    MPSDummySimulator&                                         sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>&  layers,
    double                                                     alpha = 2.0,
    std::vector<std::vector<long long int>>*                   detectedCommunities = nullptr)
{
    using IndexType = MPSDummySimulator::IndexType;
    const size_t nrQubits = sim.getNrQubits();

    if (layers.empty() || nrQubits <= 2) {
        std::vector<long long int> identity(nrQubits);
        std::iota(identity.begin(), identity.end(), 0LL);
        if (detectedCommunities) {
            detectedCommunities->clear();
            for (size_t i = 0; i < nrQubits; ++i)
                detectedCommunities->push_back({static_cast<long long int>(i)});
        }
        return identity;
    }

    // use the temporal interaction graph instead of the plain one
    const auto w = detail::BuildTemporalInteractionGraph(layers, nrQubits, alpha);
    const auto communities = detail::DetectCommunities(w, nrQubits);

    if (detectedCommunities) {
        detectedCommunities->clear();
        for (const auto& c : communities) {
            std::vector<long long int> group(c.begin(), c.end());
            detectedCommunities->push_back(std::move(group));
        }
    }

    using Pair = std::pair<IndexType, IndexType>;
    std::vector<Pair> allPairsOrdered;
    for (const auto& layer : layers) {
        for (const auto& op : layer->GetOperations()) {
            auto qbits = op->AffectedQubits();
            if (qbits.size() >= 2) {
                IndexType q1 = static_cast<IndexType>(qbits[0]);
                IndexType q2 = static_cast<IndexType>(qbits[1]);
                if (q1 > q2) std::swap(q1, q2);
                allPairsOrdered.push_back({q1, q2});
            }
        }
    }

    // community-level ordering also uses the temporal weights
    const auto communityOrder = detail::OrderCommunities(communities, w, nrQubits);

    std::vector<IndexType> fullChain;
    fullChain.reserve(nrQubits);
    for (size_t ci : communityOrder) {
        auto localChain = detail::BuildChainRestricted(
            allPairsOrdered, communities[ci],
            static_cast<IndexType>(nrQubits));
        for (IndexType q : localChain) fullChain.push_back(q);
    }

    {
        std::unordered_set<IndexType> seen(fullChain.begin(), fullChain.end());
        for (size_t q = 0; q < nrQubits; ++q)
            if (seen.find(static_cast<IndexType>(q)) == seen.end())
                fullChain.push_back(static_cast<IndexType>(q));
    }
    assert(fullChain.size() == nrQubits);

    std::vector<long long int> result(nrQubits);
    for (size_t i = 0; i < fullChain.size(); ++i)
        result[fullChain[i]] = static_cast<long long int>(i);
    return result;
}

// --- BondAwarePlacement ---

// MPS-cost-aware initial placement via Simulated Annealing.
// Minimises Maestro's bond-dimension-weighted SWAP cost directly,
// rather than proxy metrics like SWAP count or community modularity.
//
// Steps:
//   1. Warm-start from BuildChain output.
//   2. Score it using the fast MPS cost oracle (first evalFraction layers only).
//   3. SA loop: propose random 2-position swap in the permutation.
//      - If cheaper: always accept.
//      - If more expensive: accept with probability exp(-delta/T).
//      - Cool each step: T *= coolingRate.
//   4. Return the best permutation seen.
//
// sim              - configured MPSDummySimulator (for cost model / bond dim)
// layers           - circuit decomposed into parallel layers
// maxItersMultiplier - MAX_ITERS = multiplier × N
// T0               - initial SA temperature
// coolingRate      - multiplicative cooling factor per iteration
// evalFraction     - fraction of layers used for scoring (0 < f <= 1.0)
// seed             - RNG seed (0 = use std::random_device)
// Returns mapping in Maestro convention: map[logicalQubit] = physPos.
inline std::vector<long long int> ComputeBondAwarePlacement(
    MPSDummySimulator&                                         sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>&  layers,
    int    maxItersMultiplier = 200,
    double T0                 = 1.0,
    double coolingRate        = 0.995,
    double evalFraction       = 0.20,
    unsigned seed             = 0)
{
    const size_t N = sim.getNrQubits();
    if (N <= 2 || layers.empty())
        return sim.ComputeOptimalQubitsMap(layers);

    const auto   costModel = sim.getCostModel();
    const auto&  bdVec     = sim.getMaxBondDimensions();
    const MPSDummySimulator::IndexType maxBondDim =
        bdVec.empty() ? 64 : static_cast<MPSDummySimulator::IndexType>(bdVec[0]);

    const size_t evalLayers = std::max(
        size_t(1),
        std::min(layers.size(),
                 static_cast<size_t>(std::ceil(evalFraction * layers.size()))));

    // warm start: BuildChain
    std::vector<long long int> bestMap = sim.ComputeOptimalQubitsMap(layers);
    double bestCost = detail::EvaluateMappingCost(
        N, costModel, maxBondDim, bestMap, layers, evalLayers);

    std::vector<long long int> curMap  = bestMap;
    double curCost = bestCost;

    std::mt19937 rng(seed == 0 ? std::random_device{}() : seed);
    std::uniform_int_distribution<size_t> pickQubit(0, N - 1);
    std::uniform_real_distribution<double> uniform01(0.0, 1.0);

    // physToLog[physical] = logical – lets us update curMap in O(1) per proposal
    std::vector<long long int> physToLog(N, -1);
    for (long long int log = 0; log < static_cast<long long int>(N); ++log)
        physToLog[curMap[log]] = log;

    const int maxIters = maxItersMultiplier * static_cast<int>(N);
    double T = T0;

    for (int iter = 0; iter < maxIters; ++iter) {
        // propose: pick two distinct logical qubits and swap their physical positions
        size_t la = pickQubit(rng);
        size_t lb = pickQubit(rng);
        while (lb == la) lb = pickQubit(rng);

        const long long int pa = curMap[la];
        const long long int pb = curMap[lb];

        curMap[la] = pb;
        curMap[lb] = pa;
        physToLog[pa] = static_cast<long long int>(lb);
        physToLog[pb] = static_cast<long long int>(la);

        const double newCost = detail::EvaluateMappingCost(
            N, costModel, maxBondDim, curMap, layers, evalLayers);

        const double delta = newCost - curCost;

        if (delta < 0.0 || (T > 1e-12 && uniform01(rng) < std::exp(-delta / T))) {
            curCost = newCost;
            if (curCost < bestCost) {
                bestCost = curCost;
                bestMap  = curMap;
            }
        } else {
            // revert
            curMap[la]    = pa;
            curMap[lb]    = pb;
            physToLog[pa] = static_cast<long long int>(la);
            physToLog[pb] = static_cast<long long int>(lb);
        }

        T *= coolingRate;
    }

    return bestMap;
}

// --------------------------------------------------------------------------
// LayerPriorityMapping
//
// Improvement over BuildChain that keeps the entire BuildChain algorithm
// intact but reorders the layers before processing.
//
// Motivation
// ----------
// BuildChain scans layers in FCFS order and stops making meaningful decisions
// once all qubits are placed. The first few layers dominate the result.
// For dense circuits this is fine; for sparse circuits (QAOA, random) some
// important long-range or rare interactions only appear late and are missed.
//
// Fix: reorder layers so that rare + distant pairs come first.
//
//   priority(q1,q2) = (1 + distanceWeight * |q1-q2|) / freq[q1,q2]
//
// After reordering, run the standard BuildChain logic.  The circuit order
// itself is NOT changed — only the input to the mapping step is reordered.
//
// @param sim             Configured MPSDummySimulator.
// @param layers          Circuit layers in original execution order.
// @param distanceWeight  Controls how much qubit-index distance biases
//                        the priority score (default 1.0).
// --------------------------------------------------------------------------
inline std::vector<long long int> ComputeLayerPriorityMapping(
    MPSDummySimulator&                                          sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>&   layers,
    double                                                      distanceWeight = 1.0)
{
    if (layers.empty()) return sim.ComputeOptimalQubitsMap(layers);

    const size_t N = sim.getNrQubits();
    const auto reordered = detail::ReorderLayersByPriority(layers, N, distanceWeight);
    return sim.ComputeOptimalQubitsMap(reordered);
}

// Greedy set-cover layer reordering: always pick the layer with the most
// new unique qubit pairs not yet seen in already-selected layers, then run
// the standard BuildChain on the reordered sequence.
inline std::vector<long long int> ComputeNewPairsFirstMapping(
    MPSDummySimulator&                                          sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>&   layers)
{
    if (layers.empty()) return sim.ComputeOptimalQubitsMap(layers);
    const size_t N = sim.getNrQubits();
    const auto reordered = detail::ReorderLayersByNewPairs(layers, N);
    return sim.ComputeOptimalQubitsMap(reordered);
}

// --------------------------------------------------------------------------
// FreqSeededChain
//
// Motivation
// ----------
// BuildChain processes pairs in layer order (FCFS). The first pair it sees
// seeds the first group, committing to an initial chain structure before
// it knows which pairs are globally important. For circuits where layer 0
// does not contain the most frequent interactions, this seed is suboptimal
// and all subsequent local merges inherit the error.
//
// This strategy feeds BuildChain's OWN merge logic (duplicated here so we
// do not touch MPSDummySimulator) but with pairs sorted by global frequency
// (descending). The most common pair in the entire circuit anchors the
// first group, so the chain backbone reflects the circuit's interaction
// structure rather than the accident of which gate appears first.
//
// After BuildChain finishes (once all qubits are placed), the resulting
// chain is passed to the standard evaluateCost path via
// ComputeOptimalQubitsMap, which also runs the identity-map and random-
// shuffle fallbacks -- so we get the best of the frequency-seeded chain
// and the existing random-search polish.
// --------------------------------------------------------------------------
inline std::vector<long long int> ComputeFreqSeededChain(
    MPSDummySimulator&                                          sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>&   layers)
{
    if (layers.empty()) return sim.ComputeOptimalQubitsMap(layers);

    const size_t N = static_cast<size_t>(sim.getNrQubits());

    // --- Step 1: Count global pair frequencies ---
    // freq[q1*N + q2] (q1 < q2) = number of times this pair appears across all layers.
    std::unordered_map<size_t, int> freq;
    for (const auto& layer : layers) {
        for (const auto& op : layer->GetOperations()) {
            auto qbits = op->AffectedQubits();
            if (qbits.size() >= 2) {
                size_t q1 = qbits[0], q2 = qbits[1];
                if (q1 >= N || q2 >= N || q1 == q2) continue;
                if (q1 > q2) std::swap(q1, q2);
                freq[q1 * N + q2]++;
            }
        }
    }

    if (freq.empty()) return sim.ComputeOptimalQubitsMap(layers);

    // --- Step 2: Sort unique pairs by frequency descending ---
    // Tie-break by (q2 - q1) ascending so tighter pairs win ties.
    struct Pair { size_t q1, q2; int count; };
    std::vector<Pair> pairs;
    pairs.reserve(freq.size());
    for (const auto& kv : freq)
        pairs.push_back({kv.first / N, kv.first % N, kv.second});
    std::stable_sort(pairs.begin(), pairs.end(),
        [](const Pair& a, const Pair& b) {
            if (a.count != b.count) return a.count > b.count;
            return (a.q2 - a.q1) < (b.q2 - b.q1);
        });

    // --- Step 3: Run BuildChain's merge logic on the frequency-sorted pairs ---
    // This is a self-contained re-implementation of buildChain from
    // MPSDummySimulator.h so we don't need to modify that file.
    std::vector<std::deque<size_t>> groups;
    std::vector<int>                qubitGroup(N, -1);
    size_t                          placedCount = 0;

    for (const auto& p : pairs) {
        const int g1 = qubitGroup[p.q1];
        const int g2 = qubitGroup[p.q2];

        if (g1 < 0 && g2 < 0) {
            // Neither placed: start a new group [q1, q2]
            const int gIdx = static_cast<int>(groups.size());
            groups.push_back({p.q1, p.q2});
            qubitGroup[p.q1] = gIdx;
            qubitGroup[p.q2] = gIdx;
            placedCount += 2;

        } else if (g1 >= 0 && g2 < 0) {
            // q1 placed, q2 not: attach q2 adjacent to q1
            auto& grp = groups[g1];
            // Find q1's position in the group
            size_t idxQ1 = 0;
            for (size_t i = 0; i < grp.size(); ++i)
                if (grp[i] == p.q1) { idxQ1 = i; break; }
            // Place q2 on the end nearest to q1 so they end up adjacent
            if (idxQ1 * 2 + 1 <= grp.size())   // q1 closer to front
                grp.push_front(p.q2);
            else                                // q1 closer to back
                grp.push_back(p.q2);
            qubitGroup[p.q2] = g1;
            ++placedCount;

        } else if (g1 < 0 && g2 >= 0) {
            // q2 placed, q1 not: attach q1 adjacent to q2
            auto& grp = groups[g2];
            size_t idxQ2 = 0;
            for (size_t i = 0; i < grp.size(); ++i)
                if (grp[i] == p.q2) { idxQ2 = i; break; }
            if (idxQ2 * 2 + 1 <= grp.size())   // q2 closer to front
                grp.push_front(p.q1);
            else
                grp.push_back(p.q1);
            qubitGroup[p.q1] = g2;
            ++placedCount;

        } else if (g1 != g2) {
            // Both placed in different groups: merge so q1 and q2 end up adjacent.
            auto& grpA = groups[g1];
            auto& grpB = groups[g2];
            size_t idxA = 0, idxB = 0;
            for (size_t i = 0; i < grpA.size(); ++i)
                if (grpA[i] == p.q1) { idxA = i; break; }
            for (size_t i = 0; i < grpB.size(); ++i)
                if (grpB[i] == p.q2) { idxB = i; break; }
            // Distance of q1↔q2 in orientation [A][B] vs [B][A]
            const size_t distAB = grpA.size() - 1 - idxA + idxB;  // q1 at back of A, q2 at front of B
            const size_t distBA = grpB.size() - 1 - idxB + idxA;  // q2 at back of B, q1 at front of A
            int mergedGroup;
            if (distAB <= distBA) {
                // Append B after A
                for (const auto q : grpB) grpA.push_back(q);
                grpB.clear();
                mergedGroup = g1;
            } else {
                // Append A after B
                for (const auto q : grpA) grpB.push_back(q);
                grpA.clear();
                mergedGroup = g2;
            }
            for (const auto q : groups[mergedGroup])
                qubitGroup[q] = mergedGroup;
        }
        // Both in same group: skip.

        if (placedCount == N) break;
    }

    // --- Step 4: Build the chain vector ---
    // Concatenate non-empty groups, then append isolated qubits.
    std::vector<long long int> chain;
    chain.reserve(N);
    for (const auto& grp : groups)
        for (const auto q : grp)
            chain.push_back(static_cast<long long int>(q));
    for (size_t q = 0; q < N; ++q)
        if (qubitGroup[q] < 0)
            chain.push_back(static_cast<long long int>(q));

    // chain[physPos] = logicalQubit  →  result[logicalQubit] = physPos
    std::vector<long long int> initMap(N);
    for (size_t i = 0; i < chain.size(); ++i)
        initMap[chain[i]] = static_cast<long long int>(i);

    // --- Step 5: Hand off to ComputeOptimalQubitsMap ---
    // This applies the standard identity-map comparison and random-shuffle
    // polish on top of our frequency-seeded initial map.  We set the initial
    // map by calling SetInitialQubitsMap before the normal evaluation path.
    // The simplest way: just return our map and let the caller use it; or
    // use evaluateCost via the normal public API which already does this.
    // Here we return the raw frequency-seeded map and let the benchmark
    // evaluate it directly (same as every other strategy).
    return initMap;
}

// --------------------------------------------------------------------------
// WeightedMergeChain
//
// Motivation
// ----------
// BuildChain's merge step (Case D: both qubits already placed in different
// groups) chooses orientation [A][B] vs [B][A] based solely on the distance
// between the single triggering pair (q1, q2).  All other pairs between
// qubits in grpA and qubits in grpB get zero vote, even if they are far
// more frequent.
//
// This strategy is identical to BuildChain in every other respect:
//   Case A: neither placed  → start new group [q1, q2]
//   Case B/C: one placed    → attach the other to the nearer end
//   Case D: both placed, different groups
//             → score EVERY cross-group pair (a in grpA, b in grpB)
//               weighted by global frequency, sum up expected routing
//               distances for both orientations, pick the lower one.
//
// The FCFS layer ordering is preserved exactly, so circuits like QFT
// where layer order is meaningful are not disturbed.
//
// Complexity of weighted merge step:
//   O(|grpA| * |grpB|) per merge  <=  O(N^2) total across all merges.
//   In practice <1 ms even for N=40.
// --------------------------------------------------------------------------
inline std::vector<long long int> ComputeWeightedMergeChain(
    MPSDummySimulator&                                          sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>&   layers)
{
    if (layers.empty()) return sim.ComputeOptimalQubitsMap(layers);

    const size_t N = static_cast<size_t>(sim.getNrQubits());

    // --- Step 1: Count global pair frequencies (q1 < q2) ---
    std::unordered_map<size_t, double> freq;
    for (const auto& layer : layers) {
        for (const auto& op : layer->GetOperations()) {
            auto qbits = op->AffectedQubits();
            if (qbits.size() >= 2) {
                size_t q1 = static_cast<size_t>(qbits[0]);
                size_t q2 = static_cast<size_t>(qbits[1]);
                if (q1 >= N || q2 >= N || q1 == q2) continue;
                if (q1 > q2) std::swap(q1, q2);
                freq[q1 * N + q2] += 1.0;
            }
        }
    }

    // Helper: look up frequency for an unordered pair
    auto pairFreq = [&](size_t a, size_t b) -> double {
        if (a > b) std::swap(a, b);
        auto it = freq.find(a * N + b);
        return (it != freq.end()) ? it->second : 0.0;
    };

    // --- Step 2: Collect 2-qubit pairs per layer (same as BuildChain) ---
    struct QPair { size_t q1, q2; };
    std::vector<std::vector<QPair>> layerPairs;
    for (const auto& layer : layers) {
        std::vector<QPair> lp;
        for (const auto& op : layer->GetOperations()) {
            auto qbits = op->AffectedQubits();
            if (qbits.size() >= 2) {
                size_t a = static_cast<size_t>(qbits[0]);
                size_t b = static_cast<size_t>(qbits[1]);
                if (a >= N || b >= N || a == b) continue;
                if (a > b) std::swap(a, b);
                lp.push_back({a, b});
            }
        }
        if (!lp.empty()) layerPairs.push_back(std::move(lp));
    }
    if (layerPairs.empty()) return sim.ComputeOptimalQubitsMap(layers);

    // --- Step 3: FCFS merge loop (BuildChain Cases A/B/C identical;
    //             Case D uses weighted global merge score) ---
    std::vector<std::deque<size_t>> groups;
    std::vector<int>                qubitGroup(N, -1);
    size_t                          placedCount = 0;

    for (const auto& lp : layerPairs) {
        for (const auto& p : lp) {
            const int g1 = qubitGroup[p.q1];
            const int g2 = qubitGroup[p.q2];

            if (g1 < 0 && g2 < 0) {
                // Case A: start new group [q1, q2]
                const int gIdx = static_cast<int>(groups.size());
                groups.push_back({p.q1, p.q2});
                qubitGroup[p.q1] = gIdx;
                qubitGroup[p.q2] = gIdx;
                placedCount += 2;

            } else if (g1 >= 0 && g2 < 0) {
                // Case B: q1 placed, q2 not — attach q2 to nearer end
                auto& grp = groups[g1];
                size_t idxQ1 = 0;
                for (size_t i = 0; i < grp.size(); ++i)
                    if (grp[i] == p.q1) { idxQ1 = i; break; }
                if (idxQ1 + 1 <= grp.size() - idxQ1)
                    grp.push_front(p.q2);
                else
                    grp.push_back(p.q2);
                qubitGroup[p.q2] = g1;
                ++placedCount;

            } else if (g1 < 0 && g2 >= 0) {
                // Case C: q2 placed, q1 not — attach q1 to nearer end
                auto& grp = groups[g2];
                size_t idxQ2 = 0;
                for (size_t i = 0; i < grp.size(); ++i)
                    if (grp[i] == p.q2) { idxQ2 = i; break; }
                if (idxQ2 + 1 <= grp.size() - idxQ2)
                    grp.push_front(p.q1);
                else
                    grp.push_back(p.q1);
                qubitGroup[p.q1] = g2;
                ++placedCount;

            } else if (g1 != g2) {
                // Case D: both placed in different groups.
                // Score every cross-group pair (a in grpA, b in grpB)
                // weighted by global frequency.  Sum expected MPS routing
                // distances for both orientations and pick the lower one.
                auto& grpA = groups[g1];
                auto& grpB = groups[g2];
                const size_t sA = grpA.size();
                const size_t sB = grpB.size();

                double scoreAB = 0.0;  // append B after A: chain = [...A...][...B...]
                double scoreBA = 0.0;  // append A after B: chain = [...B...][...A...]

                for (size_t ia = 0; ia < sA; ++ia) {
                    for (size_t ib = 0; ib < sB; ++ib) {
                        const double w = pairFreq(grpA[ia], grpB[ib]);
                        if (w == 0.0) continue;
                        // [A][B]: posA = ia, posB = sA + ib  => dist = sA - ia + ib
                        scoreAB += w * static_cast<double>(sA - ia + ib);
                        // [B][A]: posB = ib, posA = sB + ia  => dist = sB - ib + ia
                        scoreBA += w * static_cast<double>(sB - ib + ia);
                    }
                }

                // If no frequency data at all (zero-weight groups), fall back
                // to BuildChain's original distance formula for the trigger pair.
                if (scoreAB == 0.0 && scoreBA == 0.0) {
                    size_t idxA = 0, idxB = 0;
                    for (size_t i = 0; i < sA; ++i)
                        if (grpA[i] == p.q1) { idxA = i; break; }
                    for (size_t i = 0; i < sB; ++i)
                        if (grpB[i] == p.q2) { idxB = i; break; }
                    scoreAB = static_cast<double>(sA + idxB - idxA);
                    scoreBA = static_cast<double>(sB + idxA - idxB);
                }

                int mergedGroup;
                if (scoreAB <= scoreBA) {
                    for (const auto q : grpB) grpA.push_back(q);
                    grpB.clear();
                    mergedGroup = g1;
                } else {
                    for (const auto q : grpA) grpB.push_back(q);
                    grpA.clear();
                    mergedGroup = g2;
                }
                for (const auto q : groups[mergedGroup])
                    qubitGroup[q] = mergedGroup;
            }
            // Case E (same group): nothing to do.

            if (placedCount == N) break;
        }
        if (placedCount == N) break;
    }

    // --- Step 4: Concatenate groups → chain → mapping vector ---
    std::vector<long long int> chain;
    chain.reserve(N);
    for (const auto& grp : groups)
        for (const auto q : grp)
            chain.push_back(static_cast<long long int>(q));
    for (size_t q = 0; q < N; ++q)
        if (qubitGroup[q] < 0)
            chain.push_back(static_cast<long long int>(q));

    // chain[physPos] = logicalQubit  ->  result[logicalQubit] = physPos
    std::vector<long long int> result(N);
    for (size_t i = 0; i < chain.size(); ++i)
        result[chain[i]] = static_cast<long long int>(i);

    return result;
}

// --------------------------------------------------------------------------
// ComputeLookaheadGroupMapping
//
// Algorithm
// ---------
// Identical to BuildChain in structure (FCFS layer scan, same Cases A-E)
// but replaces the three arbitrary orientation decisions with a
// lookahead-scored choice:
//
//   Case B/C (one qubit unplaced):
//     Look at layers k+1..k+W for future gates of the unplaced qubit.
//     For each future partner already in the SAME group: compute exact
//     projected 1D distance under prepend vs append.
//     Pick the orientation with the lower total weighted distance.
//     Fallback to BuildChain's nearer-end rule if no informative signal.
//
//   Case D (both placed, different groups):
//     Look at layers k+1..k+W for future cross-group gates.
//     Sum weighted distances for both [A][B] and [B][A] orientations.
//     Fallback to BuildChain's single-pair formula if no signal.
//
// Why windowed instead of global frequency (WeightedMergeChain):
//   Global frequency causes QFT failure because qubit 0 has symmetric
//   global weight on both sides of every merge.  Windowed lookahead
//   gives LOCAL context: early decisions see the initial triangle
//   structure of QFT, naturally pushing q0 to an extreme end.
//
// Complexity: O(W * gates_per_layer * N) per layer -- microseconds.
// --------------------------------------------------------------------------
inline std::vector<long long int> ComputeLookaheadGroupMapping(
    MPSDummySimulator&                                          sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>&   layers,
    size_t                                                      W)  // lookahead window size
{
    if (layers.empty()) return sim.ComputeOptimalQubitsMap(layers);
    const size_t N = static_cast<size_t>(sim.getNrQubits());

    // --- Collect 2-qubit pairs per layer ---
    struct QPair { size_t q1, q2; };
    std::vector<std::vector<QPair>> layerPairs;
    for (const auto& layer : layers) {
        std::vector<QPair> lp;
        for (const auto& op : layer->GetOperations()) {
            auto qbits = op->AffectedQubits();
            if (qbits.size() >= 2) {
                size_t a = static_cast<size_t>(qbits[0]);
                size_t b = static_cast<size_t>(qbits[1]);
                if (a >= N || b >= N || a == b) continue;
                if (a > b) std::swap(a, b);
                lp.push_back({a, b});
            }
        }
        if (!lp.empty()) layerPairs.push_back(std::move(lp));
    }
    if (layerPairs.empty()) return sim.ComputeOptimalQubitsMap(layers);
    const size_t L = layerPairs.size();

    // --- Helper: build window frequency table for layers (k+1)..(k+W) ---
    // Returns: wfreq[q1*N + q2] = count of times this pair appears in window
    auto buildWindow = [&](size_t k) -> std::unordered_map<size_t, int> {
        std::unordered_map<size_t, int> wf;
        for (size_t w = 1; w <= W && k + w < L; ++w)
            for (const auto& p : layerPairs[k + w])
                wf[p.q1 * N + p.q2]++;
        return wf;
    };

    // --- Helper: look up (a,b) count in window map ---
    auto wfreqLookup = [&](const std::unordered_map<size_t, int>& wf,
                           size_t a, size_t b) -> int {
        if (a > b) std::swap(a, b);
        auto it = wf.find(a * N + b);
        return (it != wf.end()) ? it->second : 0;
    };

    // --- Helper: find position of qubit q in a deque (returns deque.size() if not found) ---
    auto findPos = [](const std::deque<size_t>& grp, size_t q) -> size_t {
        for (size_t i = 0; i < grp.size(); ++i)
            if (grp[i] == q) return i;
        return grp.size();
    };

    // --- FCFS merge loop ---
    std::vector<std::deque<size_t>> groups;
    std::vector<int>                qubitGroup(N, -1);
    size_t                          placedCount = 0;

    for (size_t k = 0; k < L; ++k) {
        const auto wf = buildWindow(k);  // lookahead window for this layer

        for (const auto& p : layerPairs[k]) {
            const int g1 = qubitGroup[p.q1];
            const int g2 = qubitGroup[p.q2];

            if (g1 < 0 && g2 < 0) {
                // ---- Case A: neither placed -- start new group [q1, q2] ----
                const int gIdx = static_cast<int>(groups.size());
                groups.push_back({p.q1, p.q2});
                qubitGroup[p.q1] = gIdx;
                qubitGroup[p.q2] = gIdx;
                placedCount += 2;

            } else if (g1 >= 0 && g2 < 0) {
                // ---- Case B: q1 placed, q2 not ----
                // Score prepend (q2 at pos 0) vs append (q2 at pos grp.size())
                // using future partners of q2 that are in the SAME group.
                auto& grp = groups[g1];

                double scorePrepend = 0.0, scoreAppend = 0.0;
                bool   hasSignal    = false;

                for (size_t qx = 0; qx < N; ++qx) {
                    if (qx == p.q2) continue;
                    const int count = wfreqLookup(wf, p.q2, qx);
                    if (count == 0) continue;
                    if (qubitGroup[qx] != g1) continue;  // only same-group partners are informative
                    hasSignal = true;
                    const size_t idxQx = findPos(grp, qx);
                    // prepend: q2 at 0, qx shifts to idxQx+1 => dist = idxQx+1
                    scorePrepend += count * static_cast<double>(idxQx + 1);
                    // append:  q2 at grp.size(), qx stays at idxQx => dist = grp.size() - idxQx
                    scoreAppend  += count * static_cast<double>(grp.size() - idxQx);
                }

                if (!hasSignal) {
                    // No informative window signal: fall back to BuildChain's nearer-end rule
                    const size_t idxQ1 = findPos(grp, p.q1);
                    if (idxQ1 + 1 <= grp.size() - idxQ1)
                        grp.push_front(p.q2);
                    else
                        grp.push_back(p.q2);
                } else if (scorePrepend <= scoreAppend) {
                    grp.push_front(p.q2);
                } else {
                    grp.push_back(p.q2);
                }
                qubitGroup[p.q2] = g1;
                ++placedCount;

            } else if (g1 < 0 && g2 >= 0) {
                // ---- Case C: q2 placed, q1 not (symmetric to B) ----
                auto& grp = groups[g2];

                double scorePrepend = 0.0, scoreAppend = 0.0;
                bool   hasSignal    = false;

                for (size_t qx = 0; qx < N; ++qx) {
                    if (qx == p.q1) continue;
                    const int count = wfreqLookup(wf, p.q1, qx);
                    if (count == 0) continue;
                    if (qubitGroup[qx] != g2) continue;
                    hasSignal = true;
                    const size_t idxQx = findPos(grp, qx);
                    scorePrepend += count * static_cast<double>(idxQx + 1);
                    scoreAppend  += count * static_cast<double>(grp.size() - idxQx);
                }

                if (!hasSignal) {
                    const size_t idxQ2 = findPos(grp, p.q2);
                    if (idxQ2 + 1 <= grp.size() - idxQ2)
                        grp.push_front(p.q1);
                    else
                        grp.push_back(p.q1);
                } else if (scorePrepend <= scoreAppend) {
                    grp.push_front(p.q1);
                } else {
                    grp.push_back(p.q1);
                }
                qubitGroup[p.q1] = g2;
                ++placedCount;

            } else if (g1 != g2) {
                // ---- Case D: both placed, different groups ----
                // Score [A][B] vs [B][A] using windowed cross-group pair frequencies.
                auto& grpA = groups[g1];
                auto& grpB = groups[g2];
                const size_t sA = grpA.size(), sB = grpB.size();

                double scoreAB  = 0.0, scoreBA = 0.0;
                bool   hasSignal = false;

                for (size_t ia = 0; ia < sA; ++ia) {
                    for (size_t ib = 0; ib < sB; ++ib) {
                        const int count = wfreqLookup(wf, grpA[ia], grpB[ib]);
                        if (count == 0) continue;
                        hasSignal = true;
                        // [A][B]: a at ia, b at sA+ib => dist = (sA-ia) + ib
                        scoreAB += count * static_cast<double>(sA - ia + ib);
                        // [B][A]: b at ib, a at sB+ia => dist = (sB-ib) + ia
                        scoreBA += count * static_cast<double>(sB - ib + ia);
                    }
                }

                if (!hasSignal) {
                    // Fallback: BuildChain's single-pair distance formula
                    const size_t idxA = findPos(grpA, p.q1);
                    const size_t idxB = findPos(grpB, p.q2);
                    scoreAB = static_cast<double>(sA + idxB - idxA);
                    scoreBA = static_cast<double>(sB + idxA - idxB);
                }

                int mergedGroup;
                if (scoreAB <= scoreBA) {
                    for (const auto q : grpB) grpA.push_back(q);
                    grpB.clear();
                    mergedGroup = g1;
                } else {
                    for (const auto q : grpA) grpB.push_back(q);
                    grpA.clear();
                    mergedGroup = g2;
                }
                for (const auto q : groups[mergedGroup])
                    qubitGroup[q] = mergedGroup;
            }
            // Case E (same group): nothing to do.

            if (placedCount == N) break;
        }
        if (placedCount == N) break;
    }

    // --- Concatenate groups into chain, append isolated qubits ---
    std::vector<long long int> chain;
    chain.reserve(N);
    for (const auto& grp : groups)
        for (const auto q : grp)
            chain.push_back(static_cast<long long int>(q));
    for (size_t q = 0; q < N; ++q)
        if (qubitGroup[q] < 0)
            chain.push_back(static_cast<long long int>(q));

    // chain[physPos] = logicalQubit  ->  result[logicalQubit] = physPos
    std::vector<long long int> result(N);
    for (size_t i = 0; i < chain.size(); ++i)
        result[chain[i]] = static_cast<long long int>(i);
    return result;
}

// --------------------------------------------------------------------------
// Strategy: RigidSeedLocalSearch (Two-Phase "Rigid Seed + MPS-Cost Local Search")
//
// Phase 1: Baseline Greedy Heuristic
//   Produces initial chain from BuildChain: pi = [q3, q1, q0, q2, q4, ...]
//   Guarantees 0 SWAPs for early pairs.
//   Identifies Layer-0 adjacent pairs as rigid 2-qubit blocks [u, v].
//   Non-early / uncoupled qubits form single-qubit blocks [w].
//
// Phase 2: Preservation-Constrained Search
//   Evaluates full MPS cost: C(pi) = sum_layers Cost(MPSDummySimulator)
//   Permits ONLY transformations that do NOT separate Layer-0 adjacent pairs:
//     - Block Swaps: Swap entire rigid groups [q3, q1] <-> [q2, q4]
//     - Group Inversions: Reverse sub-chains of blocks (2-opt) & internal block flips
//     - Peripheral Insertion: Move non-early qubits to opposite ends (0 or end)
//
// Why this beats global SA: Global SA destroys initial clusters. Constrained search
// keeps the zero-cost execution for initial layers intact while reshuffling the
// relative positions of entire multi-qubit blocks to minimize routing overhead later.
// --------------------------------------------------------------------------
inline std::vector<long long int> ComputeRigidSeedLocalSearch(
    MPSDummySimulator&                                         sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>&  layers,
    int                                                        maxIterations = 300,
    double                                                     T0 = 5.0,
    unsigned                                                   seed = 42)
{
    const size_t N = sim.getNrQubits();
    if (N <= 2 || layers.empty())
        return sim.ComputeOptimalQubitsMap(layers);

    const auto   costModel  = sim.getCostModel();
    const auto&  bdVec      = sim.getMaxBondDimensions();
    const MPSDummySimulator::IndexType maxBondDim =
        bdVec.empty() ? 64 : static_cast<MPSDummySimulator::IndexType>(bdVec[0]);

    // Phase 1: Baseline Greedy Heuristic (BuildChain)
    std::vector<long long int> initMap = sim.ComputeOptimalQubitsMap(layers);

    // Identify Layer-0 adjacent 2-qubit pairs
    std::vector<long long int> layer0Partner(N, -1);
    for (const auto& op : layers[0]->GetOperations()) {
        const auto qbits = op->AffectedQubits();
        if (qbits.size() == 2) {
            long long int q1 = static_cast<long long int>(qbits[0]);
            long long int q2 = static_cast<long long int>(qbits[1]);
            layer0Partner[q1] = q2;
            layer0Partner[q2] = q1;
        }
    }

    // Convert initMap (logical -> physical) to physical chain (physical -> logical)
    std::vector<long long int> chain(N);
    for (size_t q = 0; q < N; ++q)
        chain[initMap[q]] = static_cast<long long int>(q);

    // Form rigid blocks:
    // If chain[pos] and chain[pos+1] are Layer-0 partners, bundle them into a rigid 2-qubit block.
    // Otherwise, each qubit is a 1-qubit block.
    std::vector<std::vector<long long int>> blocks;
    std::vector<bool> visited(N, false);
    for (size_t pos = 0; pos < N; ++pos) {
        long long int q = chain[pos];
        if (visited[q]) continue;

        long long int p = layer0Partner[q];
        if (p >= 0 && !visited[p] && pos + 1 < N && chain[pos + 1] == p) {
            blocks.push_back({q, p});
            visited[q] = true;
            visited[p] = true;
        } else {
            blocks.push_back({q});
            visited[q] = true;
        }
    }

    const size_t numBlocks = blocks.size();
    if (numBlocks <= 1)
        return initMap;

    auto BlocksToMap = [&](const std::vector<std::vector<long long int>>& blks) {
        std::vector<long long int> mapping(N);
        long long int phys = 0;
        for (const auto& b : blks) {
            for (long long int q : b) {
                mapping[q] = phys++;
            }
        }
        return mapping;
    };

    // Phase 2: Preservation-Constrained Search
    double initCost = detail::EvaluateMappingCost(N, costModel, maxBondDim, initMap, layers, 0);
    double bestCost = initCost;
    auto bestBlocks = blocks;
    auto curBlocks  = blocks;
    double curCost  = initCost;

    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uniform01(0.0, 1.0);
    std::uniform_int_distribution<size_t> pickBlock(0, numBlocks - 1);

    const int totalIters = std::max(200, static_cast<int>(numBlocks) * 15);

    for (int iter = 0; iter < totalIters; ++iter) {
        const double frac = static_cast<double>(iter) / static_cast<double>(totalIters);
        const double T    = T0 * std::pow(0.005 / T0, frac);

        auto prevBlocks = curBlocks;

        // Perturbation operators:
        // 0: Block Swap (35%)
        // 1: Group Inversion / 2-opt sub-chain reversal (30%)
        // 2: Peripheral Insertion of non-early / singleton block (25%)
        // 3: Internal block flip for a rigid pair (10%)
        double r = uniform01(rng);
        if (r < 0.35) {
            // Move 0: Block Swap
            size_t b1 = pickBlock(rng);
            size_t b2 = pickBlock(rng);
            while (b2 == b1) b2 = pickBlock(rng);
            std::swap(curBlocks[b1], curBlocks[b2]);
        } else if (r < 0.65) {
            // Move 1: Group Inversion (2-opt sub-chain reversal)
            size_t b1 = pickBlock(rng);
            size_t b2 = pickBlock(rng);
            if (b1 > b2) std::swap(b1, b2);
            if (b1 == b2) {
                if (b2 + 1 < numBlocks) ++b2;
                else if (b1 > 0) --b1;
            }
            std::reverse(curBlocks.begin() + b1, curBlocks.begin() + b2 + 1);
        } else if (r < 0.90) {
            // Move 2: Peripheral Insertion (move a block to front or back)
            std::vector<size_t> singletons;
            for (size_t i = 0; i < numBlocks; ++i) {
                if (curBlocks[i].size() == 1) singletons.push_back(i);
            }
            size_t bIdx;
            if (!singletons.empty() && uniform01(rng) < 0.70) {
                std::uniform_int_distribution<size_t> pickS(0, singletons.size() - 1);
                bIdx = singletons[pickS(rng)];
            } else {
                bIdx = pickBlock(rng);
            }

            auto movingBlock = curBlocks[bIdx];
            curBlocks.erase(curBlocks.begin() + bIdx);
            if (uniform01(rng) < 0.5) {
                curBlocks.insert(curBlocks.begin(), std::move(movingBlock));
            } else {
                curBlocks.push_back(std::move(movingBlock));
            }
        } else {
            // Move 3: Internal Block Inversion (flip [u, v] -> [v, u])
            std::vector<size_t> pairBlocks;
            for (size_t i = 0; i < numBlocks; ++i) {
                if (curBlocks[i].size() == 2) pairBlocks.push_back(i);
            }
            if (!pairBlocks.empty()) {
                std::uniform_int_distribution<size_t> pickP(0, pairBlocks.size() - 1);
                size_t bIdx = pairBlocks[pickP(rng)];
                std::swap(curBlocks[bIdx][0], curBlocks[bIdx][1]);
            } else {
                size_t b1 = pickBlock(rng);
                size_t b2 = pickBlock(rng);
                while (b2 == b1) b2 = pickBlock(rng);
                std::swap(curBlocks[b1], curBlocks[b2]);
            }
        }

        // Evaluate full MPS Cost
        const auto candMap = BlocksToMap(curBlocks);
        const double newCost = detail::EvaluateMappingCost(
            N, costModel, maxBondDim, candMap, layers, 0);

        const double delta = newCost - curCost;

        bool accept = false;
        if (delta < 0.0) {
            accept = true;
            if (newCost < bestCost) {
                bestCost   = newCost;
                bestBlocks = curBlocks;
            }
        } else if (delta == 0.0) {
            accept = (uniform01(rng) < 0.30); // allow neutral plateau drift
        } else if (T > 1e-4) {
            const double prob = std::exp(-delta / T);
            if (uniform01(rng) < prob) accept = true;
        }

        if (accept) {
            curCost = newCost;
        } else {
            curBlocks = std::move(prevBlocks);
        }
    }

    // Phase 2b: Greedy Hill-Climbing Polish from bestBlocks
    curBlocks = bestBlocks;
    curCost   = bestCost;
    for (int iter = 0; iter < 40; ++iter) {
        auto prevBlocks = curBlocks;
        double r = uniform01(rng);
        if (r < 0.40) {
            size_t b1 = pickBlock(rng);
            size_t b2 = pickBlock(rng);
            while (b2 == b1) b2 = pickBlock(rng);
            std::swap(curBlocks[b1], curBlocks[b2]);
        } else if (r < 0.80) {
            size_t b1 = pickBlock(rng);
            size_t b2 = pickBlock(rng);
            if (b1 > b2) std::swap(b1, b2);
            if (b1 == b2) {
                if (b2 + 1 < numBlocks) ++b2;
                else if (b1 > 0) --b1;
            }
            std::reverse(curBlocks.begin() + b1, curBlocks.begin() + b2 + 1);
        } else {
            std::vector<size_t> pairBlocks;
            for (size_t i = 0; i < numBlocks; ++i)
                if (curBlocks[i].size() == 2) pairBlocks.push_back(i);
            if (!pairBlocks.empty()) {
                std::uniform_int_distribution<size_t> pickP(0, pairBlocks.size() - 1);
                size_t bIdx = pairBlocks[pickP(rng)];
                std::swap(curBlocks[bIdx][0], curBlocks[bIdx][1]);
            }
        }

        const auto candMap = BlocksToMap(curBlocks);
        const double newCost = detail::EvaluateMappingCost(
            N, costModel, maxBondDim, candMap, layers, 0);

        if (newCost < curCost) {
            curCost = newCost;
            if (curCost < bestCost) {
                bestCost   = curCost;
                bestBlocks = curBlocks;
            }
        } else {
            curBlocks = std::move(prevBlocks);
        }
    }

    // Safety guarantee: strictly return BuildChain if no improvement found
    if (bestCost < initCost) {
        return BlocksToMap(bestBlocks);
    } else {
        return initMap;
    }
}

// --------------------------------------------------------------------------
// Strategy: PeripheralPinning (Dynamic Barrier / Peripheral Pinning)
//
// Motivation: In MPS, virtual bond dimensions grow toward the chain centre
// (chi^1, chi^2, ..., chi^{N/2}) and are smallest at the boundaries.
// An "isolated" qubit (one that interacts only in the first few layers and
// then remains idle) placed in the interior acts as an internal barrier:
// every gate that must pass it costs chi^3 at the high-dimension interior
// bonds.  Placing it at the outermost position (0 or N-1) instead means
// those gates cross only low-dimension boundary bonds.
//
// Algorithm (pure BuildChain variant -- no changes to MPSDummySimulator.h):
//   1. Profile circuit: for each qubit compute
//        activeSpan  = last_layer_with_any_2q_gate - first_layer_with_any_2q_gate
//        totalGates  = total 2-qubit gate count
//      A qubit is "isolated" if activeSpan < idleThresholdFrac * L  AND
//                                totalGates <= earlyGateLimit
//   2. Run BuildChain FCFS logic with one modification:
//      Cases B/C (one qubit placed, one not):
//        - If the *incoming* qubit is isolated, attach it to the far end of
//          the *entire group* (the outermost available position).
//        - Otherwise, use BuildChain's nearest-end heuristic.
//   3. When all qubits are placed, check that all isolated qubits reside
//      at position 0 or N-1 of the concatenated chain.  If not, post-process
//      by swapping them to the nearest boundary (this handles the few cases
//      where an isolated qubit ends up sharing a group with hub qubits that
//      dominate the merge direction).
// --------------------------------------------------------------------------
inline std::vector<long long int> ComputePeripheralPinning(
    MPSDummySimulator&                                        sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>& layers,
    double idleThresholdFrac = 0.25,  // qubit is isolated if active span < 25% of total layers
    int    earlyGateLimit    = 4)     // qubit is isolated if it participates in at most N gates total
{
    using IndexType = MPSDummySimulator::IndexType;
    const IndexType N = static_cast<IndexType>(sim.getNrQubits());
    if (N <= 2 || layers.empty())
        return sim.ComputeOptimalQubitsMap(layers);

    const int L = static_cast<int>(layers.size());

    // --- Step 1: Profile each qubit ---
    std::vector<int> firstLayer(N, L);   // first layer where qubit has a 2q gate
    std::vector<int> lastLayer(N, -1);   // last  layer where qubit has a 2q gate
    std::vector<int> gateCount(N, 0);    // total 2q gate appearances

    for (int li = 0; li < L; ++li) {
        for (const auto& op : layers[li]->GetOperations()) {
            const auto qbits = op->AffectedQubits();
            if (qbits.size() < 2) continue;
            for (size_t qi = 0; qi < 2; ++qi) {
                const IndexType q = static_cast<IndexType>(qbits[qi]);
                if (q < 0 || q >= N) continue;
                gateCount[q]++;
                if (li < firstLayer[q]) firstLayer[q] = li;
                if (li > lastLayer[q])  lastLayer[q]  = li;
            }
        }
    }

    // Classify qubits
    const int spanThreshold = static_cast<int>(std::ceil(idleThresholdFrac * L));
    std::vector<bool> isIsolated(N, false);
    for (IndexType q = 0; q < N; ++q) {
        if (gateCount[q] == 0) {
            isIsolated[q] = true; // no gates at all -> pure spectator, pin to boundary
            continue;
        }
        const int span = lastLayer[q] - firstLayer[q];
        if (span < spanThreshold && gateCount[q] <= earlyGateLimit)
            isIsolated[q] = true;
    }

    // --- Step 2: Modified BuildChain ---
    // Collect 2-qubit pairs per layer (same as BuildChain)
    struct QubitPair { IndexType q1, q2; };
    std::vector<std::vector<QubitPair>> layerPairs;
    for (int li = 0; li < L; ++li) {
        std::vector<QubitPair> lp;
        for (const auto& op : layers[li]->GetOperations()) {
            auto qbits = op->AffectedQubits();
            if (qbits.size() >= 2) {
                IndexType a = static_cast<IndexType>(qbits[0]);
                IndexType b = static_cast<IndexType>(qbits[1]);
                if (a > b) std::swap(a, b);
                lp.push_back({a, b});
            }
        }
        if (!lp.empty()) layerPairs.push_back(std::move(lp));
    }
    if (layerPairs.empty()) return sim.ComputeOptimalQubitsMap(layers);

    std::vector<std::deque<IndexType>> groups;
    std::vector<int>  qubitGroup(N, -1);
    int placedCount = 0;

    auto attachToGroup = [&](int gIdx, IndexType qNew, IndexType qPartner, bool forceOuter) {
        auto& grp = groups[gIdx];
        if (forceOuter) {
            // Pin to the far end away from qPartner (i.e. the boundary)
            size_t idxPartner = 0;
            for (size_t i = 0; i < grp.size(); ++i)
                if (grp[i] == qPartner) { idxPartner = i; break; }
            // "far end" = the end that maximises distance from the partner position
            if (idxPartner <= grp.size() / 2)
                grp.push_back(qNew);    // partner is near front -> push to back
            else
                grp.push_front(qNew);   // partner is near back  -> push to front
        } else {
            // BuildChain nearest-end heuristic
            size_t idxPartner = 0;
            for (size_t i = 0; i < grp.size(); ++i)
                if (grp[i] == qPartner) { idxPartner = i; break; }
            if (idxPartner + 1 <= grp.size() - idxPartner)
                grp.push_front(qNew);
            else
                grp.push_back(qNew);
        }
        qubitGroup[qNew] = gIdx;
        ++placedCount;
    };

    for (const auto& lp : layerPairs) {
        for (const auto& p : lp) {
            const int g1 = qubitGroup[p.q1];
            const int g2 = qubitGroup[p.q2];

            if (g1 < 0 && g2 < 0) {
                // Case A: new pair
                const int gIdx = static_cast<int>(groups.size());
                // If one is isolated, put it on the outside
                if (isIsolated[p.q1] && !isIsolated[p.q2])
                    groups.push_back({p.q2, p.q1});  // hub first, isolated at back
                else if (isIsolated[p.q2] && !isIsolated[p.q1])
                    groups.push_back({p.q1, p.q2});  // hub first, isolated at back
                else
                    groups.push_back({p.q1, p.q2});  // both same class: normal order
                qubitGroup[p.q1] = gIdx;
                qubitGroup[p.q2] = gIdx;
                placedCount += 2;
            } else if (g1 >= 0 && g2 < 0) {
                // Case B: q1 placed, q2 not
                attachToGroup(g1, p.q2, p.q1, isIsolated[p.q2]);
            } else if (g1 < 0 && g2 >= 0) {
                // Case C: q2 placed, q1 not
                attachToGroup(g2, p.q1, p.q2, isIsolated[p.q1]);
            } else if (g1 != g2) {
                // Case D: merge two groups (same as BuildChain: minimise distance of triggering pair)
                auto& grpA = groups[g1];
                auto& grpB = groups[g2];
                size_t idxA = 0, idxB = 0;
                for (size_t i = 0; i < grpA.size(); ++i)
                    if (grpA[i] == p.q1) { idxA = i; break; }
                for (size_t i = 0; i < grpB.size(); ++i)
                    if (grpB[i] == p.q2) { idxB = i; break; }
                const size_t distAB = grpA.size() + idxB - idxA;
                const size_t distBA = grpB.size() + idxA - idxB;
                int mergedGroup;
                if (distAB <= distBA) {
                    for (const auto q : grpB) grpA.push_back(q);
                    grpB.clear();
                    mergedGroup = g1;
                } else {
                    for (const auto q : grpA) grpB.push_back(q);
                    grpA.clear();
                    mergedGroup = g2;
                }
                for (const auto q : groups[mergedGroup])
                    qubitGroup[q] = mergedGroup;
            }
            // Case E: same group, nothing to do

            if (placedCount == static_cast<int>(N)) break;
        }
        if (placedCount == static_cast<int>(N)) break;
    }

    // Concatenate non-empty groups
    std::vector<IndexType> chain;
    chain.reserve(N);
    for (const auto& grp : groups)
        for (const auto q : grp)
            chain.push_back(q);
    // Spectator qubits (no 2q gates) go to the very end (boundary)
    for (IndexType q = 0; q < N; ++q)
        if (qubitGroup[q] < 0)
            chain.push_back(q);

    // --- Step 3: Post-process boundary guarantee for isolated qubits ---
    // If any isolated qubit ended up sandwiched between hub qubits, swap it
    // to the nearest boundary slot that is currently occupied by a non-isolated qubit.
    // We do at most one boundary-swap per isolated interior qubit to avoid cascade.
    auto swapChain = [&](size_t i, size_t j) { std::swap(chain[i], chain[j]); };

    // Collect boundary slots in order of preference (outermost first)
    // and identify interior isolated qubits
    for (size_t pos = 1; pos + 1 < chain.size(); ++pos) {
        if (!isIsolated[chain[pos]]) continue;
        // This isolated qubit is interior -- move to nearest non-isolated boundary slot
        // Try position 0 first
        if (!isIsolated[chain[0]]) {
            swapChain(pos, 0);
        } else if (!isIsolated[chain[chain.size() - 1]]) {
            swapChain(pos, chain.size() - 1);
        }
        // If both boundaries are already isolated, leave as-is (they're already cheap)
    }

    // chain[physPos] = logicalQubit  ->  result[logicalQubit] = physPos
    std::vector<long long int> result(N);
    for (size_t i = 0; i < chain.size(); ++i)
        result[chain[i]] = static_cast<long long int>(i);
    return result;
}

// --- Public dispatchers ---

// Compute initial mapping and return detailed result (including community info).
// temporalAlpha is only used for TemporalCommunityMapping.
inline MappingResult ComputeMappingWithInfo(
    MPSDummySimulator&                                        sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>& layers,
    InitialMappingStrategy                                    strategy,
    double                                                    temporalAlpha = 2.0)
{
    MappingResult res;
    switch (strategy) {
        case InitialMappingStrategy::BuildChain:
            res.map            = sim.ComputeOptimalQubitsMap(layers);
            res.numCommunities = 1;
            break;

        case InitialMappingStrategy::CommunityMapping:
            res.map = ComputeCommunityMapping(sim, layers, &res.communityGroups);
            res.numCommunities = static_cast<int>(res.communityGroups.size());
            break;

        case InitialMappingStrategy::TemporalCommunityMapping:
            res.map = ComputeTemporalCommunityMapping(
                sim, layers, temporalAlpha, &res.communityGroups);
            res.numCommunities = static_cast<int>(res.communityGroups.size());
            break;

        case InitialMappingStrategy::BondAwarePlacement:
            res.map            = ComputeBondAwarePlacement(sim, layers);
            res.numCommunities = 1;
            break;

        case InitialMappingStrategy::LayerPriorityMapping:
            res.map            = ComputeLayerPriorityMapping(sim, layers);
            res.numCommunities = 1;
            break;

        case InitialMappingStrategy::NewPairsFirstMapping:
            res.map            = ComputeNewPairsFirstMapping(sim, layers);
            res.numCommunities = 1;
            break;

        case InitialMappingStrategy::FreqSeededChain:
            res.map            = ComputeFreqSeededChain(sim, layers);
            res.numCommunities = 1;
            break;

        case InitialMappingStrategy::WeightedMergeChain:
            res.map            = ComputeWeightedMergeChain(sim, layers);
            res.numCommunities = 1;
            break;

        case InitialMappingStrategy::LookaheadGroupW3:
            res.map            = ComputeLookaheadGroupMapping(sim, layers, 3);
            res.numCommunities = 1;
            break;

        case InitialMappingStrategy::LookaheadGroupW5:
            res.map            = ComputeLookaheadGroupMapping(sim, layers, 5);
            res.numCommunities = 1;
            break;

        case InitialMappingStrategy::RigidSeedLocalSearch:
            res.map            = ComputeRigidSeedLocalSearch(sim, layers);
            res.numCommunities = 1;
            break;

        case InitialMappingStrategy::PeripheralPinning:
            res.map            = ComputePeripheralPinning(sim, layers);
            res.numCommunities = 1;
            break;
    }
    return res;
}

// Simplified dispatcher: returns only the mapping vector.
inline std::vector<long long int> ComputeInitialMapping(
    MPSDummySimulator&                                        sim,
    const std::vector<std::shared_ptr<Circuits::Circuit<>>>& layers,
    InitialMappingStrategy                                    strategy,
    double                                                    temporalAlpha = 2.0)
{
    switch (strategy) {
        case InitialMappingStrategy::BuildChain:
            return sim.ComputeOptimalQubitsMap(layers);
        case InitialMappingStrategy::CommunityMapping:
            return ComputeCommunityMapping(sim, layers);
        case InitialMappingStrategy::TemporalCommunityMapping:
            return ComputeTemporalCommunityMapping(sim, layers, temporalAlpha);
        case InitialMappingStrategy::BondAwarePlacement:
            return ComputeBondAwarePlacement(sim, layers);
        case InitialMappingStrategy::LayerPriorityMapping:
            return ComputeLayerPriorityMapping(sim, layers);
        case InitialMappingStrategy::NewPairsFirstMapping:
            return ComputeNewPairsFirstMapping(sim, layers);
        case InitialMappingStrategy::FreqSeededChain:
            return ComputeFreqSeededChain(sim, layers);
        case InitialMappingStrategy::WeightedMergeChain:
            return ComputeWeightedMergeChain(sim, layers);
        case InitialMappingStrategy::LookaheadGroupW3:
            return ComputeLookaheadGroupMapping(sim, layers, 3);
        case InitialMappingStrategy::LookaheadGroupW5:
            return ComputeLookaheadGroupMapping(sim, layers, 5);
        case InitialMappingStrategy::RigidSeedLocalSearch:
            return ComputeRigidSeedLocalSearch(sim, layers);
        case InitialMappingStrategy::PeripheralPinning:
            return ComputePeripheralPinning(sim, layers);
    }
    return sim.ComputeOptimalQubitsMap(layers);  // unreachable
}

}  // namespace Simulators

#endif  // _INITIALMAPPING_H_
