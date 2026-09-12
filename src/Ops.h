//===-- Ops.h - Expanded fuzzerop operation table --------------*- C++ -*-===//

#pragma once

#include "llvm/FuzzMutate/OpDescriptor.h"
#include "llvm/FuzzMutate/RandomIRBuilder.h"

#include <vector>

namespace mutator {

/// Every operation the `inject` pathway may synthesize: LLVM's six default
/// sets, the two it leaves on the floor (select and fneg), plus casts, freeze,
/// memory and a broad intrinsic table that upstream has no descriptors for at
/// all.
///
/// Descriptors that must choose a destination type draw from \p Rand, which is
/// the mutator's own engine, so a given seed still reproduces exactly.
std::vector<llvm::fuzzerop::OpDescriptor> allOps(llvm::RandomEngine &Rand);

/// The set InjectorIRStrategy uses by default, for --baseline-ops comparisons.
std::vector<llvm::fuzzerop::OpDescriptor> upstreamOps();

} // namespace mutator
