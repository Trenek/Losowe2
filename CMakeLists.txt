cmake_minimum_required(VERSION 3.16...3.31)
project(pmc_dap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The PMC machine, a library of its own.
add_subdirectory(libpmc)

include(FetchContent)
# cppdap, Google's C++ library for the Debug Adapter Protocol, and the JSON
# library it uses (a git submodule there, which download archives leave out).
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_Declare(cppdap
  URL https://github.com/google/cppdap/archive/6464cd7bd0a5639b11902055ca607b53915833fc.tar.gz)
FetchContent_MakeAvailable(json)
set(CPPDAP_JSON_DIR "${json_SOURCE_DIR}")
FetchContent_MakeAvailable(cppdap)

add_executable(pmc-dap src/main.cpp)
target_link_libraries(pmc-dap PRIVATE pmc cppdap)
# build/pmc-dap, also with multi-config generators
set_target_properties(pmc-dap PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}$<0:>")
