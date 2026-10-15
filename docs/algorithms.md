# Algorithms

This page describes each algorithm in the pipeline at the level needed to
modify or extend the code. Math is in [math.md](math.md), file walkthroughs in
[code.md](code.md).

---

## 1. Test-case generation (`testgen.cpp`)

**Goal:** a small set of input states that distinguishes equivalent from
non-equivalent programs quickly, plus a reference output per case.

**Input construction** per test case `i`:

1. Draw `N = 32` independent random 32-bit constants; put one in each register.
2. Force `x0 = 0` (hardware invariant).
3. For each live-in register `r_j`: if `i < 64`, set `x[r_j]` to the
   `((j*3 + i) mod 8)`-th boundary value; else to a fresh random value.

```text
boundary values: 0, 1, -1, INT_MIN, INT_MAX, 0x55555555, 0xAAAAAAAA, 31
```

**Reference output:** execute the target once per case; record live-outs.

The first 64 cases deliberately cover the sign/zero/overflow/carry boundaries;
the rest are random. Non-live-in registers are random so reading an
uninitialised register is observable.

> Extending: add cases where inputs are drawn from a distribution that
> stresses specific opcodes (e.g. `x` and `x-1` for `and`-clearing idioms).

---

## 2. Interpreter (`interpreter.cpp`)

Sequential execution, one instruction at a time over a 32-word register array.
All values `uint32_t`; signedness is applied *locally*:

| opcode class | implementation |
|---|---|
| add/sub/logic | `uint32_t` arithmetic → wraparound modulo 2^32 |
| shifts | amount `& 31`; `<<`/`>>` are well-defined on `uint32_t` |
| sra / srai | explicit arithmetic shift in `uint32_t` (sign-fill) |
| slt / slti | cast both to `int32_t`, compare as signed |
| sltu / sltiu | compare `uint32_t < uint32_t` (immediate sign-extended first) |
| lui | `imm20 << 12` |

Perf note: `run_program` is a tight loop — a single small switch, no allocation,
no exceptions — designed so it runs millions of instructions/sec because the
MCMC loop calls it `num_tests × |candidate|` times per step.

---

## 3. Sorting the search space: cost (`cost.cpp`)

Term A — **equivalence distance** (graded correctness):

```text
eq(P) = Σ over test cases t  Σ over live-outs r   popcount( P_t[r] XOR target_t[r] )
```

- `0` ⇔ P behaves like the target on all tests.
- Otherwise the magnitude encodes *how many output bits* differ, so the search
  is guided downhill even when no equivalent program is in reach yet (this is
  the trick that makes MCMC superoptimization sample useful programs instead of
  searching exhaustively).

Term B — **performance**: currently `perf(P) = #non-nop instructions`.

Total: `cost(P) = 1000·eq(P) + perf(P)` (weights `w_c = 1000`, `w_p = 1`).

---

## 4. Mutator (`mutator.cpp`) — how new programs are proposed

Programs are **fixed-length buffers**; unused slots hold `nop`. This makes
insert/delete a trivial slot rewrite and — crucially — keeps every move
*symmetric*.

Four moves, chosen uniformly (default probabilities 0.3/0.3/0.2/0.2):

1. **Opcode change** — pick slot `i`, rewrite it with a random opcode of the
   *same format* (R↔R, I↔I, IShift↔IShift, U↔U). Operands remain register- and
   immediate-valid by construction.
2. **Operand change** — pick slot `i`, change one field: `rd`/`rs1`/`rs2` drawn
   from the register pool, or `imm` from the immediate pool.
3. **Swap** — swap the instruction in slots `i` and `j`.
4. **Replace** — fill slot `i` with a fully random instruction (10% `nop`).

**Register pool** (checked in `mutator.cpp::choose_from_pool` and built in
`main.cpp`): `live_in ∪ live_out ∪ temps`. Keeping the pool to ~5–10 registers
instead of 32 is what makes the search tractable.

**Immediate pool** (arch §4.3): `{0, 1, -1, 2, 4, 8, 16, 31, 32, 255, 2047,
-2048}`. Drawing uniformly from all 4096 immediates would almost never hit a
useful value.

---

## 5. MCMC search (`mcmc.cpp`) — Metropolis–Hastings

```text
cur ← all-nop buffer
for i in 1..max_iters:
    cand ← copy(cur); mutate(cand)
    Δ ← cost(cand) − cost(cur)
    u ← Uniform(0,1)
    accept ← (Δ ≤ 0) or (u < exp(−β·Δ))
    if accept: cur ← cand
    track best (lowest cost visited)
```

Properties:

- **Correctness bias by construction** — because `cost = 1000·eq + perf` and
  `perf < 1000` for any realistic buffer, the global optimum of `cost` is always
  a *correct* program with fewest instructions.
- **β is the inverse temperature.** Large β → only downhill moves, risks
  getting stuck; small β → frequent uphill moves, explores more. `--beta 2`
  is a reasonable default; try `0.5…5`.
- **No proposal correction** — see [math.md](math.md#why-no-proposal-ratio).

Unimplemented STOKE accelerators (arch §4.2, §4.4): two-phase search,
early-exit on the accept threshold, and parallel chains.

---

## 6. Probabilistic verification

The search's 32 test cases are *training* data and can be overfit (e.g. a
program that is accidentally right on boundary values). Verification therefore
uses a **fresh, larger** test set:

- generate `10 000` cases with `fixed_boundaries=false` (pure random live-ins),
- re-run `eq_dist` for the best program,
- report correct only if `eq = 0`.

This is the "probabilistically verified" Tier‑1.5 in `architecture.md` §4.5;
the exact Z3 `QF_BV` tier is future work.

---

## 7. Reference: `architecture.md` roadmap vs. implementations

| Algorithm (arch) | Status |
|---|---|
| enumerative baseline (length ≤ 3) | not implemented |
| oracle sanity tests (dependency DAG, schedule) | not implemented (no simulator yet) |
| feature extraction + regression model | not implemented (`use_regression` is a placeholder) |
| counterexample loop (add failing Z3 case, resume) | not implemented |