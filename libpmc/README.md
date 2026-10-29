# libpmc

PMC programs and the PMC machine, as a library with no dependencies. pmc-dap
runs programs with it, and an interpreter can use it too.

## API

Everything is in `#include <pmc/pmc.hpp>`, in the namespace `pmc`:

- `load(path, error)` and `parse(text, error)` read a program. A `Program`
  holds the memory, the line of each cell (`line_of`, and `address_at` the
  other way round) and `first_instruction`, where programs start by default.
  On a syntax error they return nothing and `error` says where, e.g.
  `line 2: expected a mode: C, D, I or N, found 'X'`.
- `Machine(program, start)` is the machine with the program's memory and PC at
  `start`. Its state is public: `memory`, `ac`, `or_` (OR), `pc`, `ir`, and
  `halted` and `error`, which tell why it stopped; `running()` is true while
  neither is set.
- `machine.step()` executes one instruction.
- `machine.run(max_steps, pause)` runs until `stop`, an error, `pause`
  returning true after a step, or `max_steps` instructions.
- `machine.changed()` lists the addresses whose values differ from the program's.
- `show(value)` gives `65532 (-4)` and `disassemble(word)` gives `load D 20`.

## Using it

In this repository, as pmc-dap does:

```cmake
add_subdirectory(libpmc)
target_link_libraries(pmc-dap PRIVATE pmc::pmc)
```

In another project, CMake can download just this directory:

```cmake
include(FetchContent)
FetchContent_Declare(libpmc
  URL https://github.com/USERNAME/pmc-dap/archive/v0.1.0.tar.gz
  SOURCE_SUBDIR libpmc)
FetchContent_MakeAvailable(libpmc)
target_link_libraries(pmc-run PRIVATE pmc::pmc)
```

## Example: an interpreter

```cpp
// pmc-run: runs a PMC program and prints the cells it changed.
#include <pmc/pmc.hpp>

#include <cstdio>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: pmc-run PROGRAM.pmc\n");
    return 2;
  }
  std::string error;
  std::optional<pmc::Program> program = pmc::load(argv[1], error);
  if (!program) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  if (!program->first_instruction) {
    std::fprintf(stderr, "%s has no instructions\n", argv[1]);
    return 1;
  }
  pmc::Machine machine(*program, *program->first_instruction);
  machine.run(1000000);
  for (int address : machine.changed()) {
    std::printf("MEM[%d] = %s\n", address, pmc::show(machine.memory[address]).c_str());
  }
  if (!machine.error.empty()) {
    std::fprintf(stderr, "error at MEM[%d]: %s\n", machine.pc, machine.error.c_str());
    return 1;
  }
  if (machine.running()) {
    std::fprintf(stderr, "still running after a million instructions\n");
    return 1;
  }
  return 0;
}
```

```
$ pmc-run zadanie.pmc
MEM[2] = 65532 (-4)
MEM[3] = 65534 (-2)
MEM[4] = 2
MEM[6] = 2
```

## The machine

512 cells of 16 bits. An instruction word is the opcode (5 bits), the mode
(2 bits) and the argument (9 bits). Opcodes go stop, load, store, jump, jneg,
jzero, add, sub, mult, div, and, or, not, cmp, shz, shc = 0 to 15, and the
modes C, D, I, N = 0 to 3:

- C: OR ← argument
- D: OR ← MEM[argument]
- I: OR ← MEM[MEM[argument]]
- N: OR ← MEM[AC + argument]

Arithmetic is 16-bit two's complement, and addresses wrap around at 512. For
`shz` and `shc`, a positive OR shifts left and a negative one right. Division
by zero, or a word that isn't an instruction, sets `error`.
