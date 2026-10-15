# Building and Running

## Requirements

- Linux (developed on Gentoo Linux)
- A C++17 compiler (`g++` tested; `clang++` should work)
- GNU `make`
- No third-party libraries (no Z3 yet — see process.md)

## Build

```bash
make clean && make
```

This compiles `src/*.cpp` into `superopt`. Compiler flags:
`-std=c++17 -Wall -Wextra -O2 -g -Isrc`. A clean build prints no warnings.

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
| `--iters N`   | 10000 | number of MCMC iterations (metropolis steps) |
| `--beta B`    | 1.0   | inverse temperature (see math.md §3). Try `0.5`–`5`. |
| `--chains C`  | 1     | accepted for CLI-compatibility; **only 1 chain runs today** |
| `--seed S`    | 12345 | RNG seed for test generation and mutator |
| `--expr TEXT`, `-e TEXT` | – | compile the expression `TEXT` (see [frontend.md](frontend.md)) and optimize it |

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
best_cost=1 best_eq=0 best_perf=1 time=0.1694
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
| `best_perf` | instruction count of the best program (NOPs excluded) |
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
- `--chains` is accepted but not implemented; don't rely on it for better
  results.

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