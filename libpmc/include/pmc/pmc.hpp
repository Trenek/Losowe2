#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pmc {

constexpr int kCells = 512;
using Word = std::uint16_t;
using Memory = std::array<Word, kCells>;

inline constexpr std::array<std::string_view, 16> kOpcodes = {
    "stop", "load", "store", "jump", "jneg", "jzero", "add", "sub",
    "mult", "div",  "and",   "or",   "not",  "cmp",   "shz", "shc"};
inline constexpr std::string_view kModes = "CDIN";

struct Program {
  Memory memory{};
  std::map<int, int> line_of;
  std::map<int, int> address_at;
  std::optional<int> first_instruction;
};

std::optional<Program> parse(std::string_view text, std::string& error);
std::optional<Program> load(const std::string& path, std::string& error);

struct Machine {
  Machine() = default;
  Machine(const Program& program, int start);

  Memory memory{};
  Memory initial{};
  Word ac = 0, or_ = 0, ir = 0;
  int pc = 0;
  bool halted = false;
  std::string error;

  bool running() const { return !halted && error.empty(); }
  void step();
  long run(long max_steps, const std::function<bool(const Machine&)>& pause = nullptr);
  std::vector<int> changed() const;
};

std::string show(Word value);
std::string disassemble(Word word);

}
