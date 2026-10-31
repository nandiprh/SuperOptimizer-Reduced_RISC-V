#!/usr/bin/env python3
"""Benchmark the three search strategies on synthetic targets with known optima.

The three strategies are run separately (never combined in one process):

  enum      src/enumerate.cpp, exhaustive search with a hard length cap
  adaptive  Metropolis-Hastings MCMC with online inverse-temperature adaptation
  hc        greedy hill climbing MCMC with stagnation-triggered random kicks

Two target families make the optimum provable:

  multi N   live_out = {a0..a(N-1)}; every output is doubled.  Every live-out
            must be written by a distinct instruction, so the optimum is exactly
            N instructions (and it is reachable with N).

  padded P  a length-P chain of algebraic identity gadgets on t0, followed by
            `add a0,t0,t0`.  The optimum is exactly 1 instruction regardless of
            P; only the target/buffer length grows.

The interesting boundaries are therefore:
  * enumerative explodes / hits its cap as the *required* length grows;
  * MCMC degrades as the *buffer* length grows, even when the optimum is short.

Usage:
    python3 tools/benchmark_search.py                 # full run, writes CSV
    python3 tools/benchmark_search.py --budget 100000 --seeds 2 --quick
"""

import argparse
import csv
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

RESULT_RE = re.compile(
    r"algo=(\S+)\s+best_cost=(-?\d+)\s+best_eq=(-?\d+)\s+"
    r"best_perf=(-?\d+)\s+best_insns=(-?\d+)\s+time=([0-9.eE+-]+)"
)


def write_multi(root, n):
    regs = [f"a{i}" for i in range(n)]
    lines = [
        f"live_in: {{{', '.join(regs)}}}",
        f"live_out: {{{', '.join(regs)}}}",
        "code:",
    ]
    lines += [f"slli {r}, {r}, 1" for r in regs]
    path = root / f"multi{n}.sopt"
    path.write_text("\n".join(lines) + "\n")
    return path


def write_padded(root, pad):
    gadgets = [
        ("add  t0, a0, a1", "sub  t0, t0, a1"),
        ("xor  t0, a0, a1", "xor  t0, t0, a1"),
        ("xori t0, a0, -1", "xori t0, t0, -1"),
        ("addi t0, a0, 0", "or   t0, t0, x0"),
    ]
    lines = ["live_in: {a0, a1}", "live_out: {a0}", "code:"]
    k = 0
    while k < pad:
        lines.extend(gadgets[(k // 2) % len(gadgets)])
        k += 2
    lines.append("add  a0, t0, t0")
    path = root / f"padded{pad}.sopt"
    path.write_text("\n".join(lines) + "\n")
    return path


def run_one(binary, target, algo, budget, seed, enum_max_len, timeout):
    cmd = [binary, str(target), "--algo", algo, "--seed", str(seed)]
    if algo == "enum":
        cmd += ["--enum-max-len", str(enum_max_len)]
    else:
        cmd += ["--iters", str(budget), "--beta", "2.0"]
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return {
            "algo": algo,
            "status": "timeout",
            "best_eq": None,
            "best_insns": None,
            "time": timeout,
        }
    wall = time.perf_counter() - t0
    if proc.returncode != 0:
        return {
            "algo": algo,
            "status": "error",
            "best_eq": None,
            "best_insns": None,
            "time": wall,
        }
    m = RESULT_RE.search(proc.stdout)
    if not m:
        return {
            "algo": algo,
            "status": "parse-error",
            "best_eq": None,
            "best_insns": None,
            "time": wall,
        }
    return {
        "algo": algo,
        "status": "ok",
        "best_eq": int(m.group(3)),
        "best_insns": int(m.group(5)),
        "time": float(m.group(6)),
    }


def summarize(rows, key):
    grouped = {}
    for row in rows:
        grouped.setdefault((row["family"], row["size"], row["algo"]), []).append(row)

    out = {}
    for k, runs in grouped.items():
        ok_runs = [r for r in runs if r["status"] == "ok"]
        success = [r for r in ok_runs if r["best_eq"] == 0]
        optimal = [
            r for r in success
            if r["best_insns"] is not None and r["best_insns"] == r["opt"]
        ]
        times = [r["time"] for r in runs]
        out[k] = {
            "n": len(runs),
            "success_rate": len(success) / len(runs),
            "optimality_rate": len(optimal) / len(runs),
            "median_time": statistics.median(times) if times else float("inf"),
            "min_time": min(times) if times else float("inf"),
            "statuses": {r["status"] for r in runs},
        }
    return out


def first(seq, pred):
    for x in seq:
        if pred(x):
            return x
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./superopt")
    ap.add_argument("--budget", type=int, default=300000,
                    help="MCMC iterations for adaptive/hc (default 300000)")
    ap.add_argument("--seeds", type=int, default=3,
                    help="independent MCMC seeds per target (default 3)")
    ap.add_argument("--enum-max-len", type=int, default=4)
    ap.add_argument("--enum-timeout", type=float, default=8.0,
                    help="seconds before an enumerative run counts as intractable")
    ap.add_argument("--mcmc-timeout", type=float, default=120.0)
    ap.add_argument("--out", default="results/search_benchmark.csv")
    ap.add_argument("--quick", action="store_true",
                    help="small suite: multi 1..5, padded 0..20")
    args = ap.parse_args()

    binary = str(Path(args.binary).resolve())
    if not Path(binary).exists():
        raise SystemExit(f"binary not found: {binary} (run make first)")

    multi_sizes = list(range(1, 6)) if args.quick else list(range(1, 9))
    padded_sizes = [0, 4, 8, 16, 20] if args.quick else [0, 2, 4, 8, 12, 16, 20, 24, 28, 30]

    rows = []
    with tempfile.TemporaryDirectory(prefix="sopt-bench-") as tmp:
        root = Path(tmp)
        targets = []
        for n in multi_sizes:
            targets.append(("multi", n, n, write_multi(root, n)))
        for p in padded_sizes:
            targets.append(("padded", p, 1, write_padded(root, p)))

        for family, size, opt, path in targets:
            print(f"[{family} {size}] {path.name}", flush=True)
            # Enumerative search is deterministic; one run is enough.
            er = run_one(binary, path, "enum", 0, 1,
                         args.enum_max_len, args.enum_timeout)
            er.update(family=family, size=size, opt=opt, seed=None)
            rows.append(er)
            for algo in ("adaptive", "hc"):
                for seed in range(1, args.seeds + 1):
                    rr = run_one(binary, path, algo, args.budget, seed,
                                 args.enum_max_len, args.mcmc_timeout)
                    rr.update(family=family, size=size, opt=opt, seed=seed)
                    rows.append(rr)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", newline="") as f:
        w = csv.DictWriter(
            f,
            fieldnames=["family", "size", "opt", "algo", "seed", "status",
                        "best_eq", "best_insns", "time"],
        )
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k) for k in w.fieldnames})

    stats = summarize(rows, "algo")
    print("\n=== results ===")
    print(f"budget={args.budget} iters, seeds={args.seeds}, "
          f"enum_max_len={args.enum_max_len}, enum_timeout={args.enum_timeout}s")
    print(f"{'family':7} {'size':>4} {'algo':9} {'success':>7} {'optimal':>8} "
          f"{'med_s':>8} {'notes'}")
    for key in sorted(stats, key=lambda k: (k[0], k[1], k[2])):
        family, size, algo = key
        s = stats[key]
        notes = ",".join(sorted(s["statuses"] - {"ok"}))
        print(f"{family:7} {size:>4} {algo:9} {s['success_rate']:>7.2f} "
              f"{s['optimality_rate']:>8.2f} {s['median_time']:>8.3f} {notes}")

    # --- locate breaking points -------------------------------------------------
    def rate(family, size, algo, field):
        s = stats.get((family, size, algo))
        return s[field] if s else 0.0

    multi_sizes_sorted = sorted({k[1] for k in stats if k[0] == "multi"})
    padded_sizes_sorted = sorted({k[1] for k in stats if k[0] == "padded"})

    enum_break = first(
        multi_sizes_sorted,
        lambda z: rate("multi", z, "enum", "optimality_rate") < 1.0,
    )
    adaptive_overtake = first(
        multi_sizes_sorted,
        lambda z: max(rate("multi", z, "adaptive", "optimality_rate"),
                      rate("multi", z, "hc", "optimality_rate"))
        > rate("multi", z, "enum", "optimality_rate"),
    )
    hc_break = first(
        [z for z in multi_sizes_sorted if z >= (adaptive_overtake or 1)],
        lambda z: rate("multi", z, "adaptive", "optimality_rate")
        - rate("multi", z, "hc", "optimality_rate") >= 0.2,
    )
    mcmc_pad_break = first(
        padded_sizes_sorted,
        lambda z: max(rate("padded", z, "adaptive", "success_rate"),
                      rate("padded", z, "hc", "success_rate")) < 1.0,
    )

    print("\n=== breaking points ===")
    if enum_break is not None:
        print(f"- enumerative stops being exact at required length {enum_break} "
              f"(hard cap {args.enum_max_len} / timeout {args.enum_timeout:g}s).")
    if adaptive_overtake is not None:
        print(f"- MCMC first beats enumerative on optimality at required length "
              f"{adaptive_overtake}.")
    if hc_break is not None:
        print(f"- adaptive MCMC first beats HC-MCMC by >=0.20 optimality at "
              f"required length {hc_break}.")
    if mcmc_pad_break is not None:
        print(f"- no MCMC variant succeeds on every seed once padding reaches "
              f"{mcmc_pad_break} (target length {mcmc_pad_break + 1}) with "
              f"budget {args.budget}.")
    if all(x is None for x in (enum_break, adaptive_overtake, hc_break, mcmc_pad_break)):
        print("- no breaking point in this suite/budget.")

    print(f"\nraw results: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
