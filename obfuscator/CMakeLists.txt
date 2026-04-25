cmake_minimum_required(VERSION 3.20)
project(ObfuscationPlugin LANGUAGES CXX)

find_package(LLVM REQUIRED CONFIG)
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")

include(AddLLVM)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_llvm_pass_plugin(ObfuscationPlugin
  Obfuscator-ng.cpp
  lib/SubstitutionPass.cpp
  lib/Flattening.cpp
  lib/SplitBasicBlocks.cpp
  lib/ApiHashing.cpp
  lib/Utils.cpp
)
