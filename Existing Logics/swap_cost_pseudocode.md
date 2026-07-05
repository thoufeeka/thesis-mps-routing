# Existing SWAP Cost Computation:

> Sources:
> - [`MPSDummySimulator.h`](file:///home/steffi/thesis-mps-routing/maestro/Simulators/MPSDummySimulator.h) - cost tracking / bond dimension model
> - [`MPSSimulator.h`](file:///home/steffi/thesis-mps-routing/maestro/build/_deps/qcsim-src/QCSim/MPSSimulator.h) - real simulator swap execution
> - [`QCSimState.h`](file:///home/steffi/thesis-mps-routing/maestro/Simulators/QCSimState.h) - lookahead callback wiring
> - [`mps_benchmark.cpp`](file:///home/steffi/thesis-mps-routing/benchmarks/mps_benchmark.cpp) - top-level driver

---

## Step 1 - Initialisation

```
GIVEN: N qubits, max bond dimension χ

FOR each bond b in [0, N-2]:
    maxBondDim[b] = min(2^(b+1), 2^(N-b-1), χ)
    currentBondDim[b] = 1.0          // product state: no entanglement yet
    bondCost[b]       = 1.0^3 = 1    // cost = currentBondDim³

qubitsMap[q]    = q   // logical qubit q lives at chain position q
qubitsMapInv[p] = p   // chain position p holds logical qubit p

totalSwappingCost = 0
```

> ie, We start with every qubit in its own position on the MPS chain. The bond dimension between every neighbour pair is 1 (no entanglement), so the initial cost per bond is 1³ = 1. The cost metric will grow as gates entangle qubits and increase bond dimensions.

---

## Step 2 - Bond Dimension Growth Model

This is used by the dummy simulator to predict cost without actually doing linear algebra.
Pesudocode :
```
FUNCTION growBondDimension(bond b, isSwap):

    leftDim  = currentBondDim[b-1]  (or 1 if b is the leftmost bond)
    rightDim = currentBondDim[b+1]  (or 1 if b is the rightmost bond)
    betweenDim = currentBondDim[b]

    IF isSwap AND leftDim == rightDim:
        newMaxDim = betweenDim          // swap doesn't increase entanglement in symmetric case
    ELSE:
        newMaxDim = 2 * min(leftDim, rightDim)   // physical dimension is 2

    // Limit by Schmidt rank of the gate
    schmidtRank = isSwap ? 1 : (isControlled ? 2 : 4)
    newMaxDim = min(newMaxDim, betweenDim * schmidtRank)

    // Apply growth factor (< 1.0 models truncation / incomplete entanglement)
    growthFactor = isSwap ? growthFactorSwap(=0.65) : growthFactorGate(=0.35)

    currentBondDim[b] = clamp(newMaxDim * growthFactor, 1, maxBondDim[b])

    // Always keep cost = dim³  (SVD cost scales as O(d³))
    bondCost[b] = currentBondDim[b]³
```

> ie, Every time a gate touches the bond between two adjacent chain positions, the bond dimension might grow (more entanglement). The growth is capped by the physical qubit dimension (2), the gate's Schmidt rank, and the global χ limit. We raise the result to the power of 3 because the dominant cost of SVD truncation (which MPS simulators must do after every 2-qubit gate) scales as *d³* in the bond dimension *d*.

---

## Step 3 - Meeting Position Selection

When two non-adjacent qubits must interact, they must be slid together on the chain. The position where they "meet" matters because bonds with lower dimension are cheaper to cross.

### 3a. Heuristic (local, dummy sim and real sim fallback)

```
FUNCTION FindBestMeetingPositionHeuristic(qubit1, qubit2):

    realq1 = qubitsMap[qubit1]
    realq2 = qubitsMap[qubit2]
    ensure realq1 < realq2

    bestPos  = realq1
    bestCost = bondCost[realq1]

    FOR m = realq1+1 TO realq2-1:
        IF bondCost[m] < bestCost:
            bestCost = bondCost[m]
            bestPos  = m

    RETURN bestPos    // cheapest bond in the gap
```

### 3b. Lookahead (QCSimState callback - used when lookaheadDepth > 0)

```
FUNCTION FindBestMeetingPosition(upcomingGates, gateIndex, lookaheadDepth,
                                  currentCost, bestCost):

    op     = upcomingGates[gateIndex]
    qubit1, qubit2 = op.AffectedQubits()

    realq1 = qubitsMap[qubit1]
    realq2 = qubitsMap[qubit2]

    IF realq2 - realq1 <= 1:           // already adjacent, no swap needed
        EvaluateMeetingPositionCost(realq1, ..., currentCost, bestCost)
        RETURN realq1

    IF lookaheadDepth <= lookaheadDepthWithHeuristic:
        // Use single heuristic position (fast path)
        bestPos = FindBestMeetingPositionHeuristic(realq1, realq2)
        EvaluateMeetingPositionCost(bestPos, ..., currentCost, bestCost)
    ELSE:
        // Try EVERY possible meeting position in [realq1, realq2-1]
        FOR m = realq1 TO realq2-1:
            EvaluateMeetingPositionCost(m, ..., currentCost, bestCost)
            track bestPosition with lowest bestCost

    RETURN bestPosition
```

```
FUNCTION EvaluateMeetingPositionCost(meetPos, upcomingGates, gateIndex,
                                      lookaheadDepth, currentCost, bestCost):

    // Clone current dummy sim state (bond dims, qubit map)
    dummySim = clone of current state, totalSwappingCost = 0

    // Simulate the swaps to meetPos
    IF realq2 - realq1 > 1:
        dummySim.SwapQubitsToPosition(qubit1, qubit2, meetPos)

    // Simulate applying the gate itself
    dummySim.ApplyGate(op)

    currentCost += dummySim.totalSwappingCost    // add cost of this gate

    // Pruning: abandon if already worse than best known
    IF currentCost >= bestCost: RETURN

    IF lookaheadDepth <= 0:
        bestCost = min(bestCost, currentCost)
        RETURN

    // Recurse on next 2-qubit gate
    nextGateIndex = gateIndex + 1   // skip any 1-qubit gates
    dummySim.FindBestMeetingPosition(upcomingGates, nextGateIndex,
                                     lookaheadDepth - 1,
                                     currentCost, bestCost)
```

> ie, The lookahead search tries every place the two qubits could meet, simulates the resulting SWAP sequence *without touching the real simulator*, and recursively considers the next few gates too. It keeps the cheapest option. When `lookaheadDepth` is large, it searches all positions; once it gets shallow it switches to the heuristic (lowest bond in gap) for speed.

---

## Step 4 - Executing the SWAPs (Dummy Simulator)

```
FUNCTION SwapQubitsToPosition(qubit1, qubit2, meetPosition):

    realq1 = qubitsMap[qubit1]
    realq2 = qubitsMap[qubit2]
    ensure realq1 < realq2

    // Slide qubit1 RIGHTWARD from realq1 → meetPosition
    movingReal = realq1
    WHILE movingReal < meetPosition:
        toReal = movingReal + 1
        toInv  = qubitsMapInv[toReal]

        // Update maps (swap the two logical qubits at these positions)
        qubitsMap[toInv]     = movingReal
        qubitsMapInv[movingReal] = toInv
        qubitsMap[qubit1]    = toReal
        qubitsMapInv[toReal] = qubit1

        totalSwappingCost += bondCost[movingReal]   // ← cost accumulation
        growBondDimension(movingReal, isSwap=true)   // ← bond dim update

        movingReal = toReal

    // Slide qubit2 LEFTWARD from realq2 → meetPosition+1
    movingReal = realq2
    WHILE movingReal > meetPosition + 1:
        toReal = movingReal - 1
        toInv  = qubitsMapInv[toReal]

        qubitsMap[toInv]     = movingReal
        qubitsMapInv[movingReal] = toInv
        qubitsMap[qubit2]    = toReal
        qubitsMapInv[toReal] = qubit2

        totalSwappingCost += bondCost[toReal]        // ← cost accumulation
        growBondDimension(toReal, isSwap=true)        // ← bond dim update

        movingReal = toReal

    // Now qubits are adjacent at (meetPosition, meetPosition+1)
```

> ie, The lower qubit slides right; the upper qubit slides left. Every single elementary swap across a bond contributes `bondCost[bond]` = `currentBondDim[bond]³` to `totalSwappingCost`. After each elementary swap the bond dimension at that position is updated (it may grow slightly).

---

## Step 5 - Applying the 2-Qubit Gate (after SWAPs)

```
FUNCTION ApplyGate(gate, qubit1, qubit2):

    IF gate.isOneQubit:
        RETURN   // 1-qubit gates don't cause swaps, ignored in dummy sim

    // Bring qubits adjacent (if not already)
    IF |qubitsMap[qubit1] - qubitsMap[qubit2]| > 1:
        SwapQubits(qubit1, qubit2)       // uses heuristic meeting position

    bond = min(qubitsMap[qubit1], qubitsMap[qubit2])

    // Charge the gate cost at the bond (BEFORE bond dim growth)
    totalSwappingCost += bondCost[bond]

    // Grow bond dimension due to the gate's entanglement potential
    schmidtRank = gate.isControlled ? 2 : 4
    growBondDimension(bond, isSwap=false, schmidtRank)
```

> ie, After sliding the two qubits together, the actual gate is applied. The bond between them is charged one more time (the gate itself also does an SVD internally). Then the bond dimension is grown more aggressively than a SWAP would (a general 2-qubit gate can create more entanglement than a SWAP).

---

## Step 6 - Real Simulator Execution (MPSSimulator)

The real simulator mirrors the dummy logic but uses actual tensor algebra:

```
FUNCTION RealSim.ApplyGate(gate, qubit, controlQubit):

    realq1 = qubitsMap[qubit]
    realq2 = qubitsMap[controlQubit]

    IF |realq1 - realq2| > 1:
        IF meetingPositionCallback is set:
            // QCSimState lookahead callback runs FindBestMeetingPosition
            // on the dummy sim (seeded with real bond dims) and returns meetPos
            meetPos = meetingPositionCallback(impl.getBondDimensions())
            SwapQubitsToPosition(qubit, controlQubit, meetPos)

        ELSE IF useOptimalMeetingPosition:
            // Local greedy: pick bond with smallest actual bond dimension
            meetPos = FindBestMeetingPositionLocal(qubit, controlQubit)
            SwapQubitsToPosition(qubit, controlQubit, meetPos)

        ELSE:
            // Old heuristic (moves qubit from the end closer to middle)
            SwapQubits(qubit, controlQubit)

    // Actually apply the gate tensor contraction + SVD
    impl.ApplyGate(gate, realq1, realq2)
```

> ie, The real MPS simulator does the same positional moves as the dummy, but instead of just updating numbers it actually contracts tensors and applies SVD. The lookahead callback is what connects the dummy sim (for cost *prediction*) to the real sim (for cost *execution*).

---

## Step 7 - Top-Level Cost Accumulation (Benchmarking suite---Needs to Review later)

```
// From mps_benchmark.cpp
sim = MPSDummySimulator(N)
sim.SetMaxBondDimension(χ)
sim.SetInitialQubitsMap(identityMap)   // resets totalSwappingCost = 0, bondCost[i] = 1

FOR each gate in circuit:
    sim.ApplyGate(gate)
    // After each gate, sample peak bond dimension for statistics

totalCost = sim.getTotalSwappingCost()
```

---

## Summary: Genral formula In exisitng logic

```
totalSwappingCost = Σ  bondCost[b]
                   (b,t)

where the sum runs over every (bond b, time t) pair at which
an elementary SWAP or a 2-qubit gate is applied across bond b,
and bondCost[b] = currentBondDim[b]³  at that moment in time.
```

| Concept | Meaning |
|---|---|
| `bondCost[b]` | Current bond dimension at bond `b`, **cubed** (models SVD cost ∝ d³) |
| Elementary SWAP | Costs `bondCost[bond]`, slightly grows `currentBondDim[bond]` |
| 2-qubit gate | Costs `bondCost[bond]`, grows `currentBondDim[bond]` more than a SWAP |
| `totalSwappingCost` | Running sum - the thing we minimise by choosing a good qubit ordering and meeting position |
| `growthFactorSwap = 0.65` | SWAPs grow bond dim by 65% of theoretical max (models partial entanglement) |
| `growthFactorGate = 0.35` | General gates grow bond dim by 35% (truncation reduces growth in practice) |

GrowthFactor is set to constant values now ### Needs to be checked in Literature
