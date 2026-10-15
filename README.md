# Superoptimizer (STOKE-style, RV32I subset)

A regression-guided STOKE-style superoptimizer for a register-only RV32I
subset. Given a loop-free basic block, it searches (Metropolis–Hastings MCMC)
for a cheaper instruction sequence with identical live-out register values.

## Documentation

Start with [`docs/README.md`](docs/README.md).

- [docs/process.md](docs/process.md) — end-to-end pipeline
- [docs/format.md](docs/format.md) — `.sopt` file format
- [docs/code.md](docs/code.md) — module-by-module code walkthrough
- [docs/algorithms.md](docs/algorithms.md) — algorithms used
- [docs/math.md](docs/math.md) — the mathematics behind cost and MCMC
- [docs/running.md](docs/running.md) — how to build and run
- [architecture.md](architecture.md) — formal design / target specification

## Quick start

```bash
make
./superopt bench/toy/smoke.sopt --iters 300000 --beta 2.0 --seed 123
```

Output (the smoke target `t3 = ((a0+a1)-a1)<<1` reduces to one instruction):

```text
best_cost=1 best_eq=0 best_perf=1 time=0.1196
best program:
nop
nop
add x28, x10, x10
verify: OK (probabilistic) fresh_rand_eq=0
```