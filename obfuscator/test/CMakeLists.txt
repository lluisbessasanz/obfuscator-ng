cmake_minimum_required(VERSION 3.20)
project(obfuscator_test C)

# ---- Paths you may need to adjust ----
set(LLVM_BUILD_DIR "../../../../llvm-project/build" CACHE PATH "LLVM build directory")
set(PLUGIN_PATH "../../build/ObfuscationPlugin.so" CACHE FILEPATH "Path to ObfuscationPlugin.so")
set(PASS_NAME "sub" CACHE STRING "Pass pipeline name")
set(SUB_LOOP "3" CACHE STRING "Number of substitution rounds")

# LLVM tools from your build tree
set(CLANG "${LLVM_BUILD_DIR}/bin/clang")
set(OPT   "${LLVM_BUILD_DIR}/bin/opt")

# Input/output files
set(TEST_C   "${CMAKE_CURRENT_SOURCE_DIR}/test.c")
set(TEST_LL  "${CMAKE_CURRENT_BINARY_DIR}/test.ll")
set(OUT_LL   "${CMAKE_CURRENT_BINARY_DIR}/out.ll")
set(OUT_EXE  "${CMAKE_CURRENT_BINARY_DIR}/obfuscated_test.out")

# Sanity checks
if(NOT EXISTS "${TEST_C}")
  message(FATAL_ERROR "Missing input file: ${TEST_C}")
endif()

if(NOT EXISTS "${CLANG}")
  message(FATAL_ERROR "clang not found: ${CLANG}")
endif()

if(NOT EXISTS "${OPT}")
  message(FATAL_ERROR "opt not found: ${OPT}")
endif()

if(NOT EXISTS "${PLUGIN_PATH}")
  message(FATAL_ERROR "Plugin not found: ${PLUGIN_PATH}")
endif()

# 1) C -> LLVM IR
add_custom_command(
  OUTPUT "${TEST_LL}"
  COMMAND "${CLANG}" -O0 -Xclang -disable-O0-optnone -emit-llvm -S
          "${TEST_C}" -o "${TEST_LL}"
  DEPENDS "${TEST_C}"
  COMMENT "Generating LLVM IR: test.ll"
  VERBATIM
)

# 2) LLVM IR -> obfuscated LLVM IR
add_custom_command(
  OUTPUT "${OUT_LL}"
  COMMAND "${OPT}"
          -load-pass-plugin "${PLUGIN_PATH}"
          -passes=${PASS_NAME}
          -sub_loop=${SUB_LOOP}
          -S "${TEST_LL}" -o "${OUT_LL}"
  DEPENDS "${TEST_LL}" "${PLUGIN_PATH}"
  COMMENT "Running obfuscation pass: ${PASS_NAME}"
  VERBATIM
)

# 3) Obfuscated LLVM IR -> native executable
add_custom_command(
  OUTPUT "${OUT_EXE}"
  COMMAND "${CLANG}" "${OUT_LL}" -o "${OUT_EXE}"
  DEPENDS "${OUT_LL}"
  COMMENT "Compiling obfuscated executable"
  VERBATIM
)

# Main target
add_custom_target(build_test ALL DEPENDS "${OUT_EXE}")

# Convenience target to run it
add_custom_target(run_test
  COMMAND "${OUT_EXE}"
  DEPENDS build_test
  COMMENT "Running obfuscated executable"
  VERBATIM
)

# Convenience target to diff IR before/after
add_custom_target(diff_ir
  COMMAND ${CMAKE_COMMAND} -E echo "=== diff test.ll out.ll ==="
  COMMAND diff -u "${TEST_LL}" "${OUT_LL}" || true
  DEPENDS "${OUT_LL}"
  VERBATIM
)
