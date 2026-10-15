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

## 6. References and further reading

- E. Metropolis et al., *Equation of State Calculations by Fast Computing
  Machines*, J. Chem. Phys. 21 (1953); W.K. Hastings, Biometrika 57 (1970) —
  the sampler.
- E. Schkufza, R. Sharma, A. Aiken, *Stochastic Superoptimization*,
  ASPLOS 2013 — graded Hamming correctness, two-phase search, fixed-length nop
  buffers.
- `architecture.md` §12 for the full reference list.