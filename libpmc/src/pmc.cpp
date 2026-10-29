#include "pmc/pmc.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace pmc {

namespace {

struct Token {
  std::string text;
  int line;
};

std::vector<Token> tokenize(std::string_view text) {
  std::vector<Token> tokens;
  int line = 1;
  for (size_t i = 0; i < text.size();) {
    unsigned char c = text[i];
    if (c == '\n') {
      ++line;
      ++i;
    } else if (std::isspace(c)) {
      ++i;
    } else if (c == ';') {
      while (i < text.size() && text[i] != '\n') ++i;
    } else if (std::isalpha(c) || std::isdigit(c)) {
      bool letters = std::isalpha(c);
      size_t j = i;
      while (j < text.size() && (letters ? std::isalpha((unsigned char)text[j])
                                         : std::isdigit((unsigned char)text[j]))) {
        ++j;
      }
      tokens.push_back({std::string(text.substr(i, j - i)), line});
      i = j;
    } else {
      tokens.push_back({std::string(1, static_cast<char>(c)), line});
      ++i;
    }
  }
  return tokens;
}

}

std::optional<Program> parse(std::string_view text, std::string& error) {
  std::vector<Token> tokens = tokenize(text);
  Program program;
  size_t i = 0;
  auto expected = [&](const std::string& what) {
    error = i >= tokens.size() ? "expected " + what + " at the end of the file"
                               : "line " + std::to_string(tokens[i].line) + ": expected " + what +
                                     ", found '" + tokens[i].text + "'";
    return std::nullopt;
  };
  auto is = [&](const char* text) { return i < tokens.size() && tokens[i].text == text; };
  auto is_number = [&] { return i < tokens.size() && std::isdigit((unsigned char)tokens[i].text[0]); };
  auto number = [&](long max, long& value) {
    if (!is_number() || tokens[i].text.size() > 6) return false;
    value = std::stol(tokens[i].text);
    if (value > max) return false;
    ++i;
    return true;
  };

  while (i < tokens.size()) {
    int line = tokens[i].line;
    long address = 0, value = 0;
    if (!is("MEM")) return expected("MEM");
    ++i;
    if (!is("[")) return expected("[");
    ++i;
    if (!number(kCells - 1, address)) return expected("an address from 0 to 511");
    if (!is("]")) return expected("]");
    ++i;
    if (!is(":")) return expected(":");
    ++i;

    bool instruction = false;
    if (is_number()) {
      if (!number(65535, value)) return expected("a value from 0 to 65535");
    } else if (i < tokens.size() && !is("MEM")) {
      long opcode = 0;
      while (opcode < 16 && tokens[i].text != kOpcodes[opcode]) ++opcode;
      if (opcode == 16) return expected("a value, an instruction or the next MEM");
      ++i;
      instruction = true;
      if (opcode > 0) {
        size_t mode = i < tokens.size() && tokens[i].text.size() == 1 ? kModes.find(tokens[i].text[0])
                                                                      : std::string_view::npos;
        if (mode == std::string_view::npos) return expected("a mode: C, D, I or N");
        ++i;
        long argument = 0;
        if (!number(kCells - 1, argument)) return expected("an argument from 0 to 511");
        value = opcode << 11 | static_cast<long>(mode) << 9 | argument;
      }
    }

    if (program.line_of.count(address)) {
      error = "line " + std::to_string(line) + ": MEM[" + std::to_string(address) +
              "] is already defined on line " + std::to_string(program.line_of[address]);
      return std::nullopt;
    }
    program.memory[address] = static_cast<Word>(value);
    program.line_of[address] = line;
    program.address_at.emplace(line, address);
    if (instruction && (!program.first_instruction || address < *program.first_instruction)) {
      program.first_instruction = static_cast<int>(address);
    }
  }
  return program;
}

std::optional<Program> load(const std::string& path, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "Cannot read " + path;
    return std::nullopt;
  }
  std::stringstream text;
  text << file.rdbuf();
  std::optional<Program> program = parse(text.str(), error);
  if (!program) error = path + ": " + error;
  return program;
}

Machine::Machine(const Program& program, int start)
    : memory(program.memory), initial(program.memory), pc(start) {}

void Machine::step() {
  int at = pc;
  ir = memory[at];
  pc = (at + 1) % kCells;
  int opcode = ir >> 11, mode = ir >> 9 & 3, argument = ir & 0x1FF;
  switch (mode) {
    case 0: or_ = static_cast<Word>(argument); break;
    case 1: or_ = memory[argument]; break;
    case 2: or_ = memory[memory[argument] % kCells]; break;
    default: or_ = memory[(ac + argument) % kCells]; break;
  }
  auto s = [](Word v) { return static_cast<int>(static_cast<int16_t>(v)); };
  auto set = [this](long v) { ac = static_cast<Word>(v); };
  switch (opcode) {
    case 0: halted = true; pc = at; break;
    case 1: ac = or_; break;
    case 2: memory[or_ % kCells] = ac; break;
    case 3: pc = or_ % kCells; break;
    case 4: if (s(ac) < 0) pc = or_ % kCells; break;
    case 5: if (ac == 0) pc = or_ % kCells; break;
    case 6: set(ac + or_); break;
    case 7: set(ac - or_); break;
    case 8: set(s(ac) * s(or_)); break;
    case 9:
      if (or_ == 0) {
        error = "division by zero";
        pc = at;
      } else {
        set(s(ac) / s(or_));
      }
      break;
    case 10: ac &= or_; break;
    case 11: ac |= or_; break;
    case 12: set(~or_); break;
    case 13: ac = ac == or_ ? 0xFFFF : 0; break;
    case 14: {
      int n = s(or_);
      set(n >= 16 || n <= -16 ? 0 : n >= 0 ? ac << n : ac >> -n);
      break;
    }
    case 15: {
      int n = (s(or_) % 16 + 16) % 16;
      set(ac << n | ac >> (16 - n));
      break;
    }
    default:
      error = std::to_string(ir) + " is not an instruction";
      pc = at;
      break;
  }
}

long Machine::run(long max_steps, const std::function<bool(const Machine&)>& pause) {
  long steps = 0;
  while (running() && steps < max_steps) {
    step();
    ++steps;
    if (pause && running() && pause(*this)) break;
  }
  return steps;
}

std::vector<int> Machine::changed() const {
  std::vector<int> addresses;
  for (int address = 0; address < kCells; ++address) {
    if (memory[address] != initial[address]) addresses.push_back(address);
  }
  return addresses;
}

std::string show(Word value) {
  std::string text = std::to_string(value);
  if (value >= 0x8000) text += " (" + std::to_string(static_cast<int16_t>(value)) + ")";
  return text;
}

std::string disassemble(Word word) {
  int opcode = word >> 11;
  if (opcode > 15) return "not an instruction";
  if (opcode == 0) return "stop";
  return std::string(kOpcodes[opcode]) + " " + kModes[word >> 9 & 3] + " " + std::to_string(word & 0x1FF);
}

}
