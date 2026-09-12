cmake_minimum_required(VERSION 3.20)
project(llvm-ir-mutator C CXX)

# Point this at an LLVM build or install root, e.g.
#   cmake -S . -B build -G Ninja -DLLVM_PATH=/path/to/llvm-project/build
set(LLVM_PATH "" CACHE PATH "Root of an LLVM build or install tree")
if(LLVM_PATH)
  set(LLVM_DIR "${LLVM_PATH}/lib/cmake/llvm" CACHE PATH "Directory holding LLVMConfig.cmake" FORCE)
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})
include(AddLLVM)

set(CMAKE_CXX_STANDARD ${LLVM_CXX_STD})
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Mirror LLVM's own RTTI/EH settings; mismatching them produces link errors.
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()
if(NOT LLVM_ENABLE_EH)
  add_compile_options(-fno-exceptions)
endif()

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${LLVM_DEFINITIONS_LIST})

set(LLVM_LINK_COMPONENTS
  analysis asmparser bitwriter core fuzzmutate irreader
  scalaropts support transformutils)

add_llvm_executable(llvm-ir-mutator
  PARTIAL_SOURCES_INTENDED
  src/main.cpp
  src/Mutator.cpp
  src/Ops.cpp
  src/Strategies.cpp)
