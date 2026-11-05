// MPSDummySimulator.h
//
// Tracks qubit positions and bond dimensions through a circuit without doing
// any real simulation. Used to estimate MPS SWAP costs for initial mapping.

#pragma once

#ifndef _MPSDUMMYSIMULATOR_H_
#define _MPSDUMMYSIMULATOR_H_

#include <algorithm>
#include <deque>
#include <random>
#include <unordered_set>
#include <numeric>

#include <Eigen/Eigen>
#include <unsupported/Eigen/CXX11/Tensor>

#include "QuantumGate.h"
#include "MPSSimulatorInterface.h"
#include "../Circuit/Circuit.h"

namespace Simulators {

class MPSDummySimulator {
 public:
  using IndexType = long long int;
  using MatrixClass = QC::TensorNetworks::MPSSimulatorInterface::MatrixClass;
  using GateClass = QC::TensorNetworks::MPSSimulatorInterface::GateClass;

  enum class CostModel {
    CentralBondCubic,
    ThinSVDGeometry
  };

  MPSDummySimulator(size_t N) : nrQubits(N), maxVirtualExtent(0) {
    InitQubitsMap();

    SetMaxBondDimension(0);
  }

  std::unique_ptr<MPSDummySimulator> Clone() const {
    auto clone = std::unique_ptr<MPSDummySimulator>(
        new MPSDummySimulator(nrQubits, LightweightInitTag{}));
    clone->qubitsMap = qubitsMap;
    clone->qubitsMapInv = qubitsMapInv;
    clone->maxVirtualExtent = maxVirtualExtent;
    clone->costModel = costModel;
    clone->bondCost = bondCost;
    clone->maxBondDim = maxBondDim;
    clone->currentBondDim = currentBondDim;

    clone->totalSwappingCost = totalSwappingCost;

    clone->growthFactorSwap = growthFactorSwap;
    clone->growthFactorGate = growthFactorGate;

    return clone;
  }

  CostModel getCostModel() const { return costModel; }

  void setCostModel(CostModel model) {
    costModel = model;
    recomputeAllBondCosts();
  }

  double computeBondCost(IndexType bond) const {
    if (bond < 0 || bond >= static_cast<IndexType>(currentBondDim.size()))
      return 0.0;

    if (costModel == CostModel::ThinSVDGeometry) {
      const IndexType leftBond = bond - 1;
      const IndexType rightBond = bond + 1;

      const double chiL = (leftBond >= 0) ? currentBondDim[leftBond] : 1.0;
      const double chiR = (rightBond < static_cast<IndexType>(currentBondDim.size()))
                              ? currentBondDim[rightBond]
                              : 1.0;

      return chiL * chiR * std::min(chiL, chiR);
    } else {
      const double chiC = currentBondDim[bond];
      return chiC * chiC * chiC;
    }
  }

  void updateAffectedBondCosts(IndexType changedBond) {
    const IndexType minB = std::max<IndexType>(0, changedBond - 1);
    const IndexType maxB = std::min<IndexType>(
        static_cast<IndexType>(currentBondDim.size()) - 1, changedBond + 1);

    for (IndexType b = minB; b <= maxB; ++b) {
      bondCost[b] = computeBondCost(b);
    }
  }

  void recomputeAllBondCosts() {
    for (size_t b = 0; b < currentBondDim.size(); ++b) {
      bondCost[b] = computeBondCost(static_cast<IndexType>(b));
    }
  }

  size_t getNrQubits() const { return nrQubits; }

  double getGrowthFactorSwap() const { return growthFactorSwap; }
  double getGrowthFactorGate() const { return growthFactorGate; }

  void setGrowthFactorSwap(double factor) { growthFactorSwap = factor; }
  void setGrowthFactorGate(double factor) { growthFactorGate = factor; }

  void Clear() { InitQubitsMap(); }

  void SetMaxBondDimension(IndexType val) {
    maxVirtualExtent = val;

    if (nrQubits == 0) return;

    const double untruncatedMaxExtent = std::pow(physExtent, nrQubits / 2);
    IndexType maxVirtualExtentLimit =
        static_cast<IndexType>(untruncatedMaxExtent);

    if (untruncatedMaxExtent >= std::numeric_limits<IndexType>::max() ||
        std::isnan(untruncatedMaxExtent) || std::isinf(untruncatedMaxExtent))
      maxVirtualExtentLimit = std::numeric_limits<IndexType>::max() - 1;
    else if (untruncatedMaxExtent < 2)
      maxVirtualExtentLimit = 2;

    maxVirtualExtent = maxVirtualExtent == 0
                           ? maxVirtualExtentLimit
                           : std::min(maxVirtualExtent, maxVirtualExtentLimit);

    bondCost.resize(nrQubits - 1);
    maxBondDim.resize(nrQubits - 1);
    currentBondDim.assign(nrQubits - 1, 1.0);

    // the checks here are overkill, but better safe than sorry
    // we're dealing with large values here, overflows are to be expected
    for (IndexType i = 0; i < static_cast<IndexType>(nrQubits); ++i) {
      double maxExtent1 = std::pow((double)physExtent, (double)i + 1.);
      double maxExtent2 =
          std::pow((double)physExtent, (double)nrQubits - i - 1.);

      if (maxExtent1 >= (double)std::numeric_limits<size_t>::max() ||
          std::isnan(maxExtent1) || std::isinf(maxExtent1))
        maxExtent1 = (double)std::numeric_limits<size_t>::max() - 1;
      else if (maxExtent1 < 1)
        maxExtent1 = 1;

      if (maxExtent2 > (double)std::numeric_limits<size_t>::max() ||
          std::isnan(maxExtent2) || std::isinf(maxExtent2))
        maxExtent2 = (double)std::numeric_limits<size_t>::max() - 1;
      else if (maxExtent2 < 1)
        maxExtent2 = 1;

      size_t maxRightExtent = (size_t)std::min<double>(
          {maxExtent1, maxExtent2, (double)maxVirtualExtent});
      if (maxRightExtent < 2) maxRightExtent = 2;

      if (i < static_cast<IndexType>(nrQubits) - 1) {
        maxBondDim[i] = static_cast<double>(maxRightExtent);
      }
    }
    recomputeAllBondCosts();
  }

  void print() const {
    std::cout << "Qubits map: ";
    for (int q = 0; q < static_cast<int>(qubitsMap.size()); ++q)
      std::cout << q << "->" << qubitsMap[q] << " ";
    std::cout << std::endl;
  }

  void ApplyGate(const QC::Gates::AppliedGate<MatrixClass>& gate) {
    ApplyGate(gate, gate.getQubit1(), gate.getQubit2());
  }

  void ApplyGate(const std::shared_ptr<Circuits::IOperation<>>& gate) {
    const auto qbits = gate->AffectedQubits();

    if (qbits.size() == 1) {
      // 1-qubit gates don't affect swap order
      return;
    } else if (qbits.size() == 2) {
      // static dummy gate avoids a heap allocation; only getQubitsNumber() is checked
      static const QC::Gates::AppliedGate<MatrixClass> dummy2qGate(
          MatrixClass::Identity(4, 4), 0, 1);
      ApplyGate(dummy2qGate, qbits[0], qbits[1]);
    } else  if (qbits.size() == 3 && gate->GetType() == Circuits::OperationType::kGate) {
      static const QC::Gates::AppliedGate<MatrixClass> dummy2qGate(
          MatrixClass::Identity(4, 4), 0, 1);

      const auto gateptr =
          std::static_pointer_cast<Circuits::IQuantumGate<>>(gate);

      const size_t q1 = qbits[0];  // control 1
      const size_t q2 = qbits[1];  // control 2
      const size_t q3 = qbits[2];  // target
      if (gateptr->GetGateType() == Circuits::QuantumGateType::kCCXGateType) {
        ApplyGate(dummy2qGate, q3, q2);
        ApplyGate(dummy2qGate, q2, q1);
        ApplyGate(dummy2qGate, q3, q2);
        ApplyGate(dummy2qGate, q2, q1);
        ApplyGate(dummy2qGate, q3, q1);
      } else { // cswap
        ApplyGate(dummy2qGate, q2, q3);
        ApplyGate(dummy2qGate, q3, q2);
        ApplyGate(dummy2qGate, q2, q1);
        ApplyGate(dummy2qGate, q3, q2);
        ApplyGate(dummy2qGate, q2, q1);
        ApplyGate(dummy2qGate, q3, q1);
        ApplyGate(dummy2qGate, q2, q3);
      }
    }
  }

  void ApplyGate(const GateClass& gate, IndexType qubit,
                 IndexType controllingQubit1 = 0) {
    if (qubit < 0 || qubit >= static_cast<IndexType>(getNrQubits()))
      throw std::invalid_argument("Qubit index out of bounds");
    else if (controllingQubit1 < 0 ||
             controllingQubit1 >= static_cast<IndexType>(getNrQubits()))
      throw std::invalid_argument("Qubit index out of bounds");

    // for two-qubit gates: swap until adjacent, then apply
    if (gate.getQubitsNumber() > 1) {
      IndexType qubit1 = qubitsMap[qubit];
      IndexType qubit2 = qubitsMap[controllingQubit1];

      if (abs(qubit1 - qubit2) > 1) {
        SwapQubits(qubit, controllingQubit1);
        qubit1 = qubitsMap[qubit];
        qubit2 = qubitsMap[controllingQubit1];
        assert(abs(qubit1 - qubit2) == 1);
      }

      // charge cost before growing the bond; controlled gates have Schmidt rank 2,
      // uncontrolled assumed rank 4 (TODO: not true for all gates)
      const IndexType bond = std::min(qubit1, qubit2);
      totalSwappingCost += bondCost[bond];
      growBondDimension(bond, false, gate.isControlled() ? 2 : 4);
    }
  }

  void ApplyGates(
      const std::vector<QC::Gates::AppliedGate<MatrixClass>>& gates) {
    for (const auto& gate : gates) ApplyGate(gate);
  }

  void ApplyGates(
      const std::vector<std::shared_ptr<Circuits::IOperation<>>>& gates) {
    for (const auto& gate : gates) ApplyGate(gate);
  }

  void SetInitialQubitsMap(const std::vector<long long int>& initialMap) {
    qubitsMap = initialMap;
    for (size_t i = 0; i < initialMap.size(); ++i)
      qubitsMapInv[initialMap[i]] = i;

    totalSwappingCost = 0;
    std::fill(currentBondDim.begin(), currentBondDim.end(), 1.0);
    recomputeAllBondCosts();
  }

  void SetCurrentBondDimensions(const std::vector<double>& dims) {
    assert(dims.size() == currentBondDim.size());
    for (size_t i = 0; i < dims.size(); ++i)
      currentBondDim[i] = std::min(dims[i], maxBondDim[i]);

    recomputeAllBondCosts();
  }

  const std::vector<double>& getCurrentBondDimensions() const {
    return currentBondDim;
  }

  const std::vector<double>& getMaxBondDimensions() const { return maxBondDim; }

  const std::vector<IndexType>& getQubitsMap() const { return qubitsMap; }

  const std::vector<IndexType>& getQubitsMapInv() const { return qubitsMapInv; }

  void setTotalSwappingCost(double cost) { totalSwappingCost = cost; }
  double getTotalSwappingCost() const { return totalSwappingCost; }

  // Cost of meeting at meetPosition: applies the current gate then evaluates
  // lookaheadDepth more gates from upcomingGates.
  void EvaluateMeetingPositionCost(
      IndexType meetPosition,
      const std::vector<std::shared_ptr<Circuits::IOperation<>>>& upcomingGates,
      long long int currentGateIndex, int lookaheadDepth,
      int lookaheadDepthWithHeuristic, double currentCost, double& bestCost,
      bool useSameDummy = false) {
    if (currentGateIndex >= static_cast<long long int>(upcomingGates.size())) {
      if (currentCost < bestCost) bestCost = currentCost;
      return;
    }

    // advance past any 1-qubit gates to the next 2-qubit gate
    while (currentGateIndex <
               static_cast<long long int>(upcomingGates.size()) &&
           upcomingGates[currentGateIndex]->AffectedQubits().size() < 2)
      ++currentGateIndex;

    if (currentGateIndex >= static_cast<long long int>(upcomingGates.size())) {
      if (currentCost < bestCost) bestCost = currentCost;
      return;
    }

    const auto& op = upcomingGates[currentGateIndex];
    const auto qbits = op->AffectedQubits();

    assert(qbits.size() >= 2);

    const IndexType qubit1 = static_cast<IndexType>(qbits[0]);
    const IndexType qubit2 = static_cast<IndexType>(qbits[1]);

    // move qubits to meeting position and accumulate cost

    IndexType realq1 = qubitsMap[qubit1];
    IndexType realq2 = qubitsMap[qubit2];
    if (realq1 > realq2) std::swap(realq1, realq2);

    if (useSameDummy) {
      const double ccost = getTotalSwappingCost();

      if (realq2 - realq1 > 1)
        SwapQubitsToPosition(qubit1, qubit2, meetPosition);

      ApplyGate(op);

      // const double importanceFactor =
      //     pow(dimFactor, (lookaheadDepthWithHeuristic - lookaheadDepth - 1));
      currentCost += (getTotalSwappingCost() - ccost) /** importanceFactor*/;

      if (currentCost >= bestCost) return;
      // No more lookahead depth: return without further recursion
      if (lookaheadDepth <= 0) {
        if (currentCost < bestCost) bestCost = currentCost;
        return;
      }

      // advance to the next 2-qubit gate
      ++currentGateIndex;
      while (currentGateIndex <
                 static_cast<long long int>(upcomingGates.size()) &&
             upcomingGates[currentGateIndex]->AffectedQubits().size() < 2)
        ++currentGateIndex;

      if (currentGateIndex >=
          static_cast<long long int>(upcomingGates.size())) {
        if (currentCost < bestCost) bestCost = currentCost;
        return;
      }

      FindBestMeetingPosition(upcomingGates, currentGateIndex,
                              lookaheadDepth - 1, lookaheadDepthWithHeuristic,
                              currentCost, bestCost);
    } else {
      MPSDummySimulator dummySim(nrQubits, LightweightInitTag{});
      dummySim.costModel = costModel;
      dummySim.maxVirtualExtent = maxVirtualExtent;
      dummySim.maxBondDim = maxBondDim;
      dummySim.currentBondDim = currentBondDim;
      dummySim.bondCost = bondCost;
      dummySim.qubitsMap = qubitsMap;
      dummySim.qubitsMapInv.resize(qubitsMap.size());
      for (size_t i = 0; i < qubitsMap.size(); ++i)
        dummySim.qubitsMapInv[qubitsMap[i]] = static_cast<IndexType>(i);

      dummySim.setTotalSwappingCost(0);

      if (realq2 - realq1 > 1)
        dummySim.SwapQubitsToPosition(qubit1, qubit2, meetPosition);

      dummySim.ApplyGate(op);  // also updates bond dimensions

      currentCost += dummySim.getTotalSwappingCost();

      // prune if already worse than best
      if (currentCost >= bestCost) return;

      // no lookahead left
      if (lookaheadDepth <= 0) {
        if (currentCost < bestCost) bestCost = currentCost;
        return;
      }

      // advance to the next 2-qubit gate
      ++currentGateIndex;
      while (currentGateIndex <
                 static_cast<long long int>(upcomingGates.size()) &&
             upcomingGates[currentGateIndex]->AffectedQubits().size() < 2)
        ++currentGateIndex;

      if (currentGateIndex >=
          static_cast<long long int>(upcomingGates.size())) {
        if (currentCost < bestCost) bestCost = currentCost;
        return;
      }

      dummySim.FindBestMeetingPosition(
          upcomingGates, currentGateIndex, lookaheadDepth - 1,
          lookaheadDepthWithHeuristic, currentCost, bestCost);
    }
  }

  // Find the bond position that minimises cost for the current swap + lookahead.
  // Returns the best bond index; bestCost is updated in place.
  IndexType FindBestMeetingPosition(
      const std::vector<std::shared_ptr<Circuits::IOperation<>>>& upcomingGates,
      long long int currentGateIndex, int lookaheadDepth,
      int lookaheadDepthWithHeuristic, double currentCost, double& bestCost) {
    const auto& op = upcomingGates[currentGateIndex];
    const auto qbits = op->AffectedQubits();

    assert(qbits.size() >= 2);

    const IndexType qubit1 = static_cast<IndexType>(qbits[0]);
    const IndexType qubit2 = static_cast<IndexType>(qbits[1]);

    IndexType realq1 = qubitsMap[qubit1];
    IndexType realq2 = qubitsMap[qubit2];

    if (realq1 > realq2) std::swap(realq1, realq2);

    if (lookaheadDepth <= 0) {
      IndexType pos = (realq2 - realq1 > 1)
                          ? ComputeHeuristicMeetPosition(realq1, realq2)
                          : realq1;
      EvaluateMeetingPositionCost(pos, upcomingGates, currentGateIndex, 0,
                                  lookaheadDepthWithHeuristic, currentCost,
                                  bestCost, false);
      return pos;
    }

    if (realq2 - realq1 <= 1) {
      EvaluateMeetingPositionCost(
          realq1, upcomingGates, currentGateIndex, lookaheadDepth,
          lookaheadDepthWithHeuristic, currentCost, bestCost,
          lookaheadDepth <= lookaheadDepthWithHeuristic);

      return realq1;
    }

    IndexType bestPosition = realq1;

    if (lookaheadDepth <= lookaheadDepthWithHeuristic) {
      bestPosition = ComputeHeuristicMeetPosition(realq1, realq2);
      EvaluateMeetingPositionCost(bestPosition, upcomingGates, currentGateIndex,
                                  lookaheadDepth, lookaheadDepthWithHeuristic,
                                  currentCost, bestCost, true);
    } else {
      for (IndexType m = realq1; m < realq2; ++m) {
        const double oldCost = bestCost;
        EvaluateMeetingPositionCost(m, upcomingGates, currentGateIndex,
                                    lookaheadDepth, lookaheadDepthWithHeuristic,
                                    currentCost, bestCost);

        if (bestCost < oldCost) bestPosition = m;
      }
    }

    return bestPosition;
  }

  std::vector<long long int> ComputeOptimalQubitsMap(
      const std::vector<std::shared_ptr<Circuits::Circuit<>>>& layers,
      int nrShuffles = 0, int nrSwaps = 0) {
    const IndexType nrQubits = getNrQubits();

    if (layers.empty() || nrQubits <= 2) return qubitsMap;

    
    auto evaluateCost =
        [&, this](const std::vector<IndexType>& candidateMap) -> double {
      auto saveQubitsMap = qubitsMap;
      auto saveQubitsMapInv = qubitsMapInv;
      auto saveCurrentBondDim = currentBondDim;
      auto saveTotalSwappingCost = totalSwappingCost;
      auto saveBondCost = bondCost;

      SetInitialQubitsMap(candidateMap);

      for (const auto& layer : layers) 
          ApplyGates(layer->GetOperations());
      auto cost = getTotalSwappingCost();
      qubitsMap = std::move(saveQubitsMap);
      qubitsMapInv = std::move(saveQubitsMapInv);
      currentBondDim = std::move(saveCurrentBondDim);
      totalSwappingCost = saveTotalSwappingCost;
      bondCost = std::move(saveBondCost);

      return cost;
    };

    auto evaluateCostBounded = [&, this](
                                   const std::vector<IndexType>& candidateMap,
                                   double bound) -> double {
      auto saveQubitsMap = qubitsMap;
      auto saveQubitsMapInv = qubitsMapInv;
      auto saveCurrentBondDim = currentBondDim;
      auto saveTotalSwappingCost = totalSwappingCost;
      auto saveBondCost = bondCost;

      SetInitialQubitsMap(candidateMap);

      for (const auto& layer : layers) {
        ApplyGates(layer->GetOperations());
        auto cost = getTotalSwappingCost();
        if (cost >= bound) {
          // restore state
          qubitsMap = std::move(saveQubitsMap);
          qubitsMapInv = std::move(saveQubitsMapInv);
          currentBondDim = std::move(saveCurrentBondDim);
          totalSwappingCost = saveTotalSwappingCost;
          bondCost = std::move(saveBondCost);
          return cost;
        }
      }
      auto cost = getTotalSwappingCost();
      // restore state
      qubitsMap = std::move(saveQubitsMap);
      qubitsMapInv = std::move(saveQubitsMapInv);
      currentBondDim = std::move(saveCurrentBondDim);
      totalSwappingCost = saveTotalSwappingCost;
      bondCost = std::move(saveBondCost);

      return cost;
    };
    

    // collect 2-qubit pairs per layer
    struct QubitPair {
      IndexType q1, q2;
    };
    std::vector<std::vector<QubitPair>> layerPairs;

    for (size_t li = 0; li < layers.size(); ++li) {
      const auto& layer = layers[li];
      std::vector<QubitPair> lp;
      for (const auto& op : layer->GetOperations()) {
        auto qbits = op->AffectedQubits();
        if (qbits.size() >= 2) {
          if (qbits[0] > qbits[1]) std::swap(qbits[0], qbits[1]);

          lp.push_back({static_cast<IndexType>(qbits[0]),
                        static_cast<IndexType>(qbits[1])});
        }
      }
      if (!lp.empty()) layerPairs.push_back(std::move(lp));
    }
    if (layerPairs.empty()) return qubitsMap;

    // build a linear chain by greedily merging qubit groups in gate order
    auto buildChain = [&](const std::vector<std::vector<QubitPair>>& orderedLP)
        -> std::vector<long long int> {
      std::vector<std::deque<IndexType>> groups;
      std::vector<int> qubitGroup(nrQubits, -1);
      std::unordered_set<IndexType> placedQubits;

      for (const auto& lp : orderedLP) {
        for (const auto& p : lp) {
          const int g1 = qubitGroup[p.q1];
          const int g2 = qubitGroup[p.q2];

          if (g1 < 0 && g2 < 0) {
            // new pair, start a fresh group
            const int gIdx = static_cast<int>(groups.size());
            groups.push_back({p.q1, p.q2});
            qubitGroup[p.q1] = gIdx;
            qubitGroup[p.q2] = gIdx;
            placedQubits.insert(p.q1);
            placedQubits.insert(p.q2);
          } else if (g1 >= 0 && g2 < 0) {
            // q1 placed, q2 not: attach q2 to the closer end
            auto& grp = groups[g1];
            size_t idx = 0;
            for (size_t i = 0; i < grp.size(); ++i)
              if (grp[i] == p.q1) {
                idx = i;
                break;
              }
            if (idx + 1 <= grp.size() - idx)
              grp.push_front(p.q2);
            else
              grp.push_back(p.q2);
            qubitGroup[p.q2] = g1;
            placedQubits.insert(p.q2);
          } else if (g1 < 0 && g2 >= 0) {
            // q2 placed, q1 not: attach q1 to the closer end
            auto& grp = groups[g2];
            size_t idx = 0;
            for (size_t i = 0; i < grp.size(); ++i)
              if (grp[i] == p.q2) {
                idx = i;
                break;
              }
            if (idx + 1 <= grp.size() - idx)
              grp.push_front(p.q1);
            else
              grp.push_back(p.q1);
            qubitGroup[p.q1] = g2;
            placedQubits.insert(p.q1);
          } else if (g1 != g2) {
            // merge two groups so q1 and q2 end up adjacent
            auto& grpA = groups[g1];
            auto& grpB = groups[g2];
            size_t idxA = 0, idxB = 0;
            for (size_t i = 0; i < grpA.size(); ++i)
              if (grpA[i] == p.q1) {
                idxA = i;
                break;
              }
            for (size_t i = 0; i < grpB.size(); ++i)
              if (grpB[i] == p.q2) {
                idxB = i;
                break;
              }
            const size_t distAB = grpA.size() + idxB - idxA;  // [A][B] distance
            const size_t distBA = grpB.size() + idxA - idxB;  // [B][A] distance

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
          // both in the same group: nothing to do

          if (placedQubits.size() == static_cast<size_t>(nrQubits))
            break;
        }

        if (placedQubits.size() == static_cast<size_t>(nrQubits))
          break;
      }

      // concatenate all non-empty groups
      std::vector<IndexType> chain;
      chain.reserve(nrQubits);
      for (const auto& grp : groups)
        for (const auto q : grp) chain.push_back(q);

      // append any qubits with no 2-qubit interactions
      for (IndexType q = 0; q < nrQubits; ++q)
        if (qubitGroup[q] < 0) chain.push_back(q);

      assert(chain.size() == static_cast<size_t>(nrQubits));

      // chain[physPos] = logicalQubit  →  result[logicalQubit] = physPos
      std::vector<long long int> result(nrQubits);
      for (size_t i = 0; i < chain.size(); ++i)
        result[chain[i]] = static_cast<long long int>(i);

      return result;
    };

    auto optMap = buildChain(layerPairs);
    if (layers.size() <= 2) return optMap;
    auto optCost = evaluateCost(optMap);

    std::vector<long long int> qubitsMap(nrQubits);
    std::iota(qubitsMap.begin(), qubitsMap.end(), 0);
    double tryCost = evaluateCostBounded(qubitsMap, optCost);
    if (tryCost < optCost) {
      optCost = tryCost;
      optMap = qubitsMap;
    }

    // try random shuffles in case the heuristic isn't optimal
    std::mt19937 rng(42);
    for (int i = 0; i < nrShuffles; ++i) {
      std::shuffle(qubitsMap.begin(), qubitsMap.end(), rng);
      tryCost = evaluateCostBounded(qubitsMap, optCost);
      if (tryCost < optCost) {
        optCost = tryCost;
        optMap = qubitsMap;
      }
    }


    std::uniform_int_distribution<IndexType> qubitDist(0, nrQubits - 1);
    std::uniform_int_distribution<int> nrSwapsDist(
        1, std::min<int>(3, static_cast<int>(nrQubits) / 2));

    const int maxNoImprove = std::max(nrShuffles, static_cast<int>(nrQubits));
    const int maxTotalShuffles = maxNoImprove * 3;
    int noImproveCount = 0;

    for (int s = 0; s < maxTotalShuffles && noImproveCount < maxNoImprove; ++s) {
      auto tryMap = optMap;
      const int nrSwaps = nrSwapsDist(rng);
      for (int sw = 0; sw < nrSwaps; ++sw) {
        const IndexType a = qubitDist(rng);
        IndexType b = qubitDist(rng);
        while (b == a) b = qubitDist(rng);
        std::swap(tryMap[a], tryMap[b]);
      }

      auto cost = evaluateCostBounded(tryMap, optCost);
      if (cost < optCost) {
        optMap = tryMap;
        optCost = cost;
        noImproveCount = 0;
      } else {
        ++noImproveCount;
      }
    }

    // 2-opt local search: swap pairs until no single swap improves cost
    {
      auto candidate = optMap;
      bool improved = true;
      for (int improvementCount = 0; improved && improvementCount < nrSwaps;
           ++improvementCount) {
        improved = false;
        for (IndexType i = 0; i < nrQubits; ++i) {
          for (IndexType j = i + 1; j < nrQubits; ++j) {
            // swap the mapped positions of qubits i and j
            std::swap(candidate[i], candidate[j]);
            auto cost = evaluateCostBounded(candidate, optCost);
            if (cost < optCost) {
              optMap = candidate;
              optCost = cost;
              improved = true;
            } else {
              // revert
              std::swap(candidate[i], candidate[j]);
            }
          }
        }
      }
    }

    return optMap;
  }

 private:
  void InitQubitsMap() {
    qubitsMap.resize(getNrQubits());
    qubitsMapInv.resize(getNrQubits());

    for (IndexType i = 0; i < static_cast<IndexType>(getNrQubits()); ++i)
      qubitsMapInv[i] = qubitsMap[i] = i;

    totalSwappingCost = 0;
    if (!currentBondDim.empty())
      std::fill(currentBondDim.begin(), currentBondDim.end(), 1.0);
  }

  struct LightweightInitTag {};

  // Skips SetMaxBondDimension; caller must fill bond arrays manually.
  MPSDummySimulator(size_t N, LightweightInitTag)
      : nrQubits(N) {
  }

  void SwapQubits(IndexType qubit1, IndexType qubit2) {
    IndexType realq1 = qubitsMap[qubit1];
    IndexType realq2 = qubitsMap[qubit2];
    if (realq1 > realq2) {
      std::swap(realq1, realq2);
      std::swap(qubit1, qubit2);
    }

    if (realq2 - realq1 <= 1) return;

    const IndexType meetPos = ComputeHeuristicMeetPosition(realq1, realq2);
    SwapQubitsToPosition(qubit1, qubit2, meetPos);
  }

  // Pick the bond with the lowest current cost — mirrors FindBestMeetingPositionLocal.
  IndexType ComputeHeuristicMeetPosition(IndexType realq1,
                                         IndexType realq2) const {
    assert(realq1 < realq2);

    IndexType bestPos = realq1;
    double bestCost = bondCost[realq1];

    for (IndexType m = realq1 + 1; m < realq2; ++m) {
      if (bondCost[m] < bestCost) {
        bestCost = bondCost[m];
        bestPos = m;
      }
    }

    return bestPos;
  }

 public:
  // Move qubit1 and qubit2 until they are adjacent at meetPosition / meetPosition+1.
  // meetPosition must be in [min(realq1,realq2), max(realq1,realq2)-1].
  void SwapQubitsToPosition(IndexType qubit1, IndexType qubit2,
                            IndexType meetPosition) {
    IndexType realq1 = qubitsMap[qubit1];
    IndexType realq2 = qubitsMap[qubit2];
    if (realq1 > realq2) {
      std::swap(realq1, realq2);
      std::swap(qubit1, qubit2);
    }

    if (realq2 - realq1 <= 1) return;

    assert(meetPosition >= realq1 && meetPosition < realq2);

    // shift qubit1 right toward meetPosition
    {
      IndexType movingReal = realq1;
      while (movingReal < meetPosition) {
        const IndexType toReal = movingReal + 1;
        const IndexType toInv = qubitsMapInv[toReal];

        qubitsMap[toInv] = movingReal;
        qubitsMapInv[movingReal] = toInv;

        qubitsMap[qubit1] = toReal;
        qubitsMapInv[toReal] = qubit1;

        totalSwappingCost += bondCost[movingReal];
        growBondDimension(movingReal, true);
        movingReal = toReal;
      }
    }

    // shift qubit2 left toward meetPosition+1
    {
      IndexType movingReal = realq2;
      while (movingReal > meetPosition + 1) {
        const IndexType toReal = movingReal - 1;
        const IndexType toInv = qubitsMapInv[toReal];

        qubitsMap[toInv] = movingReal;
        qubitsMapInv[movingReal] = toInv;

        qubitsMap[qubit2] = toReal;
        qubitsMapInv[toReal] = qubit2;

        totalSwappingCost += bondCost[toReal];
        growBondDimension(toReal, true);
        movingReal = toReal;
      }
    }

    assert(abs(qubitsMap[qubit1] - qubitsMap[qubit2]) == 1);
  }

 private:
  size_t nrQubits;

  std::vector<IndexType> qubitsMap;
  std::vector<IndexType> qubitsMapInv;

  static constexpr size_t physExtent = 2;
  IndexType maxVirtualExtent = 0;
  std::vector<double> bondCost;
  std::vector<double> maxBondDim;
  std::vector<double> currentBondDim;

  double totalSwappingCost = 0;

  double growthFactorSwap = 0.65;
  double growthFactorGate = 0.35;  

  void growBondDimension(IndexType bond, bool swap = true, int schmidtRank = 4) {
    // After a gate or swap on adjacent tensors, the two tensors are contracted
    // into a matrix of size (2*chiL) x (2*chiR) and SVD is applied.
    // The resulting bond dim is at most min(2*min(chiL,chiR), maxBondDim[bond]).
    // Swaps are special — they only permute physical leg values, so they don't
    // increase Schmidt rank beyond the current chiL = chiR case.

    const IndexType leftBond = bond - 1;
    const IndexType rightNeigborBond = bond + 1;
    const double betweenDim = currentBondDim[bond];

    const double leftDim = leftBond >= 0 ? currentBondDim[leftBond] : 1;
    const double rightNeighborDim = rightNeigborBond < static_cast<IndexType>(currentBondDim.size()) ? currentBondDim[rightNeigborBond] : 1;
    
    double newMaxDim = (swap && leftDim == rightNeighborDim) ? betweenDim : 2. * std::min(leftDim, rightNeighborDim);
    newMaxDim = std::min(newMaxDim, betweenDim * schmidtRank);

    const double growthFactor = swap ? growthFactorSwap : growthFactorGate;

    currentBondDim[bond] = std::min(newMaxDim * growthFactor, maxBondDim[bond]);

    currentBondDim[bond] = std::max(currentBondDim[bond], 1.);

    updateAffectedBondCosts(bond);
  }
 private:
  CostModel costModel = CostModel::CentralBondCubic;
};

}  // namespace Simulators

#endif
