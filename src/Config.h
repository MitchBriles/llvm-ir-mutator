//===-- Config.h - All mutator tunables in one place -----------*- C++ -*-===//
//
// Everything meant to be adjusted by editing source lives here. Changing a
// weight, adding a type, or disabling a pathway should never require touching
// any other file.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Type.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mutator {
namespace config {

/// Names of every mutation pathway. These are the values accepted by --only and
/// --disable, and the row labels printed by --stats and --list.
///
/// The first eight wrap strategies from llvm/lib/FuzzMutate; the rest are
/// defined in Strategies.cpp.
struct StrategySpec {
  const char *Name;
  uint64_t Weight;
  const char *Doc;
};

/// Relative selection weights. A pathway with weight 0 is never chosen unless
/// named by --only, which forces its weight to 1.
inline const std::vector<StrategySpec> &strategies() {
  static const std::vector<StrategySpec> Specs = {
      // -- llvm/lib/FuzzMutate ------------------------------------------------
      {"inject", 30, "insert a random operation from the expanded op table"},
      {"delete", 8, "delete an instruction and repair its uses"},
      {"modify", 10, "flip instruction flags, predicates and operand order"},
      {"insert-call", 6, "insert a call to an existing or fresh function"},
      {"insert-cfg", 5, "split a block and weave in a random branch or switch"},
      {"insert-phi", 4, "add a phi node at the head of a block"},
      {"sink", 4, "add a new user to a value, deepening data dependence"},
      {"shuffle", 4, "reorder a block without violating data dependences"},
      // -- this project -------------------------------------------------------
      {"flags", 10, "toggle poison-generating flags FuzzMutate does not reach"},
      {"const", 12, "perturb constant operands toward interesting values"},
      {"opcode", 10, "swap an opcode within its type-compatible class"},
      {"int-width", 8, "rebuild an integer use chain at a different bit width"},
      {"break-use", 6, "add a second user to defeat one-use folds"},
      {"attrs", 6, "toggle function, parameter and return attributes"},
      {"wrap", 6, "wrap a use in freeze, fneg, fabs or a cast round-trip"},
      {"move", 5, "move an instruction to another legal slot in its block"},
      {"splice", 5,
       "retarget a call to another function with the same signature"},
  };
  return Specs;
}

/// Enable scalable vector types in the pool below. Note that --alive2-safe
/// filters these out regardless, since Alive2 has no model for them. Many passes and most
/// targets reject them outright, so this is off by default.
inline constexpr bool ScalableVectors = false;

/// Types the mutator may invent values of. RandomIRBuilder::randomType() draws
/// uniformly from this list, and it is also the universe a SourcePred's default
/// constant generator searches, so it must be non-empty and must contain at
/// least one integer, one float and one pointer type -- Ops.cpp asserts this.
///
/// Odd bit widths and non-power-of-two vector lengths are deliberate: they are
/// where legalization and InstCombine bugs concentrate.
inline std::vector<llvm::Type *> typePool(llvm::LLVMContext &C) {
  using namespace llvm;
  std::vector<Type *> T;
  for (unsigned W : {1u, 2u, 3u, 7u, 8u, 16u, 17u, 32u, 64u, 128u})
    T.push_back(IntegerType::get(C, W));
  T.push_back(Type::getHalfTy(C));
  T.push_back(Type::getBFloatTy(C));
  T.push_back(Type::getFloatTy(C));
  T.push_back(Type::getDoubleTy(C));
  T.push_back(Type::getFP128Ty(C));
  T.push_back(PointerType::get(C, 0));
  const struct {
    Type *Elt;
    unsigned Lanes;
  } Vecs[] = {
      {Type::getInt1Ty(C), 2},   {Type::getInt8Ty(C), 4},
      {Type::getInt32Ty(C), 2},  {Type::getInt32Ty(C), 3},
      {Type::getInt16Ty(C), 8},  {Type::getFloatTy(C), 4},
      {Type::getDoubleTy(C), 2},
  };
  for (const auto &V : Vecs)
    T.push_back(VectorType::get(V.Elt, V.Lanes, /*Scalable=*/false));

  // Scalable vectors reach a lot of unusual code, but many passes and most
  // targets reject them outright. Off by default.
  if (ScalableVectors) {
    T.push_back(VectorType::get(Type::getInt32Ty(C), 4, /*Scalable=*/true));
    T.push_back(VectorType::get(Type::getFloatTy(C), 2, /*Scalable=*/true));
  }
  return T;
}

/// Per-mutation retry budget. A strategy declines most instructions it is
/// offered, so we resample; this bounds that resampling. Every loop in the
/// mutator is bounded by a constant like this one.
inline constexpr unsigned MaxAttemptsPerMutation = 120;

/// Bounds for the default (--mutations=0) mutation count, drawn uniformly.
inline constexpr unsigned MinMutationsPerMutant = 1;
inline constexpr unsigned MaxMutationsPerMutant = 5;

/// How far a mutant may grow past the seed, in module objects (instructions +
/// blocks + globals + aliases). Feeds InstDeleterIRStrategy's weight ramp,
/// which is the only thing that pushes back on unbounded growth.
inline constexpr size_t SizeHeadroom = 512;

/// RandomIRBuilder knobs. FuzzMutate hardcodes these; exposing them widens the
/// shape of synthesized functions.
inline constexpr uint64_t MinArgNum = 0;
inline constexpr uint64_t MaxArgNum = 6;
inline constexpr uint64_t MinFunctionNum = 1;

/// Longest integer use chain the int-width pathway will rebuild, and the widths
/// it picks from (it also picks uniformly in [1, MaxRandomWidth] half the
/// time).
inline constexpr unsigned MaxIntChainLength = 8;
inline constexpr unsigned CommonWidths[] = {1, 8, 16, 32, 64, 128};
inline constexpr unsigned MaxRandomWidth = 128;

} // namespace config
} // namespace mutator
