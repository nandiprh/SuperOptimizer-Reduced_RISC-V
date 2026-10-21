#!/usr/bin/env python3
"""Train a gradient-boosted regression-tree cost model and export it to C++.

Pure standard library: histogram splits keep this fast without NumPy or
scikit-learn. The model predicts simulator cycle counts from the feature
vector produced by src/features.cpp (29 values, fixed order).

Usage:
    python3 tools/train_model.py --dataset results/dataset.csv \
        --out src/model_data.hpp [--trees 60] [--depth 3] [--lr 0.1]
"""

import argparse
import bisect
import csv
import math
import random
import sys


def load_dataset(path):
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = [row for row in reader if row]

    if "cycles_dynamic" not in header or "cycles_bundled" not in header:
        raise SystemExit("dataset is missing cycles_dynamic/cycles_bundled columns")

    label_cols = {"cycles_dynamic", "cycles_bundled"}
    feature_names = [h for h in header if h not in label_cols]
    dyn_idx = header.index("cycles_dynamic")
    bun_idx = header.index("cycles_bundled")

    X, yd, yb = [], [], []
    for row in rows:
        try:
            feats = [float(row[header.index(name)]) for name in feature_names]
            X.append(feats)
            yd.append(float(row[dyn_idx]))
            yb.append(float(row[bun_idx]))
        except (ValueError, IndexError):
            continue
    return feature_names, X, yd, yb


def make_thresholds(X, num_bins):
    n_features = len(X[0])
    thresholds = []
    for f in range(n_features):
        vals = sorted({row[f] for row in X})
        if len(vals) <= 1:
            thresholds.append([])
            continue
        if len(vals) <= num_bins:
            cuts = vals[:-1]
        else:
            cuts = [vals[(k + 1) * len(vals) // (num_bins + 1)]
                    for k in range(num_bins)]
        cuts = sorted(set(cuts))
        thresholds.append(cuts)
    return thresholds


class TreeBuilder:
    def __init__(self, X, Xbin, thresholds, residual, max_depth, min_leaf):
        self.X = X
        self.Xbin = Xbin
        self.thresholds = thresholds
        self.residual = residual
        self.max_depth = max_depth
        self.min_leaf = min_leaf
        self.nodes = []

    def new_node(self):
        self.nodes.append(None)
        return len(self.nodes) - 1

    def build(self, indices, depth):
        idx = self.new_node()
        n = len(indices)
        total = sum(self.residual[i] for i in indices)
        mean = total / n if n else 0.0

        if depth >= self.max_depth or n < 2 * self.min_leaf:
            self.nodes[idx] = {"fi": -1, "thr": 0.0, "left": -1,
                               "right": -1, "value": mean}
            return idx

        best = None  # (score, feature, bin_cut)
        baseline = total * total / n
        n_features = len(self.thresholds)

        for f in range(n_features):
            cuts = self.thresholds[f]
            if not cuts:
                continue
            nbins = len(cuts) + 1
            counts = [0] * nbins
            sums = [0.0] * nbins
            bins = self.Xbin
            for i in indices:
                b = bins[i][f]
                counts[b] += 1
                sums[b] += self.residual[i]

            left_n = 0
            left_s = 0.0
            for k in range(nbins - 1):
                left_n += counts[k]
                left_s += sums[k]
                right_n = n - left_n
                if left_n < self.min_leaf or right_n < self.min_leaf:
                    continue
                right_s = total - left_s
                score = left_s * left_s / left_n + right_s * right_s / right_n
                if best is None or score > best[0]:
                    best = (score, f, k)

        if best is None or best[0] - baseline <= 1e-12:
            self.nodes[idx] = {"fi": -1, "thr": 0.0, "left": -1,
                               "right": -1, "value": mean}
            return idx

        _, f, k = best
        thr = self.thresholds[f][k]
        left = [i for i in indices if self.Xbin[i][f] <= k]
        right = [i for i in indices if self.Xbin[i][f] > k]

        left_idx = self.build(left, depth + 1)
        right_idx = self.build(right, depth + 1)
        self.nodes[idx] = {"fi": f, "thr": thr, "left": left_idx,
                           "right": right_idx, "value": 0.0}
        return idx


def tree_predict(nodes, root, x):
    node = root
    while nodes[node]["fi"] >= 0:
        n = nodes[node]
        node = n["left"] if x[n["fi"]] <= n["thr"] else n["right"]
    return nodes[node]["value"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", default="results/dataset.csv")
    ap.add_argument("--out", default="src/model_data.hpp")
    ap.add_argument("--target", choices=["dynamic", "bundled"], default="dynamic")
    ap.add_argument("--trees", type=int, default=60)
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--lr", type=float, default=0.1)
    ap.add_argument("--bins", type=int, default=32)
    ap.add_argument("--min-leaf", type=int, default=5)
    ap.add_argument("--holdout", type=float, default=0.15)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    names, X, yd, yb = load_dataset(args.dataset)
    if not X:
        raise SystemExit("dataset is empty")
    y = yd if args.target == "dynamic" else yb

    order = list(range(len(X)))
    random.Random(args.seed).shuffle(order)
    n_test = int(len(order) * args.holdout)
    test_idx = set(order[:n_test])
    train_idx = [i for i in range(len(X)) if i not in test_idx]

    thresholds = make_thresholds(X, args.bins)
    # bisect_left matches the exported C++ test `x <= threshold`: bin <= k
    # iff x <= thresholds[f][k].
    Xbin = [[bisect.bisect_left(thresholds[f], row[f])
             for f in range(len(row))] for row in X]

    base = sum(y[i] for i in train_idx) / len(train_idx)
    pred = [base] * len(y)

    export_trees = []
    for t in range(args.trees):
        residual = [0.0] * len(y)
        for i in train_idx:
            residual[i] = y[i] - pred[i]

        builder = TreeBuilder(X, Xbin, thresholds, residual,
                              args.depth, args.min_leaf)
        root = builder.build(train_idx, 0)
        nodes = builder.nodes
        export_trees.append((nodes, root))

        # Apply this tree to every row so residuals stay correct for the next
        # boosting round (test rows are also updated for evaluation).
        for i in range(len(y)):
            pred[i] += args.lr * tree_predict(nodes, root, X[i])

    def metrics(indices):
        if not indices:
            return 0.0, 0.0
        errs = [abs(pred[i] - y[i]) for i in indices]
        mae = sum(errs) / len(errs)
        rmse = math.sqrt(sum(e * e for e in errs) / len(errs))
        return mae, rmse

    train_mae, train_rmse = metrics(train_idx)
    test_mae, test_rmse = metrics(list(test_idx))
    print(f"features={len(names)} train={len(train_idx)} test={len(test_idx)}")
    print(f"train MAE={train_mae:.4f} RMSE={train_rmse:.4f}")
    print(f"test  MAE={test_mae:.4f} RMSE={test_rmse:.4f}")

    # Flatten trees into the contiguous node array expected by model_gen.hpp.
    flat_nodes = []
    roots = []
    for nodes, root in export_trees:
        offset = len(flat_nodes)
        roots.append(offset + root)
        for node in nodes:
            if node["fi"] < 0:
                flat_nodes.append((-1, 0.0, -1, -1, node["value"] * args.lr))
            else:
                flat_nodes.append((
                    node["fi"], node["thr"],
                    offset + node["left"], offset + node["right"],
                    node["value"] * args.lr,
                ))

    # Self-check: evaluate the flattened structure exactly as C++ will, and
    # compare against the boosting loop's predictions.
    def flat_predict(x):
        total = base
        for root in roots:
            node = root
            while flat_nodes[node][0] >= 0:
                fi, thr, left, right, _ = flat_nodes[node]
                node = left if x[fi] <= thr else right
            total += flat_nodes[node][4]
        return total

    max_diff = max((abs(flat_predict(X[i]) - pred[i]) for i in range(len(X))),
                   default=0.0)
    print(f"export self-check: max |C++-style - Python| = {max_diff:.3e}")
    if max_diff > 1e-6:
        raise SystemExit("exported model does not match training predictions")

    with open(args.out, "w") as f:
        f.write("#pragma once\n\n")
        f.write("// Auto-generated by tools/train_model.py. Do not edit by hand.\n")
        f.write(f"// target=cycles_{args.target} trees={len(roots)} depth={args.depth} "
                f"lr={args.lr} bins={args.bins}\n\n")
        f.write("inline const GBDTModel g_model = GBDTModel{\n  {\n")
        for fi, thr, left, right, value in flat_nodes:
            f.write(f"    TreeNode{{{fi}, {thr!r}, {left}, {right}, {value!r}}},\n")
        f.write("  },\n")
        f.write(f"  {len(roots)},\n")
        f.write("  {" + ", ".join(str(r) for r in roots) + "},\n")
        f.write(f"  {base!r},\n")
        f.write("};\n")

    print(f"wrote {args.out} ({len(flat_nodes)} nodes, {len(roots)} trees)")


if __name__ == "__main__":
    main()
