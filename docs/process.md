# Process: End to End

This document describes the complete pipeline, from a hand-written `.sopt`
target file to a reported, verified optimized program. See also
[format.md](format.md) for the file syntax, [code.md](code.md) for where each
step lives in the source, and [math.md](math.md) for the underlying equations.

## 1. The problem

**Input.** A *program specification* (a `.sopt` file) containing:

- `live_in` — registers whose initial values are the program inputs,
- `live_out` — registers whose final values must match the target,
- a target basic block: an ordered list of RV32I instructions (register-only,
  loop-free, no loads/stores/branches).

**Output.** A program that:

1. is **correct** — on every input, every live-out register holds the same final
   value that the target program would produce, and
2. is **cheaper** — minimal among correct programs according to the performance
   term (currently: number of non-NOP instructions).

Correctness compares **only live-out registers**. Everything else is free
scratch; the search is not forced to reproduce dead values.

## 2. The pipeline

```
 target.sopt
     │
     ▼
 ┌──────────────────┐
 │  front end       │  parse header (live_in/live_out) + instructions
 │  (main.cpp)      │  → ProgramSpec { code, live_in, live_out, temps, max_len }
 └──────────────────┘
     │
     ▼
 ┌──────────────────┐
 │  test generator  │  build N=32 test cases: input states + target outputs
 │  (testgen.cpp)   │
 └──────────────────┘
     │
     ▼
 ┌──────────────────┐
 │  MCMC search     │  Metropolis–Hastings over fixed-length NOP-filled
 │  (mcmc.cpp)      │  buffers; cost = 1000·eq + insn_count (cost.cpp)
 └──────────────────┘
     │
     ▼
 ┌──────────────────┐
 │  verifier        │  re-score best program on 10 000 fresh random tests
 │  (main.cpp)      │  → "verify: OK (probabilistic)"
 └──────────────────┘
     │
     ▼
  report: best_cost / best_eq / best_perf / time + best program
```

### 2.1 Front end (parsing)

`src/main.cpp` reads the file line by line:

1. Strip whitespace and comment/blank lines.
2. While in the header, recognise `live_in:`, `live_out:`, and the `code:`
   marker. Register tokens may be `xN`, ABI names (`a0`, `t3`, `s0`…), or bare
   numbers; they are all resolved to the register index 0–31 (implementation:
   `reg_name_to_num` in `src/isa.cpp`).
3. After `code:`, each subsequent line is turned into an `Insn` by
   `parse_insn` (see [code.md](code.md#isa---the-instruction-set-table)).
   Unrecognised lines are reported and skipped.
4. Defaults: missing `live_in`/`live_out` default to register 10 (`a0`).

From the parsed target the front end also computes the **mutator register
pool**: the union of `live_in`, `live_out`, and every register the target
touches (`rd`/`rs1`/`rs2`) — a small pool (~5–9 registers), per `architecture.md`
§3. `x0` is deliberately excluded from the pool (writes to it are discarded;
only the simulator needs it). `max_len` is the target's instruction count, which
fixes the MCMC buffer length.

### 2.2 Test generation

`src/testgen.cpp` produces `N = 32` test cases:

- Every register starts as a **fixed random 32-bit constant per test case**
  (so a candidate that reads a register the target never wrote cannot hide the
  difference), except `x0 = 0`.
- Each live-in register is then set to a *boundary value* for the first 64
  indices (`0, 1, -1, INT_MIN, INT_MAX, 0x55555555, 0xAAAAAAAA, 31`, rotated by
  the live-in index) or a fresh random value otherwise.
- The **target program is executed** on each input state and the live-out
  registers are recorded (this is the correctness reference).

Why this matters: a candidate that is equivalent only *on zeros* will be
rejected once live-ins are non-zero, and a candidate relying on reading an
uninitialised register will be rejected because uninitialised registers are
non-zero and different per test.

### 2.3 MCMC search

`src/mcmc.cpp` runs a single Metropolis–Hastings chain (the `--chains` flag is
accepted but not yet honoured):

1. Start `cur` = the fixed-length buffer filled with `nop`.
2. Each iteration: copy `cur` → `cand`, apply one random mutator move, compute
   the cost `C(cand)`.
3. Accept with probability `α = min(1, exp(−β(C(cand) − C(cur))))` using a
   pre-drawn uniform random number.
4. Track the lowest-cost program ever visited as the result.

Because every move is symmetric (see [algorithms.md](algorithms.md#moves-are-symmetric)),
there is no proposal-ratio (Hastings) correction.

Other search strategies in the literature (STOKE two-phase synthesis/optimization,
early exit on the Metropolis threshold, multiple chains) are described in
[`architecture.md`](../architecture.md) §4.2/§4.4 but **not yet implemented** here.

### 2.4 Cost

`src/cost.cpp`:

```
cost(P) = 1000 · eq(P) + perf(P)
eq(P)    = Σ_t Σ_{r ∈ live_out} popcount( P_t[r] XOR target_t[r] )
perf(P)  = number of non-NOP instructions in P
```

`eq` is the STOKE-style *graded* correctness distance: it is 0 iff the
candidate matches the target on every test, and grows with how many output bits
differ. Multiplying `eq` by 1000 guarantees that *any* correct program
(`cost < 1000`) outranks *any* incorrect program (`cost ≥ 1000`), so among
correct programs the search minimises instruction count.

### 2.5 Verification

Before reporting, the best program is re-executed on **10 000 fresh random test
cases** (not the 32 used during search, and not the boundary values). If all
live-outs match, the result is printed as `verify: OK (probabilistic)`.

This is a *probabilistic* check: it never *proves* equivalence, it only makes
counterexamples vanishingly unlikely. `architecture.md` §4.5 specifies an exact
Z3 `QF_BV` tier that is not yet wired in.

## 3. Status vs. `architecture.md`

| architecture.md feature | Status here |
|---|---|
| v1 ISA table (20 ops, `kOps[]`) | implemented — `src/isa.cpp` |
| Bit-exact interpreter | implemented — `src/interpreter.cpp` |
| Test generation + graded `eq` | implemented — `src/testgen.cpp`, `src/cost.cpp` |
| Fixed-length NOP mutator, 4 moves | implemented — `src/mutator.cpp` |
| MCMC (Metropolis–Hastings) | implemented; `--chains K` runs K independent chains — `src/mcmc.cpp` |
| `perf = instruction count` (phase A) | implemented (default `--cost-mode 0`) |
| Two-phase search, early exit | not implemented |
| Z3 Tier-2 verifier + counterexample loop | implemented (`--z3`), probabilistic fallback without Z3 — `src/verify.cpp` |
| Regression model / simulator oracle | implemented — `src/sim.cpp`, `src/features.cpp`, `tools/` |
| Enumerative baseline | implemented (`--enum`) — `src/enumerate.cpp` |
| gcc/clang front end | not implemented |

The scoreboard above matches the realistic build-order in `architecture.md` §10,
steps 1–5 (interpreter → tests → eq → mutator → MCMC) with the smoke-test as the
exit criterion.