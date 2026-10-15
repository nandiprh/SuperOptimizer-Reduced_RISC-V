#include "isa.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <vector>
#include <array>
#include <utility>

namespace sopt {

const OpInfo kOps[] = {
  {"nop",   Fmt::None,   1, 0, 0},
  {"add",   Fmt::R,      1, 0, 0},
  {"sub",   Fmt::R,      1, 0, 0},
  {"and",   Fmt::R,      1, 0, 0},
  {"or",    Fmt::R,      1, 0, 0},
  {"xor",   Fmt::R,      1, 0, 0},
  {"sll",   Fmt::R,      1, 0, 0},
  {"srl",   Fmt::R,      1, 0, 0},
  {"sra",   Fmt::R,      1, 0, 0},
  {"slt",   Fmt::R,      1, 0, 0},
  {"sltu",  Fmt::R,      1, 0, 0},
  {"addi",  Fmt::I,      1, -2048, 2047},
  {"andi",  Fmt::I,      1, -2048, 2047},
  {"ori",   Fmt::I,      1, -2048, 2047},
  {"xori",  Fmt::I,      1, -2048, 2047},
  {"slli",  Fmt::IShift, 1, 0, 31},
  {"srli",  Fmt::IShift, 1, 0, 31},
  {"srai",  Fmt::IShift, 1, 0, 31},
  {"slti",  Fmt::I,      1, -2048, 2047},
  {"sltiu", Fmt::I,      1, -2048, 2047},
  {"lui",   Fmt::U,      1, 0, 0xFFFFF},
};

static_assert(sizeof(kOps) / sizeof(kOps[0]) == 21, "kOps size mismatch");

const std::array<const char*, 32> kRegNames = {
  "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
  "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
  "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
  "x24", "x25", "x26", "x27", "x28", "x29", "x30", "x31"
};

int reg_name_to_num(const std::string& s) {
  // map common ABI names to xN
  if (s == "zero" || s == "x0") return 0;
  if (s == "ra" || s == "x1") return 1;
  if (s == "sp" || s == "x2") return 2;
  if (s == "gp" || s == "x3") return 3;
  if (s == "tp" || s == "x4") return 4;
  if (s == "t0" || s == "x5") return 5;
  if (s == "t1" || s == "x6") return 6;
  if (s == "t2" || s == "x7") return 7;
  if (s == "s0" || s == "fp" || s == "x8") return 8;
  if (s == "s1" || s == "x9") return 9;
  if (s == "a0" || s == "x10") return 10;
  if (s == "a1" || s == "x11") return 11;
  if (s == "a2" || s == "x12") return 12;
  if (s == "a3" || s == "x13") return 13;
  if (s == "a4" || s == "x14") return 14;
  if (s == "a5" || s == "x15") return 15;
  if (s == "a6" || s == "x16") return 16;
  if (s == "a7" || s == "x17") return 17;
  if (s == "s2" || s == "x18") return 18;
  if (s == "s3" || s == "x19") return 19;
  if (s == "s4" || s == "x20") return 20;
  if (s == "s5" || s == "x21") return 21;
  if (s == "s6" || s == "x22") return 22;
  if (s == "s7" || s == "x23") return 23;
  if (s == "s8" || s == "x24") return 24;
  if (s == "s9" || s == "x25") return 25;
  if (s == "s10" || s == "x26") return 26;
  if (s == "s11" || s == "x27") return 27;
  if (s == "t3" || s == "x28") return 28;
  if (s == "t4" || s == "x29") return 29;
  if (s == "t5" || s == "x30") return 30;
  if (s == "t6" || s == "x31") return 31;
  if (s.size() >= 2 && s[0] == 'x') {
    int n = 0;
    for (size_t i = 1; i < s.size(); i++) {
      if (!std::isdigit(static_cast<unsigned char>(s[i]))) return -1;
      n = n * 10 + (s[i] - '0');
    }
    if (n >= 0 && n < 32) return n;
  }
  return -1;
}

std::string op_to_name(Op op) {
  size_t idx = static_cast<size_t>(op);
  if (idx < sizeof(kOps) / sizeof(kOps[0])) return kOps[idx].name;
  return "nop";
}

Op name_to_op(const std::string& name) {
  std::string n = name;
  std::transform(n.begin(), n.end(), n.begin(), ::tolower);
  for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); i++) {
    if (n == kOps[i].name) return static_cast<Op>(i);
  }
  return Op::NOP;
}

bool is_valid_reg(uint8_t r) {
  return r < 32;
}

bool is_nop(const Insn& insn) {
  return insn.op == Op::NOP || (insn.op == Op::ADDI && insn.rd == 0 && insn.rs1 == 0 && insn.imm == 0);
}

Insn make_nop() {
  Insn insn;
  insn.op = Op::NOP;
  insn.rd = insn.rs1 = insn.rs2 = 0;
  insn.imm = 0;
  return insn;
}

static std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(s[b])) b++;
  while (e > b && std::isspace(s[e - 1])) e--;
  return s.substr(b, e - b);
}

std::string format_insn(const Insn& insn) {
  if (is_nop(insn)) return "nop";
  auto idx = static_cast<size_t>(insn.op);
  const auto& info = kOps[idx];
  std::string name = info.name;
  std::ostringstream ss;
  switch (info.fmt) {
    case Fmt::R:
      ss << name << " x" << (int)insn.rd << ", x" << (int)insn.rs1 << ", x" << (int)insn.rs2;
      break;
    case Fmt::I:
      ss << name << " x" << (int)insn.rd << ", x" << (int)insn.rs1 << ", " << insn.imm;
      break;
    case Fmt::IShift:
      ss << name << " x" << (int)insn.rd << ", x" << (int)insn.rs1 << ", " << insn.imm;
      break;
    case Fmt::U:
      ss << name << " x" << (int)insn.rd << ", " << insn.imm;
      break;
    case Fmt::None:
      ss << name;
      break;
  }
  return ss.str();
}

bool parse_insn(const std::string& line, Insn& out) {
  std::string s = trim(line);
  if (s.empty() || s[0] == '#' || s[0] == ';') return false;
  if (s[0] == '.' || s.back() == ':' || (s[0] == '\t' && s.find(':') != std::string::npos)) return false;
  // remove inline comments
  size_t cpos = s.find('#');
  if (cpos != std::string::npos) s = trim(s.substr(0, cpos));
  cpos = s.find(';');
  if (cpos != std::string::npos) s = trim(s.substr(0, cpos));
  if (s.empty()) return false;
  // get op name
  std::istringstream iss(s);
  std::string opname;
  iss >> opname;
  out.op = name_to_op(opname);
  if (out.op == Op::NOP && opname != "nop") {
    // try to be lenient
  }
  size_t idx = static_cast<size_t>(out.op);
  if (idx >= sizeof(kOps) / sizeof(kOps[0])) return false;
  const auto& info = kOps[idx];
  out.rd = out.rs1 = out.rs2 = 0; out.imm = 0;
  // consume rest
  std::string rest;
  std::getline(iss, rest);
  rest = trim(rest);
  // remove commas
  std::vector<std::string> parts;
  std::string cur;
  for (char ch : rest) {
    if (ch == ',' || ch == ' ' || ch == '\t') {
      if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  switch (info.fmt) {
    case Fmt::R:
      if (parts.size() != 3) return false;
      out.rd = (uint8_t)reg_name_to_num(parts[0]);
      out.rs1 = (uint8_t)reg_name_to_num(parts[1]);
      out.rs2 = (uint8_t)reg_name_to_num(parts[2]);
      break;
    case Fmt::IShift:
      if (parts.size() != 3) return false;
      out.rd = (uint8_t)reg_name_to_num(parts[0]);
      out.rs1 = (uint8_t)reg_name_to_num(parts[1]);
      // shamt
      {
        long long v = 0;
        bool neg = false;
        size_t i = 0;
        if (parts[2].size() > 0 && parts[2][0] == '-') { neg = true; i = 1; }
        if (parts[2].size() > 0 && parts[2][0] == '+') i = 1;
        for (; i < parts[2].size(); i++) if (std::isdigit(parts[2][i])) v = v*10 + (parts[2][i]-'0');
        if (neg) v = -v;
        out.imm = (int32_t)v;
      }
      break;
    case Fmt::I:
      if (parts.size() != 3) return false;
      out.rd = (uint8_t)reg_name_to_num(parts[0]);
      out.rs1 = (uint8_t)reg_name_to_num(parts[1]);
      {
        long long v = 0;
        bool neg = false;
        size_t i = 0;
        if (parts[2].size() > 0 && parts[2][0] == '-') { neg = true; i = 1; }
        if (parts[2].size() > 0 && parts[2][0] == '+') i = 1;
        if (i+1 < parts[2].size() && parts[2][i] == '0' && (parts[2][i+1] == 'x' || parts[2][i+1] == 'X')) {
          i += 2;
          for (; i < parts[2].size(); i++) {
            char c = parts[2][i];
            if (std::isdigit(c)) v = v*16 + (c-'0');
            else if (c >= 'a' && c <= 'f') v = v*16 + (10 + (c-'a'));
            else if (c >= 'A' && c <= 'F') v = v*16 + (10 + (c-'A'));
            else break;
          }
        } else {
          for (; i < parts[2].size(); i++) if (std::isdigit(parts[2][i])) v = v*10 + (parts[2][i]-'0');
        }
        if (neg) v = -v;
        out.imm = (int32_t)v;
      }
      break;
    case Fmt::U:
      if (parts.size() != 2) return false;
      out.rd = (uint8_t)reg_name_to_num(parts[0]);
      {
        long long v = 0;
        bool neg = false;
        size_t i = 0;
        if (parts[1].size() > 0 && parts[1][0] == '-') { neg = true; i = 1; }
        if (parts[1].size() > 0 && parts[1][0] == '+') i = 1;
        if (i+1 < parts[1].size() && parts[1][i] == '0' && (parts[1][i+1] == 'x' || parts[1][i+1] == 'X')) {
          i += 2;
          for (; i < parts[1].size(); i++) {
            char c = parts[1][i];
            if (std::isdigit(c)) v = v*16 + (c-'0');
            else if (c >= 'a' && c <= 'f') v = v*16 + (10 + (c-'a'));
            else if (c >= 'A' && c <= 'F') v = v*16 + (10 + (c-'A'));
            else break;
          }
        } else {
          for (; i < parts[1].size(); i++) if (std::isdigit(parts[1][i])) v = v*10 + (parts[1][i]-'0');
        }
        if (neg) v = -v;
        out.imm = (int32_t)v;
      }
      break;
    case Fmt::None:
      if (parts.size() != 0 && !rest.empty()) return false;
      break;
  }
  if (out.op == Op::NOP) {
    out.rd = out.rs1 = out.rs2 = 0; out.imm = 0;
  }
  // validate register numbers and immediate ranges
  if (!is_valid_reg(out.rd) || !is_valid_reg(out.rs1) || !is_valid_reg(out.rs2)) return false;
  if ((info.fmt == Fmt::I || info.fmt == Fmt::IShift || info.fmt == Fmt::U) &&
      (out.imm < info.imm_min || out.imm > info.imm_max)) return false;
  // normalize nop form
  if (is_nop(out)) { out.op = Op::NOP; out.rd=out.rs1=out.rs2=0; out.imm=0; }
  return true;
}

} // namespace sopt
