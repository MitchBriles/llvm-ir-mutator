//===-- Mutator.cpp - Pathway registry and mutation driver ---------------===//
//
// llvm::IRMutator applies exactly one strategy per call, reseeds its
// RandomIRBuilder every time, and reports nothing about what it did. This
// driver keeps the strategy interface but replaces that loop so that mutations
// accumulate under one RNG and every pathway's effect is measured.
//
//===----------------------------------------------------------------------===//

#include "Mutator.h"
#include "Config.h"
#include "Ops.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;
using namespace mutator;

namespace {

/// Print a module to a string. Comparing two of these is the only fully general
/// way to answer "did that mutation change anything", which is the measurement
/// the whole stats table rests on.
std::string render(const Module &M) {
  std::string S;
  raw_string_ostream OS(S);
  M.print(OS, nullptr);
  return S;
}

bool listed(ArrayRef<std::string> L, StringRef Name) {
  return is_contained(L, Name);
}

/// Upstream ends a musttail block's insertion range at the musttail call, so
/// picking that call hands connectToSink an empty range, and its store
/// fallback then reads back() of it.
class SinkStrategy : public SinkInstructionStrategy {
public:
  using SinkInstructionStrategy::mutate;
  void mutate(BasicBlock &BB, RandomIRBuilder &IB) override {
    if (!BB.getTerminatingMustTailCall())
      SinkInstructionStrategy::mutate(BB, IB);
  }
};

/// The types backend-tv's RISC-V lifter accepts. Its AArch64 lifter also takes
/// short vectors, which this rejects.
bool liftableType(Type *T) {
  if (T->isVectorTy())
    return false;
  if (T->isFloatingPointTy())
    return T->isHalfTy() || T->isFloatTy() || T->isDoubleTy();
  if (auto *ST = dyn_cast<StructType>(T))
    return all_of(ST->elements(), liftableType);
  if (auto *AT = dyn_cast<ArrayType>(T))
    return liftableType(AT->getElementType());
  return true;
}

bool usesUnliftableType(const Module &M) {
  for (const Function &F : M) {
    FunctionType *FTy = F.getFunctionType();
    if (!liftableType(FTy->getReturnType()) ||
        !all_of(FTy->params(), liftableType))
      return true;
    for (const Instruction &I : instructions(F)) {
      if (!liftableType(I.getType()))
        return true;
      for (const Value *Op : I.operands())
        if (!liftableType(Op->getType()))
          return true;
    }
  }
  return false;
}

} // namespace

Mutator::Mutator(const Module &SeedModule, const Options &O)
    : Seed(SeedModule), Opts(O), Pool(harvestConstants(SeedModule)),
      Rand(static_cast<RandomEngine::result_type>(O.Seed)) {
  auto Types = config::typePool(Seed.getContext());
  // Alive2 has no model for scalable vectors, so they cannot appear at all --
  // not even as a type the builder might invent a value of.
  if (Opts.Alive2Safe)
    llvm::erase_if(Types, [](Type *T) {
      auto *VT = dyn_cast<VectorType>(T);
      return VT && VT->getElementCount().isScalable();
    });
  if (Opts.LifterTypes) {
    llvm::erase_if(Types, [](Type *T) { return !liftableType(T); });
    SeedUnliftable = usesUnliftableType(Seed);
  }
  assert(!Types.empty() && "config::typePool must not be empty");
  IB = std::make_unique<RandomIRBuilder>(static_cast<int>(O.Seed), Types);
  IB->MinArgNum = config::MinArgNum;
  IB->MaxArgNum = config::MaxArgNum;
  IB->MinFunctionNum = config::MinFunctionNum;

  MaxSize = Opts.MaxSize
                ? Opts.MaxSize
                : IRMutator::getModuleSize(Seed) + config::SizeHeadroom;

  for (const auto &Spec : config::strategies()) {
    StringRef Name = Spec.Name;
    if (!Opts.Only.empty() && !listed(Opts.Only, Name))
      continue;
    if (listed(Opts.Disable, Name))
      continue;

    std::unique_ptr<IRMutationStrategy> S;
    if (Name == "inject")
      S = std::make_unique<InjectorIRStrategy>(Opts.BaselineOps ? upstreamOps()
                                                                : allOps(Rand));
    else if (Name == "delete")
      S = std::make_unique<InstDeleterIRStrategy>();
    else if (Name == "modify")
      S = std::make_unique<InstModificationIRStrategy>();
    else if (Name == "insert-call")
      S = std::make_unique<InsertFunctionStrategy>();
    else if (Name == "insert-cfg")
      S = std::make_unique<InsertCFGStrategy>();
    else if (Name == "insert-phi")
      S = std::make_unique<InsertPHIStrategy>();
    else if (Name == "sink")
      S = std::make_unique<SinkStrategy>();
    else if (Name == "shuffle")
      S = std::make_unique<ShuffleBlockStrategy>();
    else
      S = createStrategy(Name, Pool, Opts.Alive2Safe);
    assert(S && "config::strategies() names a pathway nothing constructs");

    Names.emplace_back(Name);
    // --only forces a pathway on even if its configured weight is zero, so a
    // pathway can be disabled by default yet still be individually testable.
    Weights.push_back(Opts.Only.empty() ? Spec.Weight
                                        : std::max<uint64_t>(Spec.Weight, 1));
    Strategies.push_back(std::move(S));
  }
  Stats.resize(Names.size());
}

Mutator::~Mutator() = default;

size_t Mutator::sample(size_t CurSize) {
  // Past the size budget, only shrinking pathways stay eligible. This is the
  // one thing keeping a long run from growing without bound.
  bool Shrinking = CurSize > MaxSize;

  uint64_t Total = 0;
  for (size_t I = 0; I < Names.size(); ++I)
    if (!Shrinking || Names[I] == "delete")
      Total += Weights[I];
  if (Total == 0)
    return Names.size();

  uint64_t Roll = std::uniform_int_distribution<uint64_t>(0, Total - 1)(Rand);
  for (size_t I = 0; I < Names.size(); ++I) {
    if (Shrinking && Names[I] != "delete")
      continue;
    if (Roll < Weights[I])
      return I;
    Roll -= Weights[I];
  }
  return Names.size();
}

std::unique_ptr<Module> Mutator::mutate(uint64_t MutantSeed,
                                        std::vector<StringRef> &Applied) {
  Applied.clear();
  // Each mutant starts from the pristine seed, so mutants are independent and
  // the corpus cannot drift into one ever-growing module.
  Rand.seed(static_cast<RandomEngine::result_type>(MutantSeed));
  IB->Rand.seed(static_cast<RandomEngine::result_type>(MutantSeed));

  unsigned Want = Opts.Mutations;
  if (Want == 0)
    Want = config::MinMutationsPerMutant +
           std::uniform_int_distribution<unsigned>(
               0, config::MaxMutationsPerMutant -
                      config::MinMutationsPerMutant)(Rand);

  std::unique_ptr<Module> Cur = CloneModule(Seed);
  std::string CurText = render(*Cur);

  // Bounded: at most MaxAttemptsPerMutation resamples per requested mutation.
  // Strategies decline most instructions they are offered, so resampling is
  // normal, but it can never run forever.
  const uint64_t Budget =
      static_cast<uint64_t>(Want) * config::MaxAttemptsPerMutation;

  for (uint64_t Attempt = 0; Attempt < Budget && Applied.size() < Want;
       ++Attempt) {
    size_t Idx = sample(IRMutator::getModuleSize(*Cur));
    if (Idx == Names.size())
      break;
    Stats[Idx].Chosen++;

    std::unique_ptr<Module> Prev;
    if (Opts.RollbackPerStep)
      Prev = CloneModule(*Cur);

    Strategies[Idx]->mutate(*Cur, *IB);
    // Scrubbing before the change check means a mutation whose only effect was
    // an Alive2-hostile construct reads as a no-op and is resampled, rather
    // than being counted as a mutation that did nothing.
    if (Opts.Alive2Safe)
      scrubForAlive2(*Cur);

    if (Opts.RollbackPerStep &&
        (verifyModule(*Cur, nullptr) ||
         (Opts.LifterTypes && !SeedUnliftable && usesUnliftableType(*Cur)))) {
      Stats[Idx].Invalid++;
      Cur = std::move(Prev);
      continue;
    }
    
    std::string NewText = render(*Cur);
    if (NewText == CurText) {
      Stats[Idx].NoOp++;
      if (Prev)
        Cur = std::move(Prev);
      continue;
    }

    Stats[Idx].Applied++;
    Applied.push_back(Names[Idx]);
    CurText = std::move(NewText);
  }

  // Without per-step rollback nothing has checked validity yet, so do it once
  // and fall back to the untouched seed rather than emit a broken mutant.
  if (!Opts.RollbackPerStep &&
      (verifyModule(*Cur, nullptr) ||
       (Opts.LifterTypes && !SeedUnliftable && usesUnliftableType(*Cur)))) {
    for (StringRef Name : Applied)
      Stats[find(Names, Name) - Names.begin()].Invalid++;
    Applied.clear();
    Cur = CloneModule(Seed);
  }
  return Cur;
}
