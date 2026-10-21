# MCMC in the Superoptimizer

## 1. What MCMC is, in one paragraph

MCMC stands for **Markov chain Monte Carlo**: a way to *sample* from a
probability distribution when you cannot compute it directly. You build a
random walk over the state space (the *Markov chain*), whose steps are so
designed that over a long run the sequence of visited states behaves as if it
were drawn from the target distribution (**Monte Carlo**).

Here the "distribution" is a Boltzmann distribution over *programs*:
programs with low cost are vastly more likely than programs with high cost. So
a long chain spends most of its time near the cheapest programs, and we report
the cheapest one visited.

---

## 2. Common misconception: it is a random walk, not a tree search

The user question framing — "random sampling establishes nodes and then
explores paths" — describes a graph/tree traversal (DFS, BFS, Dijkstra). MCMC
does **not** build a node graph up front and then explore it. There is no
explicit graph, no frontier, no visited set.

Concretely, in this codebase (`src/mcmc.cpp`):

- the "state" is simply a fixed-length array of `Insn`s (a whole program);
- the "neighbors" of a state are not enumerated — one random one is generated
  on the fly by `mutate_program` each step;
- the chain remembers **only the current program** and the **best program so
  far**; everything else is forgotten immediately;
- it is a *walk*, not a *search*: it may revisit the same program many times
  (nothing remembers that it saw it), and it does not accumulate any global
  picture of the space.

```
iteration  state (the only thing stored)
─────────  ─────────────────────────────
0          [nop nop nop]
1          [nop nop add a0,a0,a0]      ← random mutation of previous
2          [nop sub t0,x0,x0 add …]    ← random mutation of that
3          [nop sub t0,x0,x0 add …]    ← proposal rejected, stayed
4          [add x28,x10,x10 nop nop]   ← jumped again
…
```

Every step is `propose a neighbor → decide accept/reject → maybe move`.
The previous state is gone once we step.

---

## 3. The language and notation

| | |
|---|---|
| state `x` | a full program: an array of `L` `Insn`s (slots) |
| state space `Ω` | all `L`-slot programs over the opcodes/pool |
| proposal `q(x'|x)` | probability that the mutator turns `x` into `x'` |
| acceptance `α(x,x')` | probability we actually move `x→x'` given the proposal |
| cost `C(x)` | `1000·eq(x) + perf(x)` |
| temperature `1/β` | controls how much uphill movement is allowed |
| iteration/step | one propose-and-accept cycle; `--iters` count these |

---

## 4. The two steps of the walk

The walk alternates exactly two operations.

### Step 1 — proposal (a *random sample* of a neighbor)

From the current program `x`, copy it and apply **one random mutation**

```
cand ← copy(x)
mutate_program(spec, cand)      // one of 4 moves, see below
```

The four moves (from `src/mutator.cpp`), one chosen uniformly:

| move | change |
|---|---|
| opcode | rewrite one slot with a random op of the same format |
| operand | change rd/rs1/rs2/imm of one slot |
| swap    | exchange the contents of two slots |
| replace | fill one slot with a fully random instruction (10% chance of `nop`) |

Because only the current program is used, the next candidate depends **only on
where we are now** — the defining Markov property. And note the proposal is
*local* (one slot differs): the chain explores via small steps, similar in
spirit (but not in mechanism) to how a search would expand nearby nodes.

### Step 2 — acceptance (keep the move or reject it)

Score the candidate and compare against the current program:

```
Δ  = C(cand) − C(cur)
u  ~ Uniform(0, 1)
if Δ ≤ 0 or u < exp(−β·Δ)  →  accept (cur ← cand)
else                        →  reject (stay; discard cand)
```

- **Downhill moves (`Δ ≤ 0`)** are always taken: the walk tends toward cheaper
  programs.
- **Uphill moves (`Δ > 0`)** are taken *randomly* with probability
  `exp(−β·Δ)`: worse programs have a small but non-zero chance of being
  accepted. This is what lets the walk escape local minima.
- **β** decides how "unlikely" uphill steps are: β=0 → all steps accepted
  (pure random walk, no optimisation); β→∞ → never accept worse (greedy,
  gets stuck). `2.0` is a compromise.

There is **no expansion and no pruning** — a rejected candidate is simply
discarded; we never try other neighbors from the same node.

---

## 5. Connecting the two ideas: "nodes" and "paths"

You can *think* of the space as a graph where:

- **nodes = programs** (`Ω`),
- **edges = single mutations** (two programs joined if one mutator move turns
  one into the other),
- walking the chain **traverses edges of that implicit graph**.

But the algorithm never materialises the graph:

| graph search (BFS/Dijkstra) | MCMC walk (this code) |
|---|---|
| enumerate all neighbors of a node | generate one random neighbor |
| keep frontier / visited set | keep one `cur` program |
| guarantee shortest path | no guarantee, probabilistic |
| cost fixed for edges | cost is per-state, acceptance is random |
| finds the optimum | visits good states; reports best seen |

So: *sampling* is the **proposal**; a *path* emerges afterwards as the
accepted sequence, but it is a by-product of the walk, not an exploration plan.

---

## 6. Two-phase intuition: why cost decreases on average

The acceptance rule makes the chain sample from the **Boltzmann distribution**

```
π(x) ∝ exp(−β · C(x))
```

States with lower cost have exponentially larger stationary probability.
Because the walk is *ergodic* (from any program the mutator can reach any
other program in finitely many steps), the fraction of time the chain spends
near cost `c` approaches `π`-weighted mass — most time near low cost.
In particular, since `C = 1000·eq + perf`:

- far from a solution, the cheapest/correct-reaching direction is the one that
  makes more output bits match (`eq` decreases);
- once a correct program is found (`eq = 0`), `C = perf`, so the walk
  preferentially discovers correct programs with fewer instructions.

The reported answer is the **minimum-cost state ever visited**, not the current
state. That is why longer `--iters` generally (not always) gives shorter
programs, and why different `--seed`s give different results: the walk is
random, so it is not guaranteed to reach the global minimum.

---

## 7. Determinism, parameters and the random source

`mcmc.cpp` runs `--chains K` independent chains sequentially and keeps the
best result. Each chain derives its proposal and acceptance streams from
`seed + chain_index * constant`:

```
seed + chain k  → mutator rng and std::mt19937 acceptance rng
test cases      → from testgen's own rng, same base seed
every iteration → copy, mutate (rng), u ~ Uniform(0,1) (rng)
```

Same `--seed` and chain count ⇒ same rng sequences ⇒ identical accept/reject
decisions ⇒ fully reproducible run. Different seed ⇒ different trajectory and
different local minima visited ⇒ possibly different (better or worse) answer.

The randomness enters exactly three places:

1. choosing a test case input,
2. choosing which mutation to apply and its random operand values,
3. drawing `u` for the accept/reject decision.

The cost itself is deterministic: `cost(program)` always returns the same
number for the same inputs.

---

## 8. When is the answer trustworthy?

- **Equivalence**: `best_eq = 0` over the 32 search tests, then
  `verify: OK (probabilistic)` over 10,000 fresh random inputs. The result is
  very unlikely to be *wrong*.
- **Optimality**: *not* guaranteed — there is no proof the found program is the
  shortest possible. Optimality would require the (not yet implemented)
  exhaustive enumerator of `architecture.md` §4.6.
- **Practical check**: run several seeds and keep the best program.

---

## 9. Worked walk-through

Everything below is the *actual* trace of `superopt bench/toy/smoke.sopt
--iters 300000 --beta 2.0 --seed 123` (simplified; the chain skips thousands
of mid-steps). Target: `t3 = ((a0+a1) − a1) << 1 = a0 << 1`, pool derived as
`{a0, a1, t3, t1, t2}`.

```
state 0  [nop nop nop]              cost 300000-ish (eq 300·1000)
   |  mutate slot 3 → "sub t2,x0,x0"
state 1  [nop nop sub t2,x0,x0]     eq large, slightly different
   |  … many random small changes …
state k  [nop sub t1,a0,a1 slli t3,t1,1]            ← eq dropping
   |  u < exp(−βΔ) occasionally for uphill steps.
state m  [add t3,a0,a0 nop nop]     eq = 0, cost = 1   ← global optimum
   |  further proposals mostly rejected / revert
state n  [add t3,a0,a0 nop nop]     best kept = this    ← REPORTED
```

Reported:

```
best_cost=1 best_eq=0 best_perf=1 time=0.1196
target program (3 insns):        add x6,x10,x11 / sub x7,x6,x11 / slli x28,x7,1
optimized program (1 insn):      add x28,x10,x10
verify: OK (probabilistic) fresh_rand_eq=0
```

---

## 10. Advanced MCMC features (planned, not in this code)

STOKE and `architecture.md` describe refinements not yet implemented here:

- **Two-phase search**: first run with `perf` weight 0 (find *any* correct
  program), then re-run with `perf` weight active, starting from the result.
- **Early exit**: draw `u` up front and cut evaluation of a candidate short as
  soon as `eq` already exceeds the threshold implied by `u` (a large speedup).
- **Threaded chains**: `--chains` runs independent chains today, but
  sequentially rather than in parallel threads.
- **Regression-guided perf**: implemented as `--cost-mode 1` with a
  gradient-boosted model generated by `make model`.

---

## 11. Summary

- MCMC **walks**, it does not **search a developed graph**.
- "Random sampling" = generating one random neighbor (mutation) per step.
- "Exploring paths" = the sequence of accepted moves that happens as a
  by-product; nothing is recorded except the best program seen.
- The walk converges in distribution to low-cost programs, hence finds good
  (sometimes optimal) shortened programs, with no guarantee of optimality.
- `seed` makes it reproducible; `beta` balances exploration vs. greediness;
  `iters` budgets how much of the (infinite) walk you pay for.