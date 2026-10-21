# Building and Running

## Requirements

- Linux (developed on Gentoo Linux)
- A C++17 compiler (`g++` tested; `clang++` should work)
- GNU `make`
- No third-party libraries are required for a normal build. **Z3 is optional**:
  put `z3` on `PATH` and pass `--z3` for exact, proven verification. Without it
  the verifier falls back to probabilistic testing (see [Caveats](#caveats)).
- Python 3 is needed **only** to retrain the learned cost model (`make model`);
  the committed model builds with the C++ compiler alone.

## Build

```bash
make clean && make
```

This compiles `src/*.cpp` into `superopt`. Compiler flags:
`-std=c++17 -Wall -Wextra -O2 -g -Isrc`. A clean build prints no warnings.

### Retraining the learned cost model

A trained GBDT model is committed in `src/model_data.hpp`, so the default build
needs no Python. To regenerate it:

```bash
make dataset   # label 20k random blocks with the simulator -> results/dataset.csv
make model     # fit the GBDT and rewrite src/model_data.hpp
make           # rebuild superopt with the new model
```

`tools/train_model.py` uses only the Python standard library.

## What is being optimized

Input is a **loop-free, register-only basic block** in the reduced RV32I subset
(20 opcodes plus `nop`; no loads/stores, branches, mul/div, or floating point).
The optimizer searches for a **cheaper program with identical live-out
register values for every input**.

There are two distinct meanings of "cheaper", selected by `--cost-mode`:

- **mode 0 (default)** — fewer non-NOP instructions. This is the classic
  superoptimization objective.
- **modes 1/2/3** — fewer estimated **execution cycles**: 1 = learned
  gradient-boosted model, 2 = static bundling (DAG + height-ranked list
  scheduling), 3 = dynamic Tomasulo + ROB. These can prefer *different*
  programs than instruction count, because an extra independent instruction may
  hide latency.

Correctness always dominates performance: the total cost is
`cost = 1000·eq_distance + perf`, and the graded equivalence distance
`eq_distance` is the Hamming distance between candidate and target live-outs
summed over test cases. Because any non-equivalent program has `eq ≥ 1`
(so cost ≥ 1000) while any equivalent program has cost < 1000 for realistic
sizes, the global optimum is **always a correct program** — the perf term only
breaks ties among correct programs. See [algorithms.md](algorithms.md) and
[math.md](math.md).

Only **live-out** registers are compared. Live-in registers are inputs; every
other register is a scratch value and may be freely rewritten.

## How it handles the flow

End to end:

1. **Parse** the target (`.sopt` header + code, or an `--expr` block) into a
   `ProgramSpec` with `live_in`, `live_out`, `code`, and a register pool
   `live_in ∪ live_out ∪ temps`.
2. **Generate tests** — a small set of input states (boundary values first, then
   random) and run the target once per case to record reference live-outs. These
   cases are the *training data* the search scores against.
3. **Search** — Metropolis–Hastings MCMC. Start from an all-`nop` buffer (or a
   supplied start program), repeatedly copy → mutate (opcode/operand/swap/
   replace) → score → accept or reject by the Metropolis rule. `--chains K`
   runs K independent chains and keeps the best. Track the lowest-cost program
   visited.
4. **Verify** — the best program's `best_eq` is re-checked at the end:
   - With `--z3` and Z3 on `PATH`: encode both programs as SMT `QF_BV` formulas
     and ask whether any input makes a live-out differ. `unsat` = proven
     equivalent. On `sat`, the model is a counterexample: it is added to the
     test set and the search resumes from the current best, up to 5 rounds.
   - Otherwise: re-run the best program on a **fresh, larger** test set (uniform
     random + small-signed-integer biased) and report `OK (probabilistic)` if
     every case matches, else `FAILED`.
5. **Optional baseline** — `--enum` exhaustively enumerates every length-1..N
   program over the register/immediate pools, filters by the test set, and
   reports the ground-truth optimum for short targets, so you can check whether
   MCMC missed it.

`--cost-mode` only changes step 3's performance term; the flow is otherwise
identical. `--z3` and `--enum` are post-search steps and do not change the
search itself.

## Caveats

- **Search is stochastic.** Same target with different `--seed` can give
  different results. Runs are reproducible given the same `--seed`, `--iters`,
  `--beta`, and `--chains`; sweep a few seeds and keep the best.
- **Probabilistic verification is not proof.** Without `--z3`, `verify: OK`
  means "no counterexample in ~8k fresh tests", not equivalence. A wrong program
  could still pass; use `--z3` when you need certainty.
- **Uninitialized reads can look like wins.** Registers that are neither
  live-in nor written by a candidate are set to fresh random values per test, so
  the search can occasionally "succeed" by reading one. Read the output block
  and check `best_eq` — do not trust the `verify:` line alone.
- **No `best_eq == 0` means no answer.** If `best_eq != 0` the reported program
  is *not* equivalent; no `verify:` line is printed. Lower `--beta`, raise
  `--iters`, add `--chains`, or try another seed.
- **`beta` trades exploration vs. greed.** High `β` converges fast but can stick
  in local minima; low `β` roams but may not settle within `--iters`. See the
  sections below.
- **The mutator pool is restricted.** It draws from `live_in ∪ live_out ∪ temps`
  (and excludes `x0`), so a rewrite that needs a constant must materialize it
  from the inputs or a touched register rather than `addi rd, x0, k`.
- **Immediates come from a fixed small pool** (`{0,1,-1,2,4,8,16,31,32,255,
  2047,-2048}` plus values seen in the target), so some optima are unreachable
  by design (e.g. `clearlow` cannot reach `andi a0,a0,-2`).
- **`--chains` is sequential.** Multiple chains improve coverage but cost
  wall-clock time linearly; there is no threading yet.
- **The learned model is only valid for its training config.** `--cost-mode 1`
  predicts cycles for the fixed issue width / ROB / RS configuration the
  dataset was labeled under; a different microarchitecture needs `make model`
  regenerated with a matching simulator config.
- **`--enum` length is capped at 4.** Exhaustive search grows explosively with
  length; beyond a handful of instructions use MCMC or `--cost-mode`.

## Running

Three modes:

```bash
./superopt <file.sopt> [options]                  # superoptimize a .sopt target
./superopt --expr "<expr block>" [options]        # compile an expression on the fly, then optimize it
./superopt [options]                              # interactive REPL
```

Options:

| flag | default | meaning |
|---|---|---|
| `--iters N`   | 10000 | number of MCMC iterations per chain (metropolis steps) |
| `--beta B`    | 1.0   | inverse temperature (see math.md §3). Try `0.5`–`5`. |
| `--chains C`  | 1     | run C independent chains and keep the best (sequential today) |
| `--seed S`    | 12345 | RNG seed for test generation and mutator |
| `--expr TEXT`, `-e TEXT` | – | compile the expression `TEXT` (see [frontend.md](frontend.md)) and optimize it |
| `--cost-mode M` | 0 | perf term: 0=insn count, 1=GBDT cycles, 2=bundled sim, 3=Tomasulo sim |
| `--enum`      | off   | run the exhaustive baseline for short targets |
| `--enum-max-len N` | 3 | maximum enumerative length (hard cap 4) |
| `--z3`        | off   | prove equivalence with Z3 and resume on counterexamples |

Examples from the repo:

```bash
./superopt bench/toy/smoke.sopt --iters 300000 --beta 2.0 --seed 123
./superopt --expr "t3 = (a0 + a1) - a1 << 1;" --iters 300000 --beta 2.0 --seed 1
./superopt                                            # then type expressions at the prompt
```

For fast run-through of every example:

```bash
for f in bench/toy/*.sopt; do echo "=== $f ==="; ./superopt "$f" --iters 300000 --beta 2.0 --seed 123; done
```

## Interpreting the output

```text
best_cost=1 best_eq=0 best_perf=1 best_insns=1 time=0.1694
target program (3 insns):
  add x6, x10, x11
  sub x7, x6, x11
  slli x28, x7, 1
optimized program (1 insn):
  add x28, x10, x10
verify: OK (probabilistic) fresh_rand_eq=0
```

| token | meaning |
|---|---|
| `best_cost` | `1000·eq + perf` of the best program found |
| `best_eq`   | equivalence distance on the search tests; `0` = equivalent on them |
| `best_perf` | performance term of the best program (insns or cycles, per `--cost-mode`) |
| `best_insns`| instruction count of the best program (NOPs excluded) |
| `time`      | wall-clock seconds of the MCMC loop |
| `target program (N insns)` | the input block with NOP filler stripped |
| `optimized program (N insns)` | the best program found, NOP filler stripped |
| `verify:`   | re-check of the best program on fresh tests (4096 uniform random + 4096 small-signed-integer biased): `OK (probabilistic)` or `FAILED fresh_rand_eq=…` |

> If a smaller program has not been found (`best_eq != 0`), no `verify:` line is
> printed and the reported program is *not* equivalent — inspect `best_eq` first.

**Expected results for the bundled targets** (with `--iters 300000 --beta 2`):

| target | found optimum |
|---|---|
| `bench/toy/simple.sopt` | identity → all `nop`s (correct: `(a+b)-b == a`) |
| `bench/toy/addshift.sopt` | `add x10, x10, x10` (`2·a`) |
| `bench/toy/smoke{,2,3}.sopt`, `multi.sopt` | `add x28, x10, x10` (`t3 = a0<<1`) |

## More example targets

`bench/toy/` also ships a set of small, self-contained "algebra" targets. Each is
a sequence of instructions whose live-out value is a simple function of the
live-ins; the search must recover the short form. Run them all with:

```bash
for f in bench/toy/*.sopt; do echo "=== $f ==="; \
  ./superopt "$f" --iters 1000000 --beta 1.0 --seed 1; done
```

The table below shows the instruction sequence in each file and the optimum the
threshold above reliably finds (all `verify: OK`). `a`=`a0`=x10, `b`=`a1`=x11.

| target file | target sequence | live-out | found optimum |
|---|---|---|---|
| `xorzero.sopt` | `xor a0,a0,a0` | `a^a = 0` | `xor x10,x10,x10` (0), 1 insn |
| `doubleneg.sopt` | `sub t1,x0,a0` ; `sub a0,x0,t1` | `-(-a) = a` | identity, **0 insns** |
| `xorcancel.sopt` | `xor t1,a0,a1` ; `xor a0,t1,a1` | `(a^b)^b = a` | identity, **0 insns** |
| `orzero.sopt` | `addi t1,x0,0` ; `or a0,a0,t1` | `a|0 = a` | identity, **0 insns** |
| `addcancel.sopt` | `add t1,a0,a1` ; `sub a0,t1,a0` | `(a+b)-a = b` | `slli x10,x11,0` (`b`), 1 insn |
| `invertsub.sopt` | `sub t1,a0,a1` ; `sub t2,x0,t1` ; `add a0,t2,a0` | `-(a-b)+a = b` | `and x10,x11,x11` (`b`), 1 insn |
| `andidem.sopt` | `and t1,a0,a1` ; `and t2,a0,a1` ; `or a0,t1,t2` | `(a&b)|(a&b) = a&b` | `and x10,x10,x11`, 1 insn |
| `shiftcomb.sopt` | `slli t1,a0,3` ; `slli a0,t1,2` | `(a<<3)<<2 = a<<5` | `slli x10,x10,5`, 1 insn |
| `sracomb.sopt` | `srai t1,a0,4` ; `srai a0,t1,3` | `(a>>4)>>3 = a>>7` | `srai x10,x10,7`, 1 insn |
| `clearlow.sopt` | `srli t1,a0,1` ; `slli a0,t1,1` | `(a>>1)<<1 = a&~1` | `andi x6,x10,1` ; `xor x10,x6,x10`, 2 insns |
| `demorgan.sopt` | `xori t1,a0,-1` ; `xori t2,a1,-1` ; `and a0,t1,t2` | `~a&~b = ~(a|b)` | `or x11,x10,x11` ; `xori x10,x11,-1`, 2 insns |

Notes on what you see:

- Several targets collapse to **zero** instructions — the block is the identity
  on its live-out, so the cheapest correct program is the empty buffer. This is
  expected, not a bug (see `simple.sopt` in the table above).
- `clearlow` cannot reach 1 insn (`andi a0,a0,-2`): the mutator only draws
  immediates from a fixed small set that does not include `-2`, so the two-insn
  `a ^ (a&1)` form is the reachable optimum.
- **Search is stochastic and `beta` matters.** `andidem` and `demorgan` fail to
  reach `eq=0` at the README setting (`--iters 300000 --beta 2`) because the
  chain gets stuck; they succeed at `--beta 1.0` with more iterations. This is
  exactly the high-`beta` local-minimum trap described below.
- The mutator pool excludes `x0` (see `derive_temps` in `src/main.cpp:58`), so a
  target that wants a real constant must materialise it from the inputs, not via
  `addi rd, x0, k`.

> **Caveat — uninitialised reads.** Registers that are neither live-in nor
> written by a candidate are initialised to fresh random values per test. The
> search will occasionally "win" by reading such a register if the random value
> happens to give the right answer. `verify:` catches most of these, but not all
> (the failure region can be astronomically rare for uniform inputs). Always
> sanity-check an optimized block by reading it, and keep an eye on `best_eq`,
> not just the `verify:` line.

## Writing your own target

1. Pick `live_in` (inputs) and `live_out` (only these must match afterwards).
2. Write the target block with `xN`, ABI, or numeric register names
   (comma-optional syntax; see [format.md](format.md)).
3. Save as `my.sopt`:
   ```text
   live_in: {a0, a1}
   live_out: {a0}
   code:
   add a0, a0, a1
   sub a0, a0, a1
   ```
   The optimiser may legally return zero instructions here because the block is
   the identity on `a0`.
4. Run `./superopt my.sopt --iters 200000 --beta 2.0 --seed 1`.

Ground rules that make targets well-posed:

- Only the 20 v1 opcodes (see [format.md](format.md)); no loads, stores,
  branches, mul/div, or PC-dependent ops.
- If an input or output register is *also* used as a scratch register, list it
  in `live_in`/`live_out` so it enters the mutator pool.
- `--chains` runs independent chains sequentially; it improves coverage at a
  linear cost in wall-clock time.

## Tuning the search

- Raise `--iters` for bigger/longer targets (search time is linear in iters).
- The search is stochastic: run a few `--seed`s and take the best report.

## What `--beta` and `--seed` mean

These two flags control *how* the MCMC search runs. Neither changes the problem
being solved; they only steer the random walk.

### `--beta` — inverse temperature

`beta` (β) is the **inverse temperature** of the Boltzmann distribution the
sampler targets (see [math.md](math.md) §3):

```text
π(program) ∝ exp( −β · cost(program) ),      β > 0
```

It sets how eagerly the chain accepts a *worse* program. When the proposed move
raises the cost by `Δ = cost(new) − cost(old) > 0`, it is accepted with
probability:

```text
accept probability = exp( −β · Δ )
```

- **Small β (high temperature, e.g. `0.5`)** → worse moves are accepted often;
  the chain roams widely, escapes local minima, but may not settle on the best
  program within `--iters`.
- **Large β (low temperature, e.g. `5`)** → worse moves are almost always
  rejected; the chain behaves like greedy hill-climbing. It converges fast but
  can get **stuck in a local minimum** and never find the global optimum.
- **β → 0** is uniform random search; **β → ∞** is pure hill-climbing.

Practical range is roughly `0.5`–`5`. The README examples use `2.0`; for the
trickier algebra targets above, `1.0` with more iterations works better. If a
run reports `best_eq != 0`, the usual fix is: lower `β`, raise `--iters`, or try
another seed.

### `--seed` — RNG seed

`seed` initialises the pseudo-random number generators that drive the whole
search, so that **runs are reproducible**:

- the **test-case generator** (the random inputs scored against), and
- the **mutator** (which instruction/slot/operand to change each step).

Consequences:

- Same target + same `--iters`/`--beta` + same `--seed` ⇒ identical output every
  time.
- A different `--seed` produces a different random trajectory, so it may find a
  different (better or worse) program. Best practice: sweep a few seeds and keep
  the best result.
- Default is `12345`. Passing `--seed 0` makes the *test-generation* RNG seed
  from the wall clock (see `init_random_seed` in `src/testgen.cpp:12`), i.e. a
  non-reproducible run — avoid it when you want to compare runs.

`beta` decides *how greedy* the walk is; `seed` decides *which* walk you get.
`iters` decides how long you walk.