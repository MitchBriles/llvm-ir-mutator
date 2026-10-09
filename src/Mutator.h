//===-- Mutator.h - Pathway registry and mutation driver -------*- C++ -*-===//

#pragma once

#include "Strategies.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/FuzzMutate/IRMutator.h"
#include "llvm/FuzzMutate/RandomIRBuilder.h"

#include <memory>
#include <string>
#include <vector>

namespace llvm {
class Module;
}

namespace mutator {

/// How many times a pathway was chosen, and what came of it. The distinction
/// between "chosen" and "applied" is what makes each pathway observable: a
/// pathway that is selected but never changes the module is reported, not
/// silently counted as working.
struct PathwayStats {
  uint64_t Chosen = 0;
  uint64_t Applied = 0; ///< changed the module and still verified
  uint64_t NoOp = 0;    ///< declined: module was byte-identical afterwards
  uint64_t Invalid = 0; ///< changed the module but failed the verifier
};

struct Options {
  uint64_t Seed = 0;
  unsigned Mutations = 0; ///< 0 means uniform in [Min,Max]MutationsPerMutant
  bool RollbackPerStep = true; ///< false: verify once per mutant instead
  bool BaselineOps = false;    ///< restrict `inject` to LLVM's default op set
  bool Alive2Safe = false;     ///< emit only what Alive2 can reason about
  bool LifterTypes = false;    ///< no vectors, no FP but half/float/double
  size_t MaxSize = 0;          ///< 0 means seed size + config::SizeHeadroom
  std::vector<std::string> Only;
  std::vector<std::string> Disable;
};

/// Owns the strategy registry and one long-lived RandomIRBuilder, so the RNG
/// advances across mutations instead of being reseeded on every one the way
/// llvm::IRMutator::mutateModule does.
class Mutator {
public:
  Mutator(const llvm::Module &Seed, const Options &Opts);
  ~Mutator();

  /// Produce one mutant: a fresh clone of the seed with \p Mutations successful
  /// mutations applied. Never returns null; a mutant with no applicable
  /// mutation is simply a copy of the seed.
  std::unique_ptr<llvm::Module> mutate(uint64_t MutantSeed,
                                       std::vector<llvm::StringRef> &Applied);

  llvm::ArrayRef<std::string> names() const { return Names; }
  llvm::ArrayRef<PathwayStats> stats() const { return Stats; }

private:
  const llvm::Module &Seed;
  Options Opts;
  ConstantPool Pool;
  llvm::RandomEngine Rand;
  std::unique_ptr<llvm::RandomIRBuilder> IB;

  std::vector<std::string> Names;
  std::vector<uint64_t> Weights;
  std::vector<std::unique_ptr<llvm::IRMutationStrategy>> Strategies;
  std::vector<PathwayStats> Stats;
  size_t MaxSize = 0;
  bool SeedUnliftable = false;

  /// Index of a pathway chosen by weight, or Names.size() if none is eligible.
  size_t sample(size_t CurSize);
};

} // namespace mutator
