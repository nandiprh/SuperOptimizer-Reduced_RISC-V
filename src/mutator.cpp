#include "mutator.hpp"
#include "testgen.hpp"
#include <random>
#include <algorithm>
#include <array>

namespace sopt {

static std::mt19937* mrng = nullptr;

void mutator_init_random(unsigned int s) {
  if (mrng) delete mrng;
  mrng = new std::mt19937(s == 0 ? 1 : s);
}

static uint32_t mrand() {
  if (!mrng) mutator_init_random(1);
  std::uniform_int_distribution<uint32_t> d(0, UINT32_MAX);
  return d(*mrng);
}

static int choose_from_pool(const ProgramSpec& spec) {
  if (spec.live_in.empty() && spec.live_out.empty() && spec.temps.empty()) return mrand() % 32;
  std::vector<int> pool;
  for (int r : spec.live_in) pool.push_back(r % 32);
  for (int r : spec.live_out) pool.push_back(r % 32);
  for (int r : spec.temps) pool.push_back(r % 32);
  if (pool.empty()) return mrand() % 32;
  return pool[mrand() % pool.size()];
}

static int choose_imm() {
  // use small set
  static const int imms[] = {0,1,-1,2,4,8,16,31,32,255,2047,-2048};
  return imms[mrand() % (sizeof(imms)/sizeof(imms[0]))];
}

Insn random_insn_same_fmt(const ProgramSpec& spec, Fmt fmt) {
  // pick random op with given fmt
  Insn insn; insn.rd=insn.rs1=insn.rs2=0; insn.imm=0; insn.op = Op::NOP;
  std::vector<Op> candidates;
  for (size_t i = 0; i < 21; i++) {
    Op op = static_cast<Op>(i);
    if (op == Op::NOP) continue;
    if (kOps[i].fmt == fmt) candidates.push_back(op);
  }
  if (candidates.empty()) {
    // default
    insn.op = Op::ADDI;; // not stored, but set op
    insn.rd = choose_from_pool(spec);
    insn.rs1 = choose_from_pool(spec);
    insn.imm = choose_imm();
    return insn;
  }
  insn.op = candidates[mrand() % candidates.size()];
  size_t idx = static_cast<size_t>(insn.op);
  Fmt f = kOps[idx].fmt;
  switch (f) {
    case Fmt::R:
      insn.rd = choose_from_pool(spec); insn.rs1 = choose_from_pool(spec); insn.rs2 = choose_from_pool(spec); break;
    case Fmt::I:
      insn.rd = choose_from_pool(spec); insn.rs1 = choose_from_pool(spec); insn.imm = choose_imm(); break;
    case Fmt::IShift:
      insn.rd = choose_from_pool(spec); insn.rs1 = choose_from_pool(spec); insn.imm = (int32_t)(mrand() % 32); break;
    case Fmt::U:
      insn.rd = choose_from_pool(spec); insn.imm = choose_imm() & 0xFFFFF; break;
    case Fmt::None:
      break;
  }
  return insn;
}

Insn random_insn(const ProgramSpec& spec) {
  // pick any non-nop
  int tries = 0;
  while (tries++ < 1000) {
    size_t idx = 1 + (mrand() % 20);
    Op op = static_cast<Op>(idx);
    Insn insn;
    insn.op = op; insn.rd=insn.rs1=insn.rs2=0; insn.imm=0;
    Fmt f = kOps[idx].fmt;
    switch (f) {
      case Fmt::R:
        insn.rd = choose_from_pool(spec); insn.rs1 = choose_from_pool(spec); insn.rs2 = choose_from_pool(spec); return insn;
      case Fmt::I:
        insn.rd = choose_from_pool(spec); insn.rs1 = choose_from_pool(spec); insn.imm = choose_imm(); return insn;
      case Fmt::IShift:
        insn.rd = choose_from_pool(spec); insn.rs1 = choose_from_pool(spec); insn.imm = (int32_t)(mrand() % 32); return insn;
      case Fmt::U:
        insn.rd = choose_from_pool(spec); insn.imm = choose_imm() & 0xFFFFF; return insn;
      case Fmt::None:
        return insn;
    }
  }
  return make_nop();
}

void mutate_program(const ProgramSpec& spec, Insn* buf, int len, double* move_probs) {
  if (len <= 0) return;
  std::uniform_real_distribution<double> rd(0,1);
  double r = rd(*mrng);
  // default probs: opcode 0.3, operand 0.3, swap 0.2, instr 0.2
  double p_opcode = 0.3, p_op = 0.3, p_swap = 0.2;
  if (move_probs) { p_opcode=move_probs[0]; p_op=move_probs[1]; p_swap=move_probs[2]; }
  double s1 = p_opcode, s2 = s1+p_op, s3 = s2+p_swap;
  int i = mrand() % len;
  if (r < s1) {
    // opcode change - same fmt
    size_t idx = static_cast<size_t>(buf[i].op);
    Fmt f = (idx < 21) ? kOps[idx].fmt : Fmt::None;
    if (f != Fmt::None) {
      buf[i] = random_insn_same_fmt(spec, f);
    }
  } else if (r < s2) {
    // operand change
    size_t idx = static_cast<size_t>(buf[i].op);
    Fmt f = (idx < 21) ? kOps[idx].fmt : Fmt::None;
    switch (f) {
      case Fmt::R:
        switch (mrand() % 3) { case 0: buf[i].rd = choose_from_pool(spec); break; case 1: buf[i].rs1 = choose_from_pool(spec); break; case 2: buf[i].rs2 = choose_from_pool(spec); break; }
        break;
      case Fmt::I:
        switch (mrand() % 3) { case 0: buf[i].rd = choose_from_pool(spec); break; case 1: buf[i].rs1 = choose_from_pool(spec); break; case 2: buf[i].imm = choose_imm(); break; }
        break;
      case Fmt::IShift:
        switch (mrand() % 3) { case 0: buf[i].rd = choose_from_pool(spec); break; case 1: buf[i].rs1 = choose_from_pool(spec); break; case 2: buf[i].imm = (int32_t)(mrand() % 32); break; }
        break;
      case Fmt::U:
        switch (mrand() % 2) { case 0: buf[i].rd = choose_from_pool(spec); break; case 1: buf[i].imm = choose_imm() & 0xFFFFF; break; }
        break;
      case Fmt::None:
        break;
    }
  } else if (r < s3) {
    // swap
    int j = mrand() % len;
    if (i != j) std::swap(buf[i], buf[j]);
  } else {
    // replace with random insn (maybe nop)
    if (mrand() % 10 == 0) buf[i] = make_nop();
    else buf[i] = random_insn(spec);
  }
}

} // namespace sopt
