# Direction 1: Geometry-Aware SWAP / Two-Qubit Cost

On improving the `MPSDummySimulator` cost
model, I again inspected the real MPS two-site operation path before fixing
the new formula. Like you said, the important observation is that the real simulator
does not perform an SVD whose geometry is determined only by the central
bond dimension.

For two neighbouring MPS tensors,

$$A_i: (\chi_L, 2, \chi_C), \qquad A_{i+1}: (\chi_C, 2, \chi_R)$$

the implementation contracts the tensors and subsequently performs a
thin SVD on a reshaped matrix with dimensions

$$(2\chi_L) \times (2\chi_R)$$

The current dummy model uses

$$C_{\mathrm{current}} = \chi_C^3$$

This is a useful cubic approximation when the local bond dimensions are
similar, but it cannot distinguish local geometries with the same
central bond and very different outer bonds.

## Proposed cost

I propose the lightweight local surrogate

$$\boxed{C_{\mathrm{geo}} = \frac{\chi_L\chi_C\chi_R + 2\chi_L\chi_R\min(\chi_L,\chi_R)}{3}}$$

The first term represents the leading dependence of the two-site tensor
contraction,

$$C_{\mathrm{contract}} \propto \chi_L\chi_C\chi_R$$

while the second represents the geometry of the thin SVD,

$$C_{\mathrm{SVD}} \propto \chi_L\chi_R\min(\chi_L,\chi_R)$$

The relative factor 2 comes from retaining the physical dimension ($d=2$)
in the simplified leading operation counts before removing a common
factor. The division by 3 is normalization. For a locally uniform MPS,

$$\chi_L = \chi_C = \chi_R = \chi$$

the proposed model gives exactly

$$C_{\mathrm{geo}} = \chi^3$$

Therefore the existing model is recovered in the uniform-bond case,
while the new model distinguishes asymmetric and non-uniform local
tensor geometries.

For example, the current model assigns the same cost to `(4,32,4)` and
`(64,32,64)` because both have $\chi_C = 32$. In the real
operation, however, the corresponding thin-SVD matrices are
$8 \times 8$ and $128 \times 128$. The proposed model
captures this difference without performing any tensor operation.

This should be treated as a **computational-cost surrogate**, not an
exact runtime estimator.


## Planned implementation

The change can remain local to `MPSDummySimulator`.

A helper such as

``` cpp
double computeBondCost(IndexType bond) const;
```

will obtain the current estimated
$(\chi_L, \chi_C, \chi_R)$ and evaluate
$C_{\mathrm{geo}}$.

The existing routing and recursive lookahead logic remains unchanged. An
operation is still charged using the current estimated state, after
which `growBondDimension()` predicts the new bond dimension for
subsequent operations.

One implementation detail changes because the new cost couples
neighbouring bond dimensions. If $\chi_b$ changes, it affects
not only the cost of bond $b$, but potentially

$$C_{b-1}, \quad C_b, \quad C_{b+1}$$

because $\chi_b$ becomes an outer dimension in neighbouring
cost calculations. These local costs therefore need to be recomputed
after every predicted bond update. A full recomputation is only required
when the entire bond-dimension state is reset.

The proxy remains $\mathcal{O}(1)$ per bond and uses only already estimated bond
dimensions, preserving the lightweight evaluation needed by recursive
routing.

For the first experiment I plan to change **only the cost model**,
keeping the current bond-growth heuristic unchanged. This gives a clean
comparison between the existing $\chi_C^3$ baseline and the
geometry-aware model before introducing the separate context-aware
growth direction.

