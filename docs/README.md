# Superoptimizer Documentation

A C++17 implementation of a regression-guided, STOKE-style superoptimizer for a
register-only RV32I subset. Given a loop-free register-only basic block (the
*target*), the tool searches for a cheaper sequence of RV32I instructions
(ADD/SUB/AND/OR/XOR/shifts/compares/immediates/LUI) with **identical live-out
values** for all inputs. Search is **Metropolis–Hastings MCMC** and cost is
`correctness-distance + instruction count`.

This directory is the hand-written companion to the formal design document at
[`../architecture.md`](../architecture.md). Where this implementation differs
from the design, it says so explicitly in
[process.md](process.md#status-vs-architecture).

## Quick Start

```bash
cd ..          # repo root
make clean && make
./superopt bench/toy/smoke.sopt --iters 300000 --beta 2.0 --seed 123
```

For the full build/run guide, CLI reference and file-format spec see
[running.md](running.md). That page also covers
[what is being optimized](running.md#what-is-being-optimized),
[how the flow works](running.md#how-it-handles-the-flow), and
[caveats](running.md#caveats).

## Documentation Map

| File | Contents |
|---|---|
| [process.md](process.md) | End-to-end pipeline: how a `.sopt` target becomes a verified optimized program; status vs. `architecture.md`. |
| [format.md](format.md) | The `.sopt` file format, the register/immediate conventions, and a worked example. |
| [code.md](code.md) | Module-by-module walkthrough of every source file in `src/`. |
| [algorithms.md](algorithms.md) | The algorithms: test generation, interpreter semantics, mutator moves, MCMC search loop, probabilistic verification. |
| [math.md](math.md) | The mathematics: graded correctness distance, cost function, Metropolis–Hastings acceptance, stationarity, why the moves need no proposal-ratio correction. |
| [MCMC.md](MCMC.md) | Deeper MCMC explainer: how the random walk works, why it is *not* a tree/graph search, a worked trace, and planned extensions. |
| [frontend.md](frontend.md) | The C-like expression frontend (`--expr` / interactive REPL): language, codegen notes, and the REPL command reference. |
| [running.md](running.md) | How to build and run, CLI options, output interpretation, examples, and how to write your own targets. |

## Repository Layout

```
architecture.md        design / target specification (formal)
Makefile               build (g++ -std=c++17 -Wall -Wextra -O2)
src/                   all C++ sources (see code.md)
bench/toy/*.sopt       toy benchmark targets
docs/                  this documentation
```

## Implemented vs. Roadmap

Fully implemented today:

- 20-opcode RV32I subset + NOP buffer filler (`is_nop` normalizes `addi x0,x0,0`)
- Bit-exact interpreter (wraparound `uint32_t`, low-5-bit shifts, arithmetic SRA,
  sign-extended immediates, `SLTIU` unsigned compare)
- Boundary + random test generation; non-live-in registers initialized to random
  constants per test case
- Graded correctness distance (Hamming distance on live-outs), cost = `1000*eq + insn_count`
- Fixed-length NOP-filled buffer, 4 symmetric mutator moves
- Single-chain Metropolis–Hastings search
- C-like expression frontend (`--expr` mode + interactive REPL) that compiles
  blocks to `ProgramSpec` and feeds the same search pipeline
- Probabilistic verification on fresh random **and** small-signed-integer tests
  before reporting

Implemented beyond the core search:

- Z3 `QF_BV` exact verification (`--z3`) with a counterexample-guided refine loop
- Multi-chain search (`--chains K`, sequential chains today)
- Enumerative baseline (`--enum`)
- Dependency DAG + list scheduling + Tomasulo cost oracle (`--cost-mode 2/3`)
- Gradient-boosted regression-tree cost model (`--cost-mode 1`; see `make model`)

Still deferred:

- Threaded/parallel chains and early-exit search
- `gcc`/`clang` basic-block front end