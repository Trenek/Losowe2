cmake_minimum_required(VERSION 3.16...3.31)
project(libpmc LANGUAGES CXX)

# libpmc: PMC programs and the PMC machine, with no dependencies.
add_library(pmc STATIC src/pmc.cpp)
add_library(pmc::pmc ALIAS pmc)
target_include_directories(pmc PUBLIC include)
target_compile_features(pmc PUBLIC cxx_std_17)
