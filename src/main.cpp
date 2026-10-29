// pmc-dap: a debug adapter (Debug Adapter Protocol) for PMC assembly.
//
// It runs a program on libpmc's machine and stops on entry, at breakpoints,
// after each step, and when the program halts or fails. After every stop it
// sends the custom "pmcCells" event: the memory cells whose values differ from
// the program file, each with the line that defines it, so that an editor can
// show the new values there. The program starts at the first cell that holds
// an instruction, unless the launch configuration gives "start". DAP itself
// comes from cppdap (https://github.com/google/cppdap), over stdin and stdout.

#include <dap/io.h>
#include <dap/protocol.h>
#include <dap/session.h>
#include <pmc/pmc.hpp>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace pmc_dap {

constexpr long kStepLimit = 1000000;  // instructions per "continue" before pausing

// ---- DAP types of this adapter -----------------------------------------------

struct LaunchRequest : public dap::LaunchRequest {
  dap::string program;
  dap::optional<dap::boolean> stopOnEntry;  // default true
  dap::optional<dap::integer> start;        // default: the first instruction
};

struct Cell {
  dap::integer address;
  dap::integer line;  // 0 when the program file does not define the cell
  dap::string value;
};

struct CellsEvent : public dap::Event {
  dap::array<Cell> cells;
};

}  // namespace pmc_dap

namespace dap {
DAP_STRUCT_TYPEINFO_EXT(pmc_dap::LaunchRequest, LaunchRequest, "launch",
                        DAP_FIELD(program, "program"), DAP_FIELD(stopOnEntry, "stopOnEntry"),
                        DAP_FIELD(start, "start"));
DAP_STRUCT_TYPEINFO(pmc_dap::Cell, "", DAP_FIELD(address, "address"), DAP_FIELD(line, "line"),
                    DAP_FIELD(value, "value"));
DAP_STRUCT_TYPEINFO(pmc_dap::CellsEvent, "pmcCells", DAP_FIELD(cells, "cells"));
}  // namespace dap

namespace pmc_dap {

// ---- The debugger: DAP requests on one program -------------------------------
// cppdap calls the handlers one at a time, so nothing here needs a lock.

class Debugger {
 public:
  explicit Debugger(dap::Session& session) : session_(session) {}

  dap::ResponseOrError<dap::LaunchResponse> launch(const LaunchRequest& request) {
    std::string error;
    std::optional<pmc::Program> program = pmc::load(request.program, error);
    if (!program) return dap::Error(error);
    long start = request.start ? static_cast<long>(*request.start) : program->first_instruction.value_or(-1);
    if (start < 0 || start >= pmc::kCells) {
      return dap::Error(request.start ? "start must be from 0 to 511"
                                      : request.program + " has no instructions");
    }
    program_ = std::move(*program);
    machine_ = pmc::Machine(program_, static_cast<int>(start));
    path_ = request.program;
    stop_on_entry_ = request.stopOnEntry.value(true);
    return dap::LaunchResponse();
  }

  dap::SetBreakpointsResponse set_breakpoints(const dap::SetBreakpointsRequest& request) {
    bool ours = same_file(request.source.path.value(""), path_);
    if (ours) breakpoints_.clear();
    dap::SetBreakpointsResponse response;
    for (const auto& wanted : request.breakpoints.value({})) {
      dap::Breakpoint breakpoint;
      breakpoint.line = wanted.line;
      auto cell = program_.address_at.find(static_cast<int>(wanted.line));
      breakpoint.verified = ours && cell != program_.address_at.end();
      if (breakpoint.verified) {
        breakpoints_.insert(cell->second);
      } else {
        breakpoint.message = "No memory cell on this line";
      }
      response.breakpoints.push_back(breakpoint);
    }
    return response;
  }

  // After configurationDone.
  void begin() {
    if (stop_on_entry_) return stopped("entry");
    if (breakpoints_.count(machine_.pc)) return stopped("breakpoint");
    run();
  }

  void run() {
    if (!machine_.running()) return end();
    machine_.run(kStepLimit, [this](const pmc::Machine& m) { return breakpoints_.count(m.pc) > 0; });
    if (!machine_.running()) return stopped_at_end();
    if (breakpoints_.count(machine_.pc)) return stopped("breakpoint");
    stopped("pause", "Paused after a million instructions");
  }

  void step() {
    if (!machine_.running()) return end();
    machine_.step();
    if (!machine_.running()) return stopped_at_end();
    stopped("step");
  }

  dap::StackTraceResponse stack_trace() const {
    dap::StackFrame frame;
    frame.id = 1;
    frame.name = "MEM[" + std::to_string(machine_.pc) + "]: " + pmc::disassemble(machine_.memory[machine_.pc]);
    auto line = program_.line_of.find(machine_.pc);
    if (line != program_.line_of.end()) {
      dap::Source source;
      source.path = path_;
      source.name = std::filesystem::path(path_).filename().string();
      frame.source = source;
      frame.line = line->second;
      frame.column = 1;
    }
    dap::StackTraceResponse response;
    response.stackFrames.push_back(frame);
    response.totalFrames = 1;
    return response;
  }

  static dap::ScopesResponse scopes() {
    dap::Scope registers, memory;
    registers.name = "Registers";
    registers.presentationHint = "registers";
    registers.variablesReference = 1;
    memory.name = "Changed memory";
    memory.variablesReference = 2;
    dap::ScopesResponse response;
    response.scopes = {registers, memory};
    return response;
  }

  // For the REPL and hovers: AC, OR, PC, IR or MEM[n].
  dap::ResponseOrError<dap::EvaluateResponse> evaluate(const std::string& expression) const {
    std::string e;
    for (char c : expression) {
      if (!std::isspace(static_cast<unsigned char>(c))) e += c;
    }
    dap::EvaluateResponse response;
    response.variablesReference = 0;
    if (e == "AC") {
      response.result = pmc::show(machine_.ac);
    } else if (e == "OR") {
      response.result = pmc::show(machine_.or_);
    } else if (e == "PC") {
      response.result = std::to_string(machine_.pc);
    } else if (e == "IR") {
      response.result = pmc::show(machine_.ir) + ": " + pmc::disassemble(machine_.ir);
    } else if (e.size() > 5 && e.size() <= 8 && e.compare(0, 4, "MEM[") == 0 && e.back() == ']' &&
               e.find_first_not_of("0123456789", 4) == e.size() - 1 && std::stoi(e.substr(4)) < pmc::kCells) {
      response.result = pmc::show(machine_.memory[std::stoi(e.substr(4))]);
    } else {
      return dap::Error("Use AC, OR, PC, IR or MEM[n]");
    }
    return response;
  }

  dap::VariablesResponse variables(int64_t reference) const {
    dap::VariablesResponse response;
    auto add = [&](const std::string& name, const std::string& value) {
      dap::Variable variable;
      variable.name = name;
      variable.value = value;
      variable.variablesReference = 0;
      response.variables.push_back(variable);
    };
    if (reference == 1) {
      add("AC", pmc::show(machine_.ac));
      add("OR", pmc::show(machine_.or_));
      add("PC", std::to_string(machine_.pc));
      add("IR", pmc::show(machine_.ir) + ": " + pmc::disassemble(machine_.ir));
    } else if (reference == 2) {
      for (int address : machine_.changed()) {
        add("MEM[" + std::to_string(address) + "]", pmc::show(machine_.memory[address]));
      }
    }
    return response;
  }

 private:
  void stopped(const std::string& reason, const std::string& description = "") {
    CellsEvent cells;
    for (int address : machine_.changed()) {
      auto line = program_.line_of.find(address);
      Cell cell;
      cell.address = address;
      cell.line = line == program_.line_of.end() ? 0 : line->second;
      cell.value = pmc::show(machine_.memory[address]);
      cells.cells.push_back(cell);
    }
    session_.send(cells);
    dap::StoppedEvent event;
    event.reason = reason;
    event.threadId = 1;
    event.allThreadsStopped = true;
    if (!description.empty()) {
      event.description = description;
      event.text = description;
    }
    session_.send(event);
  }

  // The program halted or an instruction failed: stop there, so that the
  // final values can be looked at. Continuing or stepping again ends it.
  void stopped_at_end() {
    std::string where = " at MEM[" + std::to_string(machine_.pc) + "]";
    dap::OutputEvent output;
    output.category = "console";
    if (machine_.halted) {
      output.output = "The program halted" + where + ".\n";
      session_.send(output);
      stopped("halt", "The program halted");
    } else {
      output.output = "Error" + where + ": " + machine_.error + "\n";
      session_.send(output);
      stopped("exception", machine_.error);
    }
  }

  void end() {
    dap::ExitedEvent exited;
    exited.exitCode = machine_.halted ? 0 : 1;
    session_.send(exited);
    session_.send(dap::TerminatedEvent());
  }

  static bool same_file(const std::string& a, const std::string& b) {
    std::error_code error;
    return !a.empty() && !b.empty() &&
           std::filesystem::weakly_canonical(a, error) == std::filesystem::weakly_canonical(b, error);
  }

  dap::Session& session_;
  pmc::Program program_;
  pmc::Machine machine_;
  std::string path_;
  std::set<int> breakpoints_;  // addresses
  bool stop_on_entry_ = true;
};

// stdin for the session. cppdap's own file reader still reports itself open
// after the end of input, so its receive loop would spin forever once the
// client is gone; this one reports the end, and the session closes.
class Stdin : public dap::Reader {
 public:
  bool isOpen() override { return open_; }
  void close() override { open_ = false; }
  size_t read(void* buffer, size_t bytes) override {
    size_t got = std::fread(buffer, 1, bytes, stdin);
    if (got < bytes) open_ = false;
    return got;
  }

 private:
  std::atomic<bool> open_{true};
};

}  // namespace pmc_dap

int main() {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::mutex mutex;
  std::condition_variable condition;
  bool done = false;
  auto quit = [&] {
    std::lock_guard<std::mutex> lock(mutex);
    done = true;
    condition.notify_all();
  };

  auto session = dap::Session::create();
  pmc_dap::Debugger debugger(*session);
  // cppdap reports problems such as a request without a handler here; they
  // are not fatal. nvim-dap writes the adapter's stderr to its log.
  session->onError([](const char* message) { std::fprintf(stderr, "pmc-dap: %s\n", message); });

  session->registerHandler([](const dap::InitializeRequest&) {
    dap::InitializeResponse response;
    response.supportsConfigurationDoneRequest = true;
    return response;
  });
  session->registerHandler([&](const pmc_dap::LaunchRequest& request) { return debugger.launch(request); });
  // Breakpoints come after "initialized", so after the program is loaded.
  session->registerSentHandler([&](const dap::ResponseOrError<dap::LaunchResponse>& response) {
    if (!response.error) session->send(dap::InitializedEvent());
  });
  session->registerHandler([&](const dap::SetBreakpointsRequest& request) {
    return debugger.set_breakpoints(request);
  });
  session->registerHandler([](const dap::SetExceptionBreakpointsRequest&) {
    return dap::SetExceptionBreakpointsResponse();
  });
  session->registerHandler([](const dap::ConfigurationDoneRequest&) {
    return dap::ConfigurationDoneResponse();
  });
  session->registerSentHandler([&](const dap::ResponseOrError<dap::ConfigurationDoneResponse>&) {
    debugger.begin();
  });
  session->registerHandler([](const dap::ThreadsRequest&) {
    dap::Thread thread;
    thread.id = 1;
    thread.name = "PMC";
    dap::ThreadsResponse response;
    response.threads.push_back(thread);
    return response;
  });
  session->registerHandler([&](const dap::StackTraceRequest&) { return debugger.stack_trace(); });
  session->registerHandler([](const dap::ScopesRequest&) { return pmc_dap::Debugger::scopes(); });
  session->registerHandler([&](const dap::VariablesRequest& request) {
    return debugger.variables(request.variablesReference);
  });
  session->registerHandler([&](const dap::EvaluateRequest& request) {
    return debugger.evaluate(request.expression);
  });

  // Running and stepping start after the response is sent, so their stopped
  // events come after it.
  session->registerHandler([](const dap::ContinueRequest&) { return dap::ContinueResponse(); });
  session->registerSentHandler([&](const dap::ResponseOrError<dap::ContinueResponse>&) { debugger.run(); });
  session->registerHandler([](const dap::NextRequest&) { return dap::NextResponse(); });
  session->registerSentHandler([&](const dap::ResponseOrError<dap::NextResponse>&) { debugger.step(); });
  session->registerHandler([](const dap::StepInRequest&) { return dap::StepInResponse(); });
  session->registerSentHandler([&](const dap::ResponseOrError<dap::StepInResponse>&) { debugger.step(); });
  session->registerHandler([](const dap::StepOutRequest&) { return dap::StepOutResponse(); });
  session->registerSentHandler([&](const dap::ResponseOrError<dap::StepOutResponse>&) { debugger.step(); });
  // The program never runs between requests, so it is always paused already.
  session->registerHandler([](const dap::PauseRequest&) { return dap::PauseResponse(); });

  session->registerHandler([](const dap::DisconnectRequest&) { return dap::DisconnectResponse(); });
  session->registerSentHandler([&](const dap::ResponseOrError<dap::DisconnectResponse>&) { quit(); });

  session->bind(std::make_shared<pmc_dap::Stdin>(), dap::file(stdout, false), quit);  // quit at the end of input

  std::unique_lock<std::mutex> lock(mutex);
  condition.wait(lock, [&] { return done; });
  // Exit at once: the session's reader thread may still be blocked on stdin.
  std::fflush(stdout);
  std::_Exit(0);
}
