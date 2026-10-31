# Mathematics

Set-up: the target program `T` is a fixed-length word of instructions; a
*candidate* `P` is any other word of the same length (unused slots are `nop`).
Both are executed on a machine state `s ∈ (𝔽₂³²)³²` (32 registers of 32 bits).
Inputs are the live-in registers; outputs the live-out registers.

---

## 1. Equivalence as a Hamming distance over outputs

Define the per-register output difference as the bitwise popcount of the XOR:

```text
δ_t,r(P) = popcount( P_t[r] ⊕ T_t[r] )      ∈ {0, 1, …, 32}
```

where `P_t[r]` is register `r` after running `P` on test input `t`. Sum over
tests and live-outs to get the **closeness of P to T**:

```text
eq(P) = Σ_{t ∈ Tests} Σ_{r ∈ live_out}  Δ_Hamming( P_t[r] , T_t[r] )
      = Σ_t Σ_r popcount( P_t[r] ⊕ T_t[r] )
```

Facts:

- `eq(P) = 0` iff `P` and `T` agree on every live-out register for every test
  input — i.e. `P` is (test-)equivalent to `T`.
- `eq(P) > 0` is a *graded* score proportional to the number of wrong output
  bits. This is the "Hamming-distance-style graded correctness" of STOKE
  (Schkufza, Sharma & Aiken, ASPLOS 2013): it turns an all-or-nothing search
  into a smooth surface the sampler can walk downhill.

---

## 2. Cost function

```text
cost(P) = w_c · eq(P) + w_p · perf(P)
```

with `w_c = 1000`, `w_p = 1`, and `perf(P) = number of non-nop instructions`.

### Why the 1000-weight works

Because `perf(P) ≤ |buffer|` and buffers are short (≤ ~16), every *correct*
program (eq = 0) has `cost ≤ |buffer| < 1000`, while every *incorrect* program
(eq ≥ 1) has `cost ≥ 1000`. Therefore:

```text
cost(P) < 1000   ⟺   eq(P) = 0
```

So the cost surface's global minimum is **always a correct program**, and among
correct programs the cost is exactly the instruction count — the search
minimises instructions automatically. Choosing `w_c` larger than any possible
`perf` makes the lexicographic priority *correctness > instruction count*
exact (rather than just weighted).

---

## 3. Metropolis–Hastings

We search a discrete state space `Ω` (all length-`L` programs over the pool).
We want to sample from the **Boltzmann distribution**

```text
π(x) ∝ exp( −β · cost(x) ),        β > 0  ("inverse temperature")
```

which concentrates mass on low-cost programs. As `β→∞` this becomes the argmin;
as `β→0` it becomes uniform. A finite positive β lets the chain climb out of
local minima.

**Proposal.** From current state `x`, propose `x'` by picking one mutation
uniformly. The version of the criterion used in code (`mcmc.cpp`):

```text
u ~ Uniform(0,1) drawn first, then accept iff
    cost(x') < cost(x) − ln(u)/β                 ← equivalent form used for early-exit
```

Rearranging, `cost(x') − cost(x) < −ln(u)/β` ⟺ `e^{−βΔ} > u`, i.e.:

```text
α(x → x') = min( 1 , exp( −β · (cost(x') − cost(x)) ) )     with Δ = cost(x') − cost(x)
```

- `Δ ≤ 0`: accept always.
- `Δ > 0`: accept with probability `exp(−βΔ)`.

### Why the chain converges to π

Metropolis–Hastings is a Markov chain whose stationary distribution is `π`
provided the chain is **irreducible** (every state reachable) and there is no
proposal bias. With acceptance `α` the transition satisfies **detailed balance**:

```text
π(x)·Q(x→x') = π(x')·Q(x'→x),    Q(x→x') = q(x'|x)·α(x→x')
```

which implies `π` is stationary. The chain starts at the all-nop state, which
is connected (mutator can reach anything), so the ergodic average converges to
the Boltzmann distribution, and with enough iterations the best visited state
approximates the global minimum of `cost`.

### Why no proposal-ratio correction

For a symmetric proposal `q(x'|x) = q(x|x')` the full MH acceptance is:

```text
α = min( 1 ,  [π(x')·q(x|x')] / [π(x)·q(x'|x)] )
  = min( 1 ,  exp(−β(cost(x')−cost(x))) )       (when q symmetric)
```

Check each move is symmetric (all pick slot and values uniformly):

- **opcode change:** pick slot `i` uniformly, then uniform among same-format
  opcodes. From `(x, x')` the reverse proposal picks the same slot and the same
  uniform random opcode distribution → equal probability.
- **operand change:** pick slot, field, then the operand value uniformly from
  the pool. Reverse chooses the identical field with identical distribution.
- **swap:** swapping is its own inverse → symmetric by construction.
- **replace:** pick slot and draw uniformly from the same distribution
  (10% nop / 90% random insn) in both directions.

Because all moves are symmetric, the Hastings ratio `q(x|x')/q(x'|x) = 1` and
the simple `exp(−βΔ)` criterion is exact — no correction factor.

### 3.1 Adaptive inverse temperature (`--algo adaptive`)

Fixed `β` is target-dependent: a value that works for a 3-instruction block can
be too hot or too cold for a 20-instruction one. Adaptive MCMC keeps the same
symmetric proposal and the same Metropolis rule, but changes `β` online.

Let `a_j` be the fraction of proposals accepted in window `j` (default window
`W = 200`) and let `a*` be the target acceptance rate (`--target-accept`,
default `0.234`). The implementation performs a bounded stochastic-
approximation update in log-space:

```text
log β_{j+1} = log β_j + g · (a_j − a*)
β_{j+1}     = clamp( β_{j+1}, β_min, β_max )
```

with gain `g = --adapt-gain` (default 1.0) and `β ∈ [10⁻³, 10³]`.

Interpretation:

- `a_j > a*` (chain accepts too much) → `β` increases → colder chain, more greed.
- `a_j < a*` (chain rejects too much) → `β` decreases → hotter chain, more
  exploration.
- In expectation the update stops when `a_j = a*`; the window average damps
  noise from the discrete cost surface.

This is the Robbins–Monro-style adaptation of the inverse temperature. With a
constant gain and a non-diminishing step it is a practical adaptive scheme, not
a proof of exact stationarity; the clamp keeps it bounded. The proposals are
still symmetric, so no Hastings correction is introduced.

### 3.2 HC-MCMC (`--algo hc`)

HC-MCMC is the `β → ∞` limit of the Metropolis rule:

```text
lim_{β→∞} min(1, exp(−βΔ)) = 1[Δ ≤ 0]
```

so the chain becomes greedy: it accepts a proposal iff
`cost(x') ≤ cost(x)`. Equal-cost moves are kept, which lets the walk traverse
plateaus instead of freezing immediately.

Greedy descent alone is not ergodic. After `S = --hc-stall` iterations without
a new best, the implementation performs a kick:

```text
x ← mutate^K( x_best )       (K = --hc-kick, default 4)
```

where `x_best` is the lowest-cost program visited so far and `mutate` is one
uniform random mutator move. The kicked state is accepted unconditionally and
greedy descent resumes. This is an iterated-local-search / random-restart
hybrid, not a sampler: there is no detailed balance and no claim that the
visited states follow `π`. The reported answer is still the best state ever
visited.

### 3.3 Enumerative search space (`--algo enum`)

Enumeration tries every program of length `ℓ = 0, 1, …, L_max` over the
register pool `R` and immediate pool `I`. For one slot the branching factor is
approximately

```text
B ≈ Σ_{op ∈ R-format} R³  +  Σ_{op ∈ I-format} R²·|I|
      +  Σ_{op ∈ shift} R²·32  +  Σ_{op ∈ U-format} R·|I|
```

so the unexplored space at depth `ℓ` is `Θ(B^ℓ)`. The implementation prunes
dead writes (a written register must be live-out or read later) and
canonicalises commutative operands, but those only remove a lower-order factor:
growth is still exponential in the required program length. This is why
enumeration is exact but has a hard practical breaking point around `ℓ = 3–4`;
MCMC instead pays a fixed budget `O(iters · tests · L)`, independent of the
depth of the solution it has to discover.

---

## 4. What the search is actually optimising

Combining §1–§3: the sampler sees a smooth surface

```text
cost(x) = 1000 · Σ_t Σ_r popcount( x_t[r] ⊕ T_t[r] )  +  #instructions(x)
```

- Far from a solution, the gradient goes through *output-bit similarity* —
  programs that "almost" compute the right answer have lower cost and are
  favoured.
- Near equivalence, the `#instructions` term takes over, so the chain climbs
  toward the shortest correct program.
- The final posted result is the lowest-cost program ever visited (not the
  current state), e.g. the smoke target `t3 = (a0+a1−a1)<<1` converges to the
  single instruction `add x28, x10, x10`.

---

## 5. Verification probability (how "probabilistic" is it?)

The result passes `k = 10 000` independent random tests each with `m` live-out
bits. If a candidate were wrong on a single input, roughly a fraction `q` of
random inputs would expose it (typically `q ≥ 1/2`). The probability that all
`k` tests miss it is bounded by

```text
P(false pass) ≤ (1 − q)^k
```

e.g. for `q = 1/2`, `2^−10000 ≈ 0`. This is why the readout says
`OK (probabilistic)` rather than "proven" — provable equivalence requires the
Z3 `QF_BV` tier of `architecture.md` §4.5.

---

## 6. How the benchmark measures "better"

`tools/benchmark_search.py` compares the three strategies on targets whose
optimum is known by construction. For each run it records:

```text
success(P)    = 1[ eq(P) = 0 ]                       (found a correct program)
optimal(P)    = 1[ eq(P) = 0 ∧ #insns(P) = opt ]     (found the known optimum)
time          = wall-clock seconds of that run
```

With `n` seeds, the reported success/optimality rates are
`(1/n)·Σ success`, `(1/n)·Σ optimal`, and the timing is the median over seeds.
Because the MCMC budget is fixed, its time grows with iteration count and
buffer length; enumeration's time grows with the required optimum length.
The crossing of those two curves defines the breaking points listed in
[benchmark.md](benchmark.md).

Formally, let `ℓ*` be the shortest correct program length and `L` the target
buffer length. Enumeration costs `Θ(B^{ℓ*})`; one MCMC run costs
`Θ(iters · tests · L)`. Thus:

- small `ℓ*` → enumeration wins;
- larger `ℓ*` or a hard length cap → MCMC wins;
- very large `L` with a fixed iteration budget → both MCMC variants lose
  reliability, while enumeration is still exact if `ℓ*` is small enough.

---

## 7. References and further reading

- E. Metropolis et al., *Equation of State Calculations by Fast Computing
  Machines*, J. Chem. Phys. 21 (1953); W.K. Hastings, Biometrika 57 (1970) —
  the sampler.
- E. Schkufza, R. Sharma, A. Aiken, *Stochastic Superoptimization*,
  ASPLOS 2013 — graded Hamming correctness, two-phase search, fixed-length nop
  buffers.
- `architecture.md` §12 for the full reference list.