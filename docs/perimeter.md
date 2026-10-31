# Perimeter: Search Limits, Success, and Optimality

This page uses **perimeter** for the boundary of a search run: the set of
programs a strategy can reach within its budget. The perimeter is shaped by the
target's buffer length `L`, its shortest correct program length `l*`, the
iteration/time budget, and the search parameters below.

The practical question is always the same:

- Did the run cross the perimeter and find a **correct** program?
- Did it also find the **shortest** correct program?

Those are different questions. The first is **success**, the second is
**optimality**.

## 1. The two metrics

For a candidate program `P`, let `eq(P)` be the graded correctness distance
(Hamming distance over live-out bits on the test set), and let `#insns(P)` be
the number of non-NOP instructions.

```text
success(P) = 1[ eq(P) = 0 ]
optimal(P) = 1[ eq(P) = 0 and #insns(P) = opt ]
```

Here `opt` is the known optimum for the target. In the benchmark, `opt` is known
by construction (`multi N` needs `N` instructions; `padded P` needs 1).

| Outcome | `best_eq` | `best_insns` | Meaning |
|---|---:|---:|---|
| failure | `> 0` | any | No correct program found; `best_insns` describes an incorrect candidate. |
| success, not optimal | `0` | `> opt` | Correct, but longer than the optimum. |
| success and optimal | `0` | `= opt` | Correct and shortest. |
| timeout / no result | n/a | n/a | Run did not finish; counted as failure in benchmark rates. |

Important details:

- `best_insns = 0` on a failed run usually means the best candidate was still
  the all-NOP buffer. It does **not** mean an empty program was accepted.
- `best_insns` is only a solution length when `best_eq = 0`.
- Success is measured on the search test set. A later fresh-test or Z3 check can
  still fail, so always read the `verify:` line.
- For `--cost-mode 0`, "optimal" means minimum instruction count. For
  `--cost-mode 1/2/3`, the objective is predicted/simulated cycles, so
  instruction count alone is not the optimization objective.
- Over `n` seeds, the benchmark reports
  `success_rate = (# successes)/n` and
  `optimality_rate = (# correct-and-optimal)/n`.
  Timeouts count in the denominator as failures.

### Success is not optimality

`bench/examples/long_shallow_100.sopt` is the clean example:

- The target has 100 instructions, but the optimum is **1** (`add a0,a0,a0`).
- `enum` finds it immediately: `best_eq=0`, `best_insns=1`.
- At 300k iterations, `hc` finds a correct **3**-instruction program
  (`best_eq=0`, `best_insns=3`): success, but not optimality.
- At 300k iterations, fixed `mcmc` finds a correct **44**-instruction program
  on seed 1: success, but not optimality.
- At 10k iterations, adaptive MCMC and HC-MCMC both return `best_eq=305`,
  `best_insns=0`: neither success nor optimality.

This is why a run can be reported as successful without being optimal. MCMC
minimizes `1000*eq + perf`; once a correct program is found the `1000*eq` term
is zero, but the chain may never visit the shorter correct program within its
budget. Enumeration instead tries lengths `0, 1, 2, ...`, so the first correct
program it finds is also the shortest (within its length cap).

## 2. Parameters that move the perimeter

### MCMC parameters

| Parameter | Default | Effect |
|---|---:|---|
| `--beta B` | `1.0` | Fixed inverse temperature. Higher = greedier (accepts fewer uphill moves); lower = more exploration. |
| `--iters N` | `10000` | Number of proposals. More iterations widen the reachable perimeter but cost time linearly. |
| `--chains C` | `1` | Runs `C` sequential independent chains and keeps the best result. More chains reduce seed variance. |
| `--seed S` | `12345` | Controls test generation and mutation randomness. Success/optimality are seed-dependent. |
| `--cost-mode M` | `0` | `0` = instruction count, `1` = GBDT cycles, `2` = bundled cycles, `3` = Tomasulo cycles. |

### Adaptive MCMC parameters

Adaptive MCMC changes `beta` online so the acceptance rate tracks a target.

```text
every adapt_window proposals:
    beta <- clamp(beta * exp(adapt_gain * (observed_accept - target_accept)),
                  beta_min, beta_max)
```

| Parameter | Default | Effect |
|---|---:|---|
| `--target-accept A` | `0.234` | Desired acceptance rate. Higher target usually lowers `beta` and increases exploration. |
| `--adapt-window W` | `200` | Number of proposals between updates. Smaller = faster adaptation, noisier. |
| `--adapt-gain G` | `1.0` | Log-space step size. Larger = more aggressive adaptation. |
| beta bounds | `[1e-3, 1e3]` | Internal clamp; not currently a CLI flag. |

Adaptive temperature changes **acceptance behavior**, not the size or
dimensionality of the program buffer.

### HC-MCMC parameters

HC-MCMC accepts only non-worsening moves, then kicks the incumbent best when it
stalls.

| Parameter | Default | Effect |
|---|---:|---|
| `--hc-stall N` | `1000` | Iterations without a new best before a kick. |
| `--hc-kick K` | `4` | Random mutations applied to the incumbent best during a kick. |

### Enumeration parameters

| Parameter | Default | Effect |
|---|---:|---|
| `--enum-max-len N` | `3` | Maximum program length to enumerate; hard cap is `4`. |
| `--algo enum` | off | Use enumeration as the search strategy. |
| `--enum` | off | Run enumeration as a baseline after MCMC. |

Enumeration cost grows like `Θ(B^l*)`, where `l*` is the shortest correct
length and `B` is the instruction/operand branching factor. It is independent
of the target buffer length `L`.

### Buffer length and solution depth

These are not CLI flags, but they define the perimeter:

- `L` = target instruction count. This is the MCMC buffer length.
- `l*` = shortest correct program length. This is the enumeration depth.

For the benchmark families:

- `multi N`: `L` and `l*` both grow with `N`.
- `padded P`: `L` grows with `P`, but `l* = 1`.

That difference explains why `padded P` is hard for MCMC but trivial for
enumeration.

## 3. Measured perimeter

Settings: 300k MCMC iterations, 3 seeds, `enum-max-len=4`, 8s enum timeout.

| Strategy | `multi N` success/optimality | `padded P` success/optimality |
|---|---:|---:|
| fixed MCMC | 91.7% / 91.7% | 86.7% / 36.7% |
| enumeration | 37.5% / 37.5% | 100% / 100% |
| HC-MCMC | 54.2% / 54.2% | 90.0% / 80.0% |
| adaptive MCMC | 95.8% / 95.8% | 73.3% / 73.3% |

Interpretation:

- Enumeration is exact and optimal when the shortest solution fits under its
  cap; it loses only to timeouts when the required depth is too large.
- MCMC succeeds more often than it is optimal on long padded targets. It can
  find a long correct program without finding the 1-instruction optimum.
- Adaptive MCMC is strongest on deep `multi N` targets.
- HC-MCMC is strongest on long padded targets, where kicks help it escape the
  all-NOP/local-minimum region.

See [benchmark.md](benchmark.md) for the full per-size tables and
[math.md](math.md#6-how-the-benchmark-measures-better) for the formal metric
definitions.

## 4. What each parameter does to the perimeter

| Change | Likely effect |
|---|---|
| More `--iters` | Larger reachable perimeter; more success, longer runtime. Optimality may still lag. |
| More `--chains` | Lower seed variance; better chance of one successful chain. |
| Higher fixed `--beta` | Greedier search; faster descent, more local minima. |
| Lower fixed `--beta` | More exploration; slower convergence, more uphill moves. |
| Higher `--target-accept` | Adaptive search usually lowers `beta` and explores more. |
| Smaller `--adapt-window` | Faster temperature adaptation; can be unstable. |
| Larger `--hc-kick` | Farther jumps from the incumbent; more escape, less local refinement. |
| Larger `--enum-max-len` | Larger exact perimeter; exponential cost, hard cap 4. |
| Longer target (`L` grows, `l*` fixed) | MCMC search space grows; enumeration unchanged. |
| Deeper solution (`l*` grows) | Enumeration cost grows exponentially; MCMC may need more iterations. |

## 5. Reproduce

```bash
# Enumeration: success and optimality in one step
./superopt bench/examples/long_shallow_100.sopt --algo enum --enum-max-len 4

# Default MCMC budget: neither success nor optimality on this target
./superopt bench/examples/long_shallow_100.sopt --algo adaptive --iters 10000 --seed 1
./superopt bench/examples/long_shallow_100.sopt --algo hc       --iters 10000 --seed 1

# Larger budget: success, but not optimality
./superopt bench/examples/long_shallow_100.sopt --algo hc       --iters 300000 --seed 1
./superopt bench/examples/long_shallow_100.sopt --algo mcmc     --iters 300000 --beta 2 --seed 1
```

Read the first output line as:

```text
algo=<strategy> best_cost=<1000*eq+perf> best_eq=<correctness> \
best_perf=<cost> best_insns=<non-NOP count> time=<seconds>
```

Then apply the definitions:

```text
success   <=> best_eq == 0
optimal   <=> best_eq == 0 && best_insns == opt
```

For `long_shallow_100.sopt`, `opt = 1`.

## 6. Related documents

- [benchmark.md](benchmark.md) — strategy comparison and winner regimes.
- [math.md](math.md) — cost function, acceptance rule, adaptive/HC math.
- [running.md](running.md) — CLI reference and output interpretation.
- [process.md](process.md) — end-to-end pipeline from target to verified result.
