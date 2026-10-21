#include "enumerate.hpp"
#include "cost.hpp"
#include "interpreter.hpp"
#include "testgen.hpp"
#include <random>
#include <algorithm>
#include <chrono>
#include <set>
#include <functional>
#include <bitset>

namespace sopt {

namespace {

std::vector<int> build_pool(const ProgramSpec& spec) {
  std::vector<int> pool;
  auto add = [&](int r) {
    if (r < 0 || r >= 32 || r == 0) return;
    for (int p : pool) if (p == r) return;
    pool.push_back(r);
  };
  for (int r : spec.live_in) add(r);
  for (int r : spec.live_out) add(r);
  for (int r : spec.temps) add(r);
  if (pool.empty()) pool = {10, 11, 28};
  return pool;
}

std::vector<int32_t> build_imm_pool(const ProgramSpec& spec) {
  std::set<int32_t> s;
  static const int base[] = {0, 1, -1, 2, 4, 8, 16, 31, 32, 255, 2047, -2048};
  for (int v : base) s.insert(v);
  for (const auto& insn : spec.code) {
    if (insn.op == Op::LUI) s.insert(insn.imm & 0xFFFFF);
    else if (insn.op != Op::NOP) s.insert(insn.imm);
  }
  return std::vector<int32_t>(s.begin(), s.end());
}

bool is_commutative(Op op) {
  return op == Op::ADD || op == Op::AND || op == Op::OR || op == Op::XOR;
}

bool is_dead_write(const Insn& insn) {
  // Identity operations: they write the same value they read, so the slot can
  // be represented by a NOP and is pruned from the search.
  switch (insn.op) {
    case Op::ADDI: return insn.imm == 0 && insn.rd == insn.rs1;
    case Op::XORI: return insn.imm == 0 && insn.rd == insn.rs1;
    case Op::ORI:  return insn.imm == 0 && insn.rd == insn.rs1;
    case Op::ANDI: return insn.imm == -1 && insn.rd == insn.rs1;
    case Op::SLLI:
    case Op::SRLI:
    case Op::SRAI: return insn.imm == 0 && insn.rd == insn.rs1;
    default: return false;
  }
}

} // namespace

EnumerateResult enumerate_search(const ProgramSpec& spec,
                                 const std::vector<TestCase>& tests,
                                 const EnumerateConfig& cfg) {
  EnumerateResult res;
  res.best_cost = std::numeric_limits<long long>::max();
  res.best_eq = std::numeric_limits<long long>::max();
  res.best_perf = std::numeric_limits<long long>::max();
  res.programs_tested = 0;
  res.time_sec = 0.0;

  auto start = std::chrono::high_resolution_clock::now();

  int Lmax = cfg.max_len > 0 ? cfg.max_len : 3;
  if (Lmax < 1) Lmax = 1;
  if (Lmax > 4) Lmax = 4; // hard cap: exhaustive beyond this is intractable

  std::vector<int> pool = build_pool(spec);
  std::vector<int32_t> imms = build_imm_pool(spec);
  int pool_n = (int)pool.size();

  // live_out as a bitset for fast checks
  std::bitset<32> live_out_bits;
  for (int r : spec.live_out) if (r >= 0 && r < 32) live_out_bits.set(r);

  std::vector<Insn> buf(4);
  for (int i = 0; i < 4; i++) buf[i] = make_nop();

  bool found = false;

  // Backward-use constraint: slot i's rd must be in live_out, or be read by a
  // later slot j>i (as rs1/rs2). This prunes dead writes without changing the
  // set of observable programs.
  std::function<bool(int, int)> gen = [&](int pos, int len) -> bool {
    if (pos == len) {
      // Validate backward-use constraint: every written reg must be live_out or read later.
      for (int i = 0; i < len; i++) {
        if (buf[i].op == Op::NOP) continue;
        int rd = buf[i].rd;
        if (live_out_bits.test(rd)) continue;
        bool read_later = false;
        for (int j = i + 1; j < len; j++) {
          if (buf[j].op == Op::NOP) continue;
          if (buf[j].rs1 == rd || buf[j].rs2 == rd) { read_later = true; break; }
        }
        if (!read_later) return false; // dead write -> prune
      }

      res.programs_tested++;
      long long eqv = cost_eq_only(buf.data(), len, tests, spec.live_out);
      if (eqv == 0) {
        // The training tests can be overfit, so optionally confirm survivors on
        // a fresh test set before accepting them as the enumerated optimum.
        if (cfg.verify_survivors) {
          auto fresh = gen_test_set(spec.code, spec.live_in, spec.live_out, 512, false);
          if (cost_eq_only(buf.data(), len, fresh, spec.live_out) != 0) return false;
        }
        long long perv = perf_insn_count(buf.data(), len);
        long long cost = eqv * 1000LL + perv;
        if (cost < res.best_cost) {
          res.best_prog.assign(buf.begin(), buf.begin() + len);
          res.best_cost = cost;
          res.best_eq = eqv;
          res.best_perf = perv;
        }
        return true;
      }
      return false;
    }

    for (size_t oi = 1; oi < 21 && !found; oi++) {
      Op op = static_cast<Op>(oi);
      size_t idx = static_cast<size_t>(op);
      Fmt f = kOps[idx].fmt;

      buf[pos].op = op;
      buf[pos].rd = buf[pos].rs1 = buf[pos].rs2 = 0;
      buf[pos].imm = 0;

      // Enumerate register operands
      for (int ra = 0; ra < pool_n && !found; ra++) {
        uint8_t rd = (uint8_t)pool[ra];
        buf[pos].rd = rd;

        if (f == Fmt::R) {
          for (int rb = 0; rb < pool_n && !found; rb++) {
            uint8_t rs1 = (uint8_t)pool[rb];
            buf[pos].rs1 = rs1;
            for (int rc = 0; rc < pool_n && !found; rc++) {
              uint8_t rs2 = (uint8_t)pool[rc];
              if (is_commutative(op) && rs1 > rs2) continue; // canonicalize
              buf[pos].rs2 = rs2;
              if (is_dead_write(buf[pos])) continue;
              if (gen(pos + 1, len)) return true;
            }
          }
        } else if (f == Fmt::I || f == Fmt::IShift) {
          for (int rb = 0; rb < pool_n && !found; rb++) {
            buf[pos].rs1 = (uint8_t)pool[rb];
            for (int32_t im : imms) {
              buf[pos].imm = im;
              if (im < kOps[idx].imm_min || im > kOps[idx].imm_max) continue;
              if (is_dead_write(buf[pos])) continue;
              if (gen(pos + 1, len)) return true;
            }
          }
        } else if (f == Fmt::U) {
          for (int32_t im : imms) {
            buf[pos].imm = im & 0xFFFFF;
            if (gen(pos + 1, len)) return true;
          }
        }
      }
      buf[pos] = make_nop();
    }
    return false;
  };

  // Length 0: the empty (all-nop) program. Handles identity targets optimally.
  {
    res.programs_tested++;
    long long eqv = cost_eq_only(buf.data(), 0, tests, spec.live_out);
    if (eqv == 0) {
      res.best_prog.assign(buf.begin(), buf.begin()); // empty
      res.best_cost = 0;
      res.best_eq = 0;
      res.best_perf = 0;
      found = true;
    }
  }

  for (int len = 1; len <= Lmax && !found; len++) {
    if (gen(0, len)) {
      found = true;
      break;
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  res.time_sec = std::chrono::duration<double>(end - start).count();
  return res;
}

} // namespace sopt
