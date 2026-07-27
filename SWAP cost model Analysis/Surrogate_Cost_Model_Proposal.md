# Proposal: Improving the Surrogate Cost Model of Maestro's MPSDummySimulator

## Background

The current routing algorithm in Maestro relies on the
`MPSDummySimulator` to estimate the computational cost of candidate
routing decisions. This design is intentional: evaluating every
candidate by executing a complete Matrix Product State (MPS) simulation
would require repeated tensor contractions and singular value
decompositions (SVDs), making routing significantly more expensive than
the simulation itself. Instead, the dummy simulator acts as a
lightweight surrogate that predicts how bond dimensions evolve and
estimates the cost of future operations.

At present, the surrogate is deliberately simple. The local operation
cost is primarily estimated using the current bond dimension (`χ³`),
while bond growth is approximated using fixed empirical growth factors
for SWAP gates and general two-qubit gates. This provides very fast
evaluation and enables recursive lookahead during routing, but it also
ignores much of the local information that is already available inside
the dummy simulator.

During the analysis of the current implementation, one observation
became clear: the routing algorithm itself is not necessarily the
limiting factor. Instead, the quality of routing decisions depends
heavily on the quality of the surrogate cost model. If the surrogate can
better predict the relative cost of candidate routing paths while
maintaining constant-time evaluation, the existing routing framework may
already be capable of producing better routing decisions.

Therefore, instead of proposing an entirely new routing algorithm, this
work focuses on improving the surrogate model used by the existing
routing framework.

------------------------------------------------------------------------

## Research Question 1: Bond-Dimension-Aware Cost Functions and Lookahead

**How can the SWAP cost prediction and lookahead heuristic in the Maestro MPS simulator be systematically improved by transitioning from static, guessed parameters to a dynamic, bond-dimension-aware cost function?**

The objective is not to reproduce the behaviour of the full MPS
simulator exactly. Instead, the surrogate should remain lightweight
while producing a more meaningful estimate of routing cost than the
current implementation.

------------------------------------------------------------------------

## Proposed Direction

Rather than replacing the routing algorithm, the proposed work
introduces progressively richer surrogate models inside the existing
`MPSDummySimulator`. The routing logic, recursive lookahead and
meeting-position search remain unchanged. Only the way candidate costs
are estimated is modified.

### 1. Geometry-aware operation cost

The current implementation estimates the cost of an operation primarily
from the current bond dimension. However, two bonds with the same
central bond dimension may have very different neighbouring bond
dimensions, resulting in tensors of different sizes.

For example,

-   χL = 4, χ = 32, χR = 4
-   χL = 64, χ = 32, χR = 64

currently receive essentially the same estimated cost even though the
surrounding tensor geometry is very different.

The first improvement is therefore to extend the surrogate state from
only the current bond dimension to the local bond profile `(χL, χ, χR)`
and derive a geometry-aware cost function. Since these quantities are
already available during bond-growth prediction, this change introduces
very little computational overhead.

------------------------------------------------------------------------

### 2. Context-aware bond growth

The second improvement targets the bond-growth prediction itself.

Currently the dummy simulator uses fixed empirical growth factors for
SWAP gates and general two-qubit gates. Although computationally
efficient, these constants cannot capture differences caused by the
surrounding bond profile.

Instead of using fixed values, the growth prediction will incorporate
the local MPS context already available during routing. The goal is not
to perfectly model entanglement generation, but to obtain a more
informative prediction of future bond dimensions that can guide the
existing lookahead search. Will define a 
growthFactor = computeGrowthFactor(leftDim, betweenDim, rightDim, isSwap, schmidtRank) will be added. 
A formula that predicts real entanglement. for method's body either 
linear interpolation, ratio-based scaling, saturation curves, etc. can be used based on the results. 
Note: cannot establish that without real MPS validation.


------------------------------------------------------------------------

### 3. State-aware surrogate cost : Interaction-Weighted Terminal State Cost

The current surrogate evaluates recursively and accumulates costs over the configured lookahead depth.

However, routing decisions also influence the predicted MPS state after
the operation. Two candidate routes may have similar immediate costs but
produce very different bond-dimension distributions for the remaining
circuit.

The final surrogate therefore introduces a lightweight state-quality
term that penalizes undesirable predicted bond profiles, such as
excessive concentration of large bond dimensions or strong imbalance
between neighbouring bonds. This additional information is intended to
help the recursive search favour routing decisions that preserve a
healthier MPS representation for future operations.

Importantly, this penalty is computed entirely from the predicted bond
dimensions produced by the dummy simulator and does not require
execution of the full MPS simulator.

------------------------------------------------------------------------

## Implementation Plan

One advantage of this approach is that it fits naturally into the
existing architecture.

The routing algorithm itself remains unchanged. The recursive lookahead
and meeting-position optimization continue to operate exactly as before.

Implementation is mainly limited to:

-   introducing a lightweight surrogate state representation,
-   extending the current operation cost computation,
-   improving bond-growth prediction,
-   adding an optional state-quality penalty(direction 3).

Most changes are expected to be localized within the
`MPSDummySimulator`, allowing direct comparison with the current
implementation.

------------------------------------------------------------------------

## Evaluation

The proposed surrogate models can be evaluated against the current
Maestro implementation using the existing benchmark circuits.

Since the objective is to improve routing decisions rather than
reproduce exact MPS runtime, evaluation will focus on routing quality
and computational overhead instead of detailed runtime calibration with
the full MPS simulator.

# Possible Extensions (Future Work)

The current thesis focuses on improvements that are practical within the
available implementation time. Nevertheless, the proposed framework
naturally opens several future research directions.


## 1: Adaptive Growth Factors via Online Calibration

Instead of hardcoding growth factors, **learn them from the circuit being compiled** using a short online calibration pass over the first $k$ layers.

The idea: run a quick forward pass of the dummy simulator over the first $k \leq 10$ gate layers with multiple candidate growth factor pairs $\{(g_s, g_g)\}$ sampled on a grid. Compare the predicted bond dimension profile to the actual profile obtained from the **real** QCSim simulator after the same $k$ layers.
This is a one-time calibration, not a per-gate optimisation.

### Mathematical Formulation

The calibration runs on a grid: $g_s \in \{0.1, 0.2, \ldots, 1.0\}$, $g_g \in \{0.1, 0.2, \ldots, 1.0\}$ — 100 dummy simulator evaluations, each $O(kN)$.

After calibration, the fitted $(g_s^*, g_g^*)$ replace the static 0.65/0.35 constants **for the entire circuit routing pass**.

### Variant: Circuit-Family Classifier

Use a lightweight feature vector of the circuit:
- Fraction of controlled vs. non-controlled 2-qubit gates
- Mean qubit distance of gate operands
- Gate depth

Train a small lookup table or regression model offline, and select growth factors from the model at runtime in $O(1)$.
Note: Requires the real MPS simulator to be accessible at calibration time(calibration harness and real sim access)
## 2: Bond-Dimension-Profile Entropy Regularisation 

speculative but potentially interesting. A well-structured quantum circuit on an MPS chain should produce a peaked, smooth bond-dimension profile (high in the middle of the chain, low at boundaries). Highly non-uniform profiles indicate that the qubit ordering is misaligned with the entanglement structure, leading to expensive SWAP sequences later.

This observation motivates adding a profile regularity term to the cost: routes that would produce a more uniform bond-dimension distribution across the chain are preferred.

------------------------------------------------------------------------

## References (Initial)

1.  U. Schollwöck, *The Density-Matrix Renormalization Group in the Age
    of Matrix Product States*, Annals of Physics, 2011.
2.  R. Orús, *A Practical Introduction to Tensor Networks*, Annals of
    Physics, 2014.
3.  Li, Ding, Xie, *Tackling the Qubit Mapping Problem for NISQ-Era
    Quantum Devices*, ASPLOS, 2019.
4.  Cowtan et al., *On the Qubit Routing Problem*, 2019.
5.  Gray and Kourtis, *Hyper-optimized tensor network contraction*,
    Quantum, 2021.
6. Ryo Watanabe, Dries Sels, Joseph Tindall, Tensor network surrogate models for variational quantum computation, 2026
7. Gabin Schieffer, Stefano Markidis, Ivy Peng, Harnessing CUDA-Q's MPS for Tensor Network Simulations of Large-Scale Quantum Circuits, 2025


