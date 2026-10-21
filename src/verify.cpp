#include "verify.hpp"
#include "testgen.hpp"
#include "interpreter.hpp"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <unistd.h>

namespace sopt {

namespace {

// 32-bit unsigned representation of a signed immediate (sign-extended to 32 bits).
uint32_t imm32(int32_t imm) {
  return static_cast<uint32_t>(imm);
}

// SMT-LIB2 bitvector literal for a 32-bit value.
std::string bv32(uint32_t v) {
  char buf[64];
  snprintf(buf, sizeof(buf), "#x%08x", v);
  return std::string(buf);
}

std::string input_name(int r) {
  return "in_x" + std::to_string(r);
}

bool is_z3_available_cached = false;
bool z3_available_result = false;

std::string trim_copy(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
  return s.substr(b, e - b);
}

} // namespace

bool z3_available() {
  if (is_z3_available_cached) return z3_available_result;
  std::string cmd = "command -v z3 >/dev/null 2>&1";
  int rc = std::system(cmd.c_str());
  z3_available_result = (rc == 0);
  is_z3_available_cached = true;
  return z3_available_result;
}

// Encode one program in SSA form. Each instruction becomes a top-level
// definition `(define-fun <prefix>_vN () (_ BitVec 32) <expr>)`; later
// definitions refer to earlier ones by name, so there is no parallel-let
// scoping problem. Both programs share the same input symbols.
void encode_program(const std::vector<Insn>& prog,
                    const std::vector<int>& live_in,
                    const std::vector<int>& live_out,
                    const std::string& prefix,
                    std::set<int>& inputs,
                    std::vector<std::string>& defs,
                    std::unordered_map<int, std::string>& out_expr) {
  std::vector<std::string> cur(32, "");
  cur[0] = "(_ bv0 32)";

  auto src = [&](int r) -> std::string {
    if (r == 0) return "(_ bv0 32)";
    if (!cur[r].empty()) return cur[r];
    inputs.insert(r);
    cur[r] = input_name(r);
    return cur[r];
  };

  // Live-in registers are inputs even if the program never reads them.
  for (int r : live_in) {
    if (r <= 0 || r >= 32) continue;
    inputs.insert(r);
    if (cur[r].empty()) cur[r] = input_name(r);
  }

  int step = 0;
  for (const auto& insn : prog) {
    if (insn.op == Op::NOP) continue;

    std::string expr;
    switch (insn.op) {
      case Op::ADD:  expr = "(bvadd " + src(insn.rs1) + " " + src(insn.rs2) + ")"; break;
      case Op::SUB:  expr = "(bvsub " + src(insn.rs1) + " " + src(insn.rs2) + ")"; break;
      case Op::AND:  expr = "(bvand " + src(insn.rs1) + " " + src(insn.rs2) + ")"; break;
      case Op::OR:   expr = "(bvor " + src(insn.rs1) + " " + src(insn.rs2) + ")"; break;
      case Op::XOR:  expr = "(bvxor " + src(insn.rs1) + " " + src(insn.rs2) + ")"; break;
      case Op::SLL: {
        std::string amt = "(bvand " + src(insn.rs2) + " (_ bv31 32))";
        expr = "(bvshl " + src(insn.rs1) + " " + amt + ")"; break;
      }
      case Op::SRL: {
        std::string amt = "(bvand " + src(insn.rs2) + " (_ bv31 32))";
        expr = "(bvlshr " + src(insn.rs1) + " " + amt + ")"; break;
      }
      case Op::SRA: {
        std::string amt = "(bvand " + src(insn.rs2) + " (_ bv31 32))";
        expr = "(bvashr " + src(insn.rs1) + " " + amt + ")"; break;
      }
      case Op::SLT:
        expr = "(ite (bvslt " + src(insn.rs1) + " " + src(insn.rs2) + ") (_ bv1 32) (_ bv0 32))";
        break;
      case Op::SLTU:
        expr = "(ite (bvult " + src(insn.rs1) + " " + src(insn.rs2) + ") (_ bv1 32) (_ bv0 32))";
        break;
      case Op::ADDI:  expr = "(bvadd " + src(insn.rs1) + " " + bv32(imm32(insn.imm)) + ")"; break;
      case Op::ANDI:  expr = "(bvand " + src(insn.rs1) + " " + bv32(imm32(insn.imm)) + ")"; break;
      case Op::ORI:   expr = "(bvor " + src(insn.rs1) + " " + bv32(imm32(insn.imm)) + ")"; break;
      case Op::XORI:  expr = "(bvxor " + src(insn.rs1) + " " + bv32(imm32(insn.imm)) + ")"; break;
      case Op::SLLI: {
        uint32_t shamt = static_cast<uint32_t>(insn.imm) & 31u;
        expr = "(bvshl " + src(insn.rs1) + " " + bv32(shamt) + ")"; break;
      }
      case Op::SRLI: {
        uint32_t shamt = static_cast<uint32_t>(insn.imm) & 31u;
        expr = "(bvlshr " + src(insn.rs1) + " " + bv32(shamt) + ")"; break;
      }
      case Op::SRAI: {
        uint32_t shamt = static_cast<uint32_t>(insn.imm) & 31u;
        expr = "(bvashr " + src(insn.rs1) + " " + bv32(shamt) + ")"; break;
      }
      case Op::SLTI:
        expr = "(ite (bvslt " + src(insn.rs1) + " " + bv32(imm32(insn.imm)) + ") (_ bv1 32) (_ bv0 32))";
        break;
      case Op::SLTIU:
        expr = "(ite (bvult " + src(insn.rs1) + " " + bv32(imm32(insn.imm)) + ") (_ bv1 32) (_ bv0 32))";
        break;
      case Op::LUI:
        expr = "(bvshl " + bv32(static_cast<uint32_t>(insn.imm) & 0xFFFFFu) + " (_ bv12 32))";
        break;
      case Op::NOP:
        break;
    }

    if (insn.op != Op::NOP && insn.rd != 0 && !expr.empty()) {
      std::string var = prefix + "_v" + std::to_string(step++);
      defs.push_back("(define-fun " + var + " () (_ BitVec 32) " + expr + ")");
      cur[insn.rd] = var;
    }
  }

  for (int r : live_out) {
    if (r < 0 || r >= 32) continue;
    out_expr[r] = cur[r].empty() ? src(r) : cur[r];
  }
}

// Run z3 with the given SMT-LIB2 text, capture stdout.
std::string run_z3(const std::string& smt) {
  char tmpl[] = "/tmp/sopt_verify_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) return "";

  FILE* f = fdopen(fd, "w");
  if (!f) {
    close(fd);
    unlink(tmpl);
    return "";
  }
  fputs(smt.c_str(), f);
  fclose(f);

  std::string cmd = std::string("z3 -smt2 ") + tmpl + " 2>/dev/null";
  std::string result;
  FILE* pipe = popen(cmd.c_str(), "r");
  if (pipe) {
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) result += buf;
    pclose(pipe);
  }
  unlink(tmpl);
  return result;
}

VerifyResult z3_verify_equivalent(const ProgramSpec& target,
                                  const std::vector<Insn>& candidate,
                                  const std::vector<int>& live_out) {
  VerifyResult res;
  res.equivalent = false;
  res.status = "z3_unavailable";
  if (!z3_available()) return res;

  std::vector<int> outs;
  for (int r : live_out) {
    if (r >= 0 && r < 32) outs.push_back(r);
  }
  if (outs.empty()) {
    res.equivalent = true;
    res.status = "unsat";
    return res;
  }

  std::set<int> inputs;
  std::vector<std::string> defs;
  std::unordered_map<int, std::string> out_t, out_c;

  encode_program(target.code, target.live_in, outs, "t", inputs, defs, out_t);
  encode_program(candidate, target.live_in, outs, "c", inputs, defs, out_c);

  std::ostringstream smt;
  smt << "; QF_BV equivalence check (all live-outs)\n";
  smt << "(set-logic QF_BV)\n";
  for (int r : inputs) {
    smt << "(declare-fun " << input_name(r) << " () (_ BitVec 32))\n";
  }
  for (const auto& d : defs) smt << d << "\n";

  // The programs are different iff at least one live-out differs.
  smt << "\n(assert (or";
  for (int r : outs) {
    const std::string& te = out_t[r];
    const std::string& ce = out_c[r];
    smt << "\n  (not (= " << te << " " << ce << "))";
  }
  smt << "))\n(check-sat)\n(get-model)\n";

  std::string output = run_z3(smt.str());
  if (output.empty()) {
    res.status = "unknown";
    return res;
  }

  bool sat = false, unsat = false, unknown = false;
  {
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
      std::string tok = trim_copy(line);
      if (tok == "sat") sat = true;
      else if (tok == "unsat") unsat = true;
      else if (tok == "unknown") unknown = true;
    }
  }

  if (unsat) {
    res.equivalent = true;
    res.status = "unsat";
    return res;
  }
  if (!sat) {
    res.status = unknown ? "unknown" : "error";
    return res;
  }

  res.status = "sat";
  // Parse counterexample model entries. Z3 may spread a definition over
  // several lines, so extract parenthesis-balanced `(define-fun ...)` blocks.
  const std::string kDefine = "(define-fun ";
  size_t pos = 0;
  while ((pos = output.find(kDefine, pos)) != std::string::npos) {
    size_t start = pos;
    int depth = 0;
    size_t end = std::string::npos;
    for (size_t i = start; i < output.size(); i++) {
      if (output[i] == '(') depth++;
      else if (output[i] == ')') {
        depth--;
        if (depth == 0) { end = i; break; }
      }
    }
    if (end == std::string::npos) break;

    std::string block = output.substr(start, end - start + 1);
    pos = end + 1;

    size_t name_begin = kDefine.size();
    size_t name_end = block.find_first_of(" \t\r\n", name_begin);
    if (name_end == std::string::npos) continue;
    std::string name = block.substr(name_begin, name_end - name_begin);
    if (name.rfind("in_x", 0) != 0) continue;

    int r = -1;
    try { r = std::stoi(name.substr(4)); } catch (...) { continue; }
    if (r <= 0 || r >= 32) continue;

    uint32_t val = 0;
    bool have_val = false;
    size_t hx = block.find("#x", name_end);
    if (hx != std::string::npos) {
      size_t hend = block.find_first_of(" )\t\r\n", hx + 2);
      std::string hexv = block.substr(hx + 2, hend - (hx + 2));
      try { val = static_cast<uint32_t>(std::stoul(hexv, nullptr, 16)); have_val = true; } catch (...) {}
    } else {
      size_t bv = block.find("(_ bv", name_end);
      if (bv != std::string::npos) {
        size_t vbeg = bv + 5;
        size_t vend = block.find_first_of(" )\t\r\n", vbeg);
        if (vend != std::string::npos) {
          try { val = static_cast<uint32_t>(std::stoul(block.substr(vbeg, vend - vbeg))); have_val = true; } catch (...) {}
        }
      }
    }
    if (have_val) res.counterexample.emplace_back(r, val);
  }
  return res;
}

std::optional<TestCase> z3_counterexample(const ProgramSpec& target,
                                          const std::vector<Insn>& candidate,
                                          const std::vector<int>& live_out) {
  VerifyResult vr = z3_verify_equivalent(target, candidate, live_out);
  if (vr.equivalent || vr.status != "sat") return std::nullopt;

  TestCase tc;
  for (int i = 0; i < 32; i++) tc.in[i] = rand32();
  tc.in[0] = 0;
  tc.out_target.fill(0);
  for (const auto& [r, val] : vr.counterexample) {
    if (r >= 0 && r < 32) tc.in[r] = val;
  }
  {
    State st = tc.in;
    run_program(target.code.empty() ? nullptr : target.code.data(),
                static_cast<int>(target.code.size()), st);
    for (int r : live_out) {
      if (r >= 0 && r < 32) tc.out_target[r] = st[r];
    }
  }
  return tc;
}

} // namespace sopt
