//===-- Strategies.h - Mutation pathways beyond FuzzMutate -----*- C++ -*-===//

#pragma once

#include "llvm/FuzzMutate/IRMutator.h"

#include <memory>
#include <vector>

namespace llvm {
class Constant;
class Module;
} // namespace llvm

namespace mutator {

/// Constants harvested from the seed module. Reusing values the program already
/// mentions hits far more folding patterns than uniform random constants do.
using ConstantPool = std::vector<llvm::Constant *>;
ConstantPool harvestConstants(const llvm::Module &M);

/// Remove the constructs Alive2 cannot reason about from \p M. Two kinds:
/// things it refuses to translate outright (volatile accesses, noalias), and
/// the fast-math flags it models as uninterpreted functions -- afn, arcp,
/// contract and reassoc -- which silently downgrade a real miscompile to
/// "couldn't prove", masking the bug. nnan, ninf and nsz are modelled exactly
/// and are kept.
///
/// This runs after a strategy, not inside one, so it also covers the upstream
/// strategies whose code we do not own.
///
/// \returns true if anything was removed.
bool scrubForAlive2(llvm::Module &M);

/// Build the named custom strategy, or nullptr if \p Name is not one of ours.
/// \p Pool is borrowed and must outlive the returned strategy.
std::unique_ptr<llvm::IRMutationStrategy>
createStrategy(llvm::StringRef Name, const ConstantPool &Pool);

} // namespace mutator
