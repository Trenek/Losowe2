# pmc-dap

A debug adapter ([Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/))
for PMC assembly. It runs programs on [libpmc](libpmc/), the PMC machine, which
is a library of its own that an interpreter can use too. It stops on entry, at
breakpoints, after each step, and when the program halts. In Neovim, use it
through [pmc-dap.nvim](https://github.com/USERNAME/pmc-dap.nvim).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

This needs CMake 3.16 or newer, a C++17 compiler, and internet access: CMake
downloads [cppdap](https://github.com/google/cppdap) and
[nlohmann/json](https://github.com/nlohmann/json). The adapter is
`build/pmc-dap`; it talks DAP on stdin and stdout.

## Launch configuration

| Field         | Meaning                                                               |
| ------------- | --------------------------------------------------------------------- |
| `program`     | the `.pmc` file                                                       |
| `stopOnEntry` | stop before the first instruction (default `true`)                    |
| `start`       | the address to start at (default: the first cell with an instruction) |

## What it reports

- The scopes "Registers" (AC, OR, PC, IR) and "Changed memory": the cells
  whose values differ from the file.
- After every stop, the custom event `pmcCells`: those cells, each with the
  line that defines it (0 if the file doesn't). pmc-dap.nvim shows them there.
- `evaluate` (the REPL, hovers): `AC`, `OR`, `PC`, `IR` and `MEM[n]`.

Values are 16-bit and shown as in program files, with the signed value when it
is negative: `65532 (-4)`.

When the program executes `stop`, it stops there so you can look at the
results; continuing or stepping again ends the session. An instruction that
fails (see [the machine](libpmc/README.md#the-machine)) stops it with an
error. A continue that runs a million instructions without stopping pauses.

## Layout

```
libpmc/        the PMC machine: reading and running programs, no dependencies
src/main.cpp   the debug adapter: DAP on top of libpmc
```

## Releasing a new version

1. Commit, then `git tag v0.2.0` and `git push --tags`.
2. In pmc-dap.nvim, change the tag in `lua/pmc-dap/package.lua`, commit and push.
