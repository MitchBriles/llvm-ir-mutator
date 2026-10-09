//===-- Strategies.cpp - Mutation pathways beyond FuzzMutate -------------===//
//
// Each class here is an llvm::IRMutationStrategy, so it composes with the
// upstream strategies and inherits their sampling: overriding
// mutate(Instruction&) is enough to be handed a uniformly chosen instruction. A
// strategy that cannot act on what it is given simply returns, and the driver
// resamples.
//
//===----------------------------------------------------------------------===//

#include "Strategies.h"
#include "Config.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/FuzzMutate/RandomIRBuilder.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GEPNoWrapFlags.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <functional>

using namespace llvm;

namespace {

//===----------------------------------------------------------------------===//
// Small random helpers, all drawing from the mutator's own engine.
//===----------------------------------------------------------------------===//

unsigned pick(RandomEngine &R, unsigned N) {
  return std::uniform_int_distribution<unsigned>(0, N - 1)(R);
}
bool coin(RandomEngine &R) { return pick(R, 2) != 0; }

template <typename T> T pickOne(RandomEngine &R, ArrayRef<T> Xs) {
  return Xs[pick(R, Xs.size())];
}

/// Apply one uniformly chosen action, if any are available. Every strategy
/// funnels through this so "collect what is legal, then do exactly one thing"
/// is the single shape in this file.
void applyOne(RandomEngine &R, ArrayRef<std::function<void()>> Actions) {
  if (!Actions.empty())
    Actions[pick(R, Actions.size())]();
}

/// A uniformly random integer of \p Width bits. std::mt19937 yields 32 bits at
/// a time, so wide types have to be filled word by word -- drawing once would
/// leave everything above bit 32 zero.
APInt randomAPInt(RandomEngine &R, unsigned Width) {
  SmallVector<uint64_t, 4> Words((Width + 63) / 64);
  for (uint64_t &W : Words)
    W = (static_cast<uint64_t>(R()) << 32) | R();
  return APInt(Width, Words);
}

/// A uniformly random constant of \p Ty, every bit of the representation drawn
/// independently. For floats that covers denormals, both infinities and NaNs
/// with arbitrary payloads, which are otherwise only reachable as special
/// cases. Returns null for a type with no such interpretation.
Constant *randomConstant(RandomEngine &R, Type *Ty) {
  if (auto *VT = dyn_cast<FixedVectorType>(Ty)) {
    SmallVector<Constant *, 16> Lanes;
    for (unsigned I = 0, E = VT->getNumElements(); I != E; ++I) {
      Constant *Lane = randomConstant(R, VT->getElementType());
      if (!Lane)
        return nullptr;
      Lanes.push_back(Lane);
    }
    return ConstantVector::get(Lanes);
  }
  if (auto *IT = dyn_cast<IntegerType>(Ty))
    return ConstantInt::get(IT, randomAPInt(R, IT->getBitWidth()));
  if (Ty->isFloatingPointTy()) {
    const fltSemantics &Sem = Ty->getFltSemantics();
    return ConstantFP::get(
        Ty, APFloat(Sem, randomAPInt(R, APFloat::getSizeInBits(Sem))));
  }
  return nullptr;
}

/// Base for strategies whose weighting is owned by the driver's config table.
class Pathway : public IRMutationStrategy {
public:
  uint64_t getWeight(size_t, size_t, uint64_t) override { return 1; }
};

//===----------------------------------------------------------------------===//
// flags -- the poison-generating and semantic flags InstModificationIRStrategy
// does not reach: disjoint, nneg, samesign, trunc nuw/nsw, GEP nusw/nuw, the
// is_zero_poison immediates, and load/store volatility and alignment.
//===----------------------------------------------------------------------===//

class FlagsStrategy : public Pathway {
public:
  using IRMutationStrategy::mutate;
  void mutate(Instruction &I, RandomIRBuilder &IB) override {
    std::vector<std::function<void()>> Mods;

    if (auto *TI = dyn_cast<TruncInst>(&I)) {
      Mods.push_back(
          [TI] { TI->setHasNoUnsignedWrap(!TI->hasNoUnsignedWrap()); });
      Mods.push_back([TI] { TI->setHasNoSignedWrap(!TI->hasNoSignedWrap()); });
    }
    if (auto *PDI = dyn_cast<PossiblyDisjointInst>(&I))
      Mods.push_back([PDI] { PDI->setIsDisjoint(!PDI->isDisjoint()); });
    if (auto *PNI = dyn_cast<PossiblyNonNegInst>(&I))
      Mods.push_back([PNI] { PNI->setNonNeg(!PNI->hasNonNeg()); });
    if (auto *Cmp = dyn_cast<ICmpInst>(&I))
      Mods.push_back([Cmp] { Cmp->setSameSign(!Cmp->hasSameSign()); });

    if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
      Mods.push_back([GEP] { GEP->setNoWrapFlags(GEPNoWrapFlags::none()); });
      Mods.push_back(
          [GEP] { GEP->setNoWrapFlags(GEPNoWrapFlags::inBounds()); });
      Mods.push_back([GEP] {
        GEP->setNoWrapFlags(GEPNoWrapFlags::noUnsignedSignedWrap());
      });
      Mods.push_back(
          [GEP] { GEP->setNoWrapFlags(GEPNoWrapFlags::noUnsignedWrap()); });
    }

    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      Mods.push_back([LI] { LI->setVolatile(!LI->isVolatile()); });
      Mods.push_back([LI] { LI->setAlignment(Align(1)); });
    }
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      Mods.push_back([SI] { SI->setVolatile(!SI->isVolatile()); });
      Mods.push_back([SI] { SI->setAlignment(Align(1)); });
    }

    // The trailing i1 immediate of ctlz/cttz/abs is a flag in all but name.
    if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
      switch (II->getIntrinsicID()) {
      case Intrinsic::ctlz:
      case Intrinsic::cttz:
      case Intrinsic::abs:
        Mods.push_back([II] {
          auto *C = cast<ConstantInt>(II->getArgOperand(1));
          II->setArgOperand(1, ConstantInt::get(C->getType(), !C->isOne()));
        });
        break;
      default:
        break;
      }
    }

    if (isa<FPMathOperator>(&I)) {
      Mods.push_back([&I] { I.setHasNoInfs(!I.hasNoInfs()); });
      Mods.push_back([&I] { I.setHasNoNaNs(!I.hasNoNaNs()); });
      Mods.push_back([&I] { I.setHasNoSignedZeros(!I.hasNoSignedZeros()); });
      Mods.push_back([&I] { I.setHasAllowContract(!I.hasAllowContract()); });
    }

    if (auto *CB = dyn_cast<CallBase>(&I))
      if (!CB->getType()->isVoidTy())
        Mods.push_back([CB] {
          if (CB->hasRetAttr(Attribute::NoUndef))
            CB->removeRetAttr(Attribute::NoUndef);
          else
            CB->addRetAttr(Attribute::NoUndef);
        });

    applyOne(IB.Rand, Mods);
  }
};

//===----------------------------------------------------------------------===//
// const -- perturb constant operands toward values that trip folds.
//===----------------------------------------------------------------------===//

class ConstStrategy : public Pathway {
  const mutator::ConstantPool &Pool;

  /// Interesting replacements for an integer constant of \p Ty.
  Constant *newInt(RandomEngine &R, const APInt &C, Type *Ty) {
    unsigned W = C.getBitWidth();
    switch (pick(R, 8)) {
    case 0:
      return ConstantInt::get(Ty, 0);
    case 1:
      return ConstantInt::get(Ty, 1);
    case 2:
      return ConstantInt::get(Ty, APInt::getAllOnes(W));
    case 3:
      return ConstantInt::get(Ty, APInt::getSignedMinValue(W));
    case 4:
      return ConstantInt::get(Ty, APInt::getSignedMaxValue(W));
    case 5:
      return ConstantInt::get(Ty, -C);
    case 6:
      return ConstantInt::get(Ty, ~C);
    default:
      return ConstantInt::get(Ty, randomAPInt(R, W));
    }
  }

  Constant *newFloat(RandomEngine &R, const APFloat &C, Type *Ty) {
    const fltSemantics &S = C.getSemantics();
    APFloat V = C;
    switch (pick(R, 7)) {
    case 0:
      V.changeSign();
      break;
    case 1:
      V.next(/*nextDown=*/false);
      break;
    case 2:
      V.next(/*nextDown=*/true);
      break;
    case 3:
      V = APFloat::getInf(S, coin(R));
      break;
    case 4:
      V = APFloat::getNaN(S, coin(R));
      break;
    case 5:
      V = APFloat(S, randomAPInt(R, APFloat::getSizeInBits(S)));
      break;
    default:
      V = APFloat::getZero(S, coin(R));
      break;
    }
    return ConstantFP::get(Ty, V);
  }

public:
  explicit ConstStrategy(const mutator::ConstantPool &P) : Pool(P) {}

  using IRMutationStrategy::mutate;
  void mutate(Instruction &I, RandomIRBuilder &IB) override {
    // Indices and masks in these carry structural meaning; changing them is
    // either meaningless or immediately invalid.
    if (isa<PHINode>(I) || isa<SwitchInst>(I) || isa<GetElementPtrInst>(I) ||
        isa<ShuffleVectorInst>(I) || isa<InsertValueInst>(I) ||
        isa<ExtractValueInst>(I))
      return;

    auto *CB = dyn_cast<CallBase>(&I);
    std::vector<std::function<void()>> Mods;

    for (Use &U : I.operands()) {
      unsigned No = U.getOperandNo();
      if (!isa<Constant>(U.get()))
        continue;
      // Intrinsic immediates have their own validity rules; leave them to the
      // `flags` pathway, which knows each one.
      if (CB && No < CB->arg_size() && CB->paramHasAttr(No, Attribute::ImmArg))
        continue;

      Type *Ty = U->getType();

      // A shift amount at or above the bit width is poison, which is legal but
      // collapses the whole expression, so keep it in range.
      if (I.isShift() && No == 1) {
        unsigned W = Ty->getScalarSizeInBits();
        Mods.push_back([&U, Ty, W, &IB] {
          U.set(ConstantInt::get(Ty, pick(IB.Rand, W)));
        });
        continue;
      }
      if (isa<ExtractElementInst>(I) && No == 1) {
        auto *VT = cast<FixedVectorType>(I.getOperand(0)->getType());
        unsigned N = VT->getNumElements();
        Mods.push_back([&U, Ty, N, &IB] {
          U.set(ConstantInt::get(Ty, pick(IB.Rand, N)));
        });
        continue;
      }

      if (const auto *CI = dyn_cast<ConstantInt>(U.get()))
        Mods.push_back([this, &U, Ty, CI, &IB] {
          U.set(newInt(IB.Rand, CI->getValue(), Ty));
        });
      else if (const auto *CF = dyn_cast<ConstantFP>(U.get()))
        Mods.push_back([this, &U, Ty, CF, &IB] {
          U.set(newFloat(IB.Rand, CF->getValueAPF(), Ty));
        });

      // A wholly random value of the operand's own type. This is also the
      // only arm that reaches vector constants, which match neither
      // ConstantInt nor ConstantFP and so fall through everything above.
      if (Ty->isIntOrIntVectorTy() || Ty->isFPOrFPVectorTy())
        Mods.push_back([&U, Ty, &IB] {
          if (Constant *C = randomConstant(IB.Rand, Ty))
            U.set(C);
        });

      // Constants the seed itself mentions fold far more often than random
      // ones, so offer a same-typed value from the harvested pool too.
      for (Constant *C : Pool)
        if (C->getType() == Ty)
          Mods.push_back([&U, C] { U.set(C); });
    }

    applyOne(IB.Rand, Mods);
  }
};

//===----------------------------------------------------------------------===//
// opcode -- swap an opcode for a sibling in its type-compatible class, and
// clear the flags that only made sense for the old one.
//===----------------------------------------------------------------------===//

class OpcodeStrategy : public Pathway {
  /// Opcodes that share a signature, so any one can replace any other.
  static ArrayRef<ArrayRef<unsigned>> binaryClasses() {
    static const unsigned AddSubMul[] = {Instruction::Add, Instruction::Sub,
                                         Instruction::Mul};
    static const unsigned Logic[] = {Instruction::And, Instruction::Or,
                                     Instruction::Xor};
    static const unsigned Shifts[] = {Instruction::Shl, Instruction::LShr,
                                      Instruction::AShr};
    static const unsigned Div[] = {Instruction::UDiv, Instruction::SDiv,
                                   Instruction::URem, Instruction::SRem};
    static const unsigned FP[] = {Instruction::FAdd, Instruction::FSub,
                                  Instruction::FMul, Instruction::FDiv,
                                  Instruction::FRem};
    static const ArrayRef<unsigned> Classes[] = {AddSubMul, Logic, Shifts, Div,
                                                 FP};
    return Classes;
  }

  static ArrayRef<ArrayRef<Intrinsic::ID>> intrinsicClasses() {
    static const Intrinsic::ID MinMax[] = {Intrinsic::smin, Intrinsic::smax,
                                           Intrinsic::umin, Intrinsic::umax};
    static const Intrinsic::ID Sat[] = {
        Intrinsic::sadd_sat, Intrinsic::uadd_sat, Intrinsic::ssub_sat,
        Intrinsic::usub_sat};
    static const Intrinsic::ID FMinMax[] = {
        Intrinsic::minnum, Intrinsic::maxnum, Intrinsic::minimum,
        Intrinsic::maximum};
    static const Intrinsic::ID Funnel[] = {Intrinsic::fshl, Intrinsic::fshr};
    static const Intrinsic::ID CountZeros[] = {Intrinsic::ctlz,
                                               Intrinsic::cttz};
    static const ArrayRef<Intrinsic::ID> Classes[] = {MinMax, Sat, FMinMax,
                                                      Funnel, CountZeros};
    return Classes;
  }

  static ArrayRef<ArrayRef<unsigned>> castClasses() {
    static const unsigned Ext[] = {Instruction::ZExt, Instruction::SExt};
    static const unsigned IntToFP[] = {Instruction::SIToFP,
                                       Instruction::UIToFP};
    static const unsigned FPToInt[] = {Instruction::FPToSI,
                                       Instruction::FPToUI};
    static const ArrayRef<unsigned> Classes[] = {Ext, IntToFP, FPToInt};
    return Classes;
  }

public:
  using IRMutationStrategy::mutate;
  void mutate(Instruction &I, RandomIRBuilder &IB) override {
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      for (ArrayRef<unsigned> Class : binaryClasses()) {
        if (!is_contained(Class, BO->getOpcode()))
          continue;
        SmallVector<unsigned, 5> Others;
        for (unsigned Op : Class)
          if (Op != BO->getOpcode())
            Others.push_back(Op);
        // Building a fresh instruction rather than rewriting the opcode in
        // place is what drops the old flags: an `nsw` that was meaningful on
        // `add` must not survive onto `and`.
        auto *New = BinaryOperator::Create(
            static_cast<Instruction::BinaryOps>(
                pickOne<unsigned>(IB.Rand, Others)),
            BO->getOperand(0), BO->getOperand(1), "B", BO->getIterator());
        BO->replaceAllUsesWith(New);
        BO->eraseFromParent();
        return;
      }
      return;
    }

    if (auto *Cmp = dyn_cast<CmpInst>(&I)) {
      bool IsInt = isa<ICmpInst>(Cmp);
      unsigned First =
          IsInt ? CmpInst::FIRST_ICMP_PREDICATE : CmpInst::FIRST_FCMP_PREDICATE;
      unsigned Last =
          IsInt ? CmpInst::LAST_ICMP_PREDICATE : CmpInst::LAST_FCMP_PREDICATE;
      SmallVector<unsigned, 16> Others;
      for (unsigned P = First; P <= Last; ++P)
        if (P != Cmp->getPredicate())
          Others.push_back(P);
      Cmp->setPredicate(
          static_cast<CmpInst::Predicate>(pickOne<unsigned>(IB.Rand, Others)));
      return;
    }

    if (auto *CI = dyn_cast<CastInst>(&I)) {
      for (ArrayRef<unsigned> Class : castClasses()) {
        if (!is_contained(Class, CI->getOpcode()))
          continue;
        SmallVector<unsigned, 2> Others;
        for (unsigned Op : Class)
          if (Op != CI->getOpcode())
            Others.push_back(Op);
        auto *New = CastInst::Create(static_cast<Instruction::CastOps>(
                                         pickOne<unsigned>(IB.Rand, Others)),
                                     CI->getOperand(0), CI->getType(), "C",
                                     CI->getIterator());
        CI->replaceAllUsesWith(New);
        CI->eraseFromParent();
        return;
      }
      return;
    }

    if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
      for (ArrayRef<Intrinsic::ID> Class : intrinsicClasses()) {
        if (!is_contained(Class, II->getIntrinsicID()))
          continue;
        SmallVector<Intrinsic::ID, 4> Others;
        for (Intrinsic::ID ID : Class)
          if (ID != II->getIntrinsicID())
            Others.push_back(ID);
        II->setCalledFunction(Intrinsic::getOrInsertDeclaration(
            II->getModule(), pickOne<Intrinsic::ID>(IB.Rand, Others),
            {II->getType()}));
        return;
      }
    }
  }
};

//===----------------------------------------------------------------------===//
// int-width -- rebuild a chain of integer arithmetic at a different bit width,
// adapting at the boundaries. No other pathway changes the types of existing
// computation.
//===----------------------------------------------------------------------===//

class IntWidthStrategy : public Pathway {
  static bool resizable(const Instruction *I) {
    return isa<BinaryOperator>(I) && I->getType()->isIntegerTy() &&
           I->getType()->getScalarSizeInBits() > 1;
  }

public:
  using IRMutationStrategy::mutate;
  void mutate(Instruction &I, RandomIRBuilder &IB) override {
    if (!resizable(&I))
      return;

    // Follow single-use arithmetic downstream to form a chain.
    SmallVector<Instruction *, 8> Chain{&I};
    while (Chain.size() < mutator::config::MaxIntChainLength) {
      Instruction *Last = Chain.back();
      if (!Last->hasOneUse())
        break;
      auto *User = dyn_cast<Instruction>(*Last->user_begin());
      if (!User || !resizable(User) || User->getType() != Last->getType() ||
          User->getParent() != Last->getParent())
        break;
      Chain.push_back(User);
    }

    IntegerType *OldTy = cast<IntegerType>(I.getType());
    unsigned OldW = OldTy->getBitWidth();
    unsigned NewW;
    if (coin(IB.Rand))
      NewW = pickOne<unsigned>(IB.Rand, mutator::config::CommonWidths);
    else
      NewW = pick(IB.Rand, mutator::config::MaxRandomWidth) + 1;
    if (NewW == OldW)
      return;
    auto *NewTy = IntegerType::get(I.getContext(), NewW);

    // Rebuild the chain at NewW, mapping each old node to its new counterpart
    // and adapting anything that comes from outside the chain.
    DenseMap<Value *, Value *> Resized;
    IRBuilder<> B(&I);
    auto adapt = [&](Value *V) -> Value * {
      if (Value *R = Resized.lookup(V))
        return R;
      return B.CreateZExtOrTrunc(V, NewTy, "rw");
    };

    Instruction *Tail = Chain.back();
    for (Instruction *Node : Chain) {
      B.SetInsertPoint(Node);
      auto *BO = cast<BinaryOperator>(Node);
      Value *New = B.CreateBinOp(BO->getOpcode(), adapt(BO->getOperand(0)),
                                 adapt(BO->getOperand(1)), "rw");
      Resized[Node] = New;
    }

    B.SetInsertPoint(Tail->getNextNode());
    Value *Back = B.CreateZExtOrTrunc(Resized[Tail], OldTy, "rw");
    Tail->replaceAllUsesWith(Back);
  }
};

//===----------------------------------------------------------------------===//
// break-use -- give a single-use value a second user, so every fold guarded by
// hasOneUse() stops firing.
//
// The extra user is built by RandomIRBuilder::connectToSink, the same machinery
// the `sink` pathway uses, so it is ordinary IR -- a store, or an operand
// replacement in a later instruction -- rather than a marker symbol. What this
// pathway adds over `sink` is the targeting: `sink` samples a value uniformly,
// whereas a one-use fold is only reachable by picking a value that has exactly
// one use to begin with.
//===----------------------------------------------------------------------===//

class BreakUseStrategy : public Pathway {
public:
  using IRMutationStrategy::mutate;
  void mutate(BasicBlock &BB, RandomIRBuilder &IB) override {
    SmallVector<Instruction *, 32> Insts;
    for (Instruction &I : BB)
      if (!isa<PHINode>(I) && !I.isTerminator() && !I.isEHPad())
        Insts.push_back(&I);

    // A sink has to come after its value to keep the block dominance-correct,
    // and connectToSink reads Insts.back(), so the last instruction has no
    // room and is not a candidate.
    SmallVector<size_t, 32> Candidates;
    for (size_t I = 0; I + 1 < Insts.size(); ++I) {
      Type *Ty = Insts[I]->getType();
      if (!Ty->isVoidTy() && !Ty->isTokenTy() && Insts[I]->hasOneUse())
        Candidates.push_back(I);
    }
    if (Candidates.empty())
      return;

    size_t Idx = Candidates[pick(IB.Rand, Candidates.size())];
    IB.connectToSink(BB, ArrayRef(Insts).slice(Idx + 1), Insts[Idx]);
  }
};

//===----------------------------------------------------------------------===//
// attrs -- toggle function, parameter and return attributes. These drive a
// large amount of optimizer behaviour and nothing else in the mutator touches
// them.
//===----------------------------------------------------------------------===//

class AttrsStrategy : public Pathway {
  bool AllowRange;

  static void toggle(Function &F, unsigned Idx, Attribute::AttrKind K) {
    if (F.getAttributes().hasAttributeAtIndex(Idx, K))
      F.removeAttributeAtIndex(Idx, K);
    else
      F.addAttributeAtIndex(Idx, Attribute::get(F.getContext(), K));
  }

public:
  explicit AttrsStrategy(bool AllowRange) : AllowRange(AllowRange) {}

  using IRMutationStrategy::mutate;
  void mutate(Function &F, RandomIRBuilder &IB) override {
    if (F.isDeclaration())
      return;

    std::vector<std::function<void()>> Mods;

    for (Attribute::AttrKind K :
         {Attribute::NoFree, Attribute::WillReturn, Attribute::NoRecurse,
          Attribute::NoSync, Attribute::MustProgress, Attribute::NoUnwind})
      Mods.push_back([&F, K] { toggle(F, AttributeList::FunctionIndex, K); });

    Type *RetTy = F.getReturnType();
    if (!RetTy->isVoidTy()) {
      Mods.push_back(
          [&F] { toggle(F, AttributeList::ReturnIndex, Attribute::NoUndef); });
      if (RetTy->isPointerTy())
        for (Attribute::AttrKind K : {Attribute::NonNull, Attribute::NoAlias})
          Mods.push_back([&F, K] { toggle(F, AttributeList::ReturnIndex, K); });
      if (RetTy->isIntegerTy() && RetTy->getIntegerBitWidth() < 32)
        for (Attribute::AttrKind K : {Attribute::ZExt, Attribute::SExt})
          Mods.push_back([&F, K] { toggle(F, AttributeList::ReturnIndex, K); });
    }

    for (Argument &A : F.args()) {
      Argument *Arg = &A;
      Mods.push_back([Arg] {
        if (Arg->hasAttribute(Attribute::NoUndef))
          Arg->removeAttr(Attribute::NoUndef);
        else
          Arg->addAttr(Attribute::NoUndef);
      });
      if (A.getType()->isPointerTy()) {
        for (Attribute::AttrKind K :
             {Attribute::NonNull, Attribute::NoAlias, Attribute::ReadOnly,
              Attribute::WriteOnly})
          Mods.push_back([Arg, K] {
            if (Arg->hasAttribute(K))
              Arg->removeAttr(K);
            else
              Arg->addAttr(K);
          });
        Mods.push_back([Arg, &IB] {
          Arg->addAttrs(AttrBuilder(Arg->getContext())
                            .addDereferenceableAttr(1ULL << pick(IB.Rand, 5)));
        });
        Mods.push_back([Arg, &IB] {
          Arg->addAttrs(AttrBuilder(Arg->getContext())
                            .addAlignmentAttr(1ULL << pick(IB.Rand, 5)));
        });
      }
      if (A.getType()->isIntegerTy() &&
          A.getType()->getIntegerBitWidth() < 32) {
        for (Attribute::AttrKind K : {Attribute::ZExt, Attribute::SExt})
          Mods.push_back([Arg, K] {
            if (Arg->hasAttribute(K))
              Arg->removeAttr(K);
            else
              Arg->addAttr(K);
          });
      }
      if (AllowRange && A.getType()->isIntegerTy() &&
          A.getType()->getIntegerBitWidth() > 1) {
        Mods.push_back([Arg, &IB] {
          unsigned W = Arg->getType()->getIntegerBitWidth();
          APInt Lo(W, pick(IB.Rand, 16)), Hi(W, pick(IB.Rand, 16) + 16);
          Arg->addAttrs(AttrBuilder(Arg->getContext())
                            .addRangeAttr(ConstantRange(Lo, Hi)));
        });
      }
    }

    applyOne(IB.Rand, Mods);
  }
};

//===----------------------------------------------------------------------===//
// wrap -- interpose freeze, fneg, fabs or a cast round-trip on an operand.
//===----------------------------------------------------------------------===//

class WrapStrategy : public Pathway {
public:
  using IRMutationStrategy::mutate;
  void mutate(Instruction &I, RandomIRBuilder &IB) override {
    // A PHI operand must be available in its incoming block, not here.
    if (isa<PHINode>(I) || I.isEHPad() || I.getNumOperands() == 0)
      return;

    SmallVector<Use *, 8> Candidates;
    for (Use &U : I.operands())
      if (!U->getType()->isVoidTy() && !U->getType()->isLabelTy() &&
          !U->getType()->isMetadataTy() && !U->getType()->isTokenTy() &&
          !isa<Function>(U.get()) && !isa<BasicBlock>(U.get()))
        Candidates.push_back(&U);
    if (Candidates.empty())
      return;
    if (auto *CB = dyn_cast<CallBase>(&I))
      if (CB->isInlineAsm())
        return;

    Use &U = *Candidates[pick(IB.Rand, Candidates.size())];
    // An immarg must stay a literal constant.
    if (auto *CB = dyn_cast<CallBase>(&I))
      if (U.getOperandNo() < CB->arg_size() &&
          CB->paramHasAttr(U.getOperandNo(), Attribute::ImmArg))
        return;

    Value *V = U.get();
    Type *Ty = V->getType();

    enum Kind { Freeze, FNeg, FAbs, RoundTrip };
    SmallVector<Kind, 4> Kinds{Freeze};
    if (Ty->isFPOrFPVectorTy()) {
      Kinds.push_back(FNeg);
      Kinds.push_back(FAbs);
    }
    if (Ty->isIntOrIntVectorTy() && Ty->getScalarSizeInBits() > 1)
      Kinds.push_back(RoundTrip);

    IRBuilder<> B(&I);
    switch (Kinds[pick(IB.Rand, Kinds.size())]) {
    case Freeze:
      U.set(B.CreateFreeze(V, "w"));
      break;
    case FNeg:
      U.set(B.CreateFNeg(V, "w"));
      break;
    case FAbs:
      U.set(B.CreateUnaryIntrinsic(Intrinsic::fabs, V, nullptr, "w"));
      break;
    case RoundTrip: {
      // A widen/narrow round-trip: value-preserving only when it fits, which is
      // exactly the kind of reasoning we want to stress.
      Type *Wide = Ty->getWithNewBitWidth(Ty->getScalarSizeInBits() * 2);
      U.set(B.CreateTrunc(B.CreateZExt(V, Wide, "w"), Ty, "w"));
      break;
    }
    }
  }
};

//===----------------------------------------------------------------------===//
// move -- relocate an instruction inside its block. The legal window is bounded
// by its last operand definition and its first use, so the result always
// verifies without any operand repair.
//===----------------------------------------------------------------------===//

class MoveStrategy : public Pathway {
public:
  using IRMutationStrategy::mutate;
  void mutate(Instruction &I, RandomIRBuilder &IB) override {
    if (isa<PHINode>(I) || I.isTerminator() || I.isEHPad())
      return;

    BasicBlock &BB = *I.getParent();
    SmallVector<Instruction *, 32> Insts;
    for (Instruction &Other : BB)
      Insts.push_back(&Other);

    auto indexOf = [&](const Instruction *X) {
      return static_cast<size_t>(find(Insts, X) - Insts.begin());
    };
    size_t Self = indexOf(&I);

    // Stay after every operand defined here, and after the leading PHIs.
    size_t Lo = 0;
    for (size_t J = 0; J < Insts.size(); ++J)
      if (isa<PHINode>(Insts[J]))
        Lo = J + 1;
    for (Value *Op : I.operands())
      if (auto *Def = dyn_cast<Instruction>(Op))
        if (Def->getParent() == &BB)
          Lo = std::max(Lo, indexOf(Def) + 1);

    // Stay before every use defined here, and before the terminator.
    size_t Hi = Insts.size() - 1;
    for (User *U : I.users())
      if (auto *Use = dyn_cast<Instruction>(U))
        if (Use->getParent() == &BB)
          Hi = std::min(Hi, indexOf(Use));

    if (Lo >= Hi)
      return;
    size_t Target = Lo + pick(IB.Rand, static_cast<unsigned>(Hi - Lo));
    if (Target == Self)
      return;
    I.moveBefore(Insts[Target]->getIterator());
  }
};

//===----------------------------------------------------------------------===//
// splice -- retarget a call at another function with the same signature, and
// sometimes inline it. This is the only pathway that moves code across function
// boundaries.
//===----------------------------------------------------------------------===//

class SpliceStrategy : public Pathway {
public:
  using IRMutationStrategy::mutate;
  void mutate(Function &F, RandomIRBuilder &IB) override {
    SmallVector<CallBase *, 8> Calls;
    for (Instruction &I : instructions(F))
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (CB->getCalledFunction() &&
            !CB->getCalledFunction()->isIntrinsic() && !CB->isInlineAsm() &&
            !isa<InvokeInst>(CB))
          Calls.push_back(CB);
    if (Calls.empty())
      return;

    CallBase *CB = Calls[pick(IB.Rand, Calls.size())];
    Function *Old = CB->getCalledFunction();

    SmallVector<Function *, 8> Compatible;
    for (Function &Cand : *F.getParent())
      if (&Cand != Old && !Cand.isIntrinsic() &&
          Cand.getFunctionType() == Old->getFunctionType())
        Compatible.push_back(&Cand);

    if (!Compatible.empty() && coin(IB.Rand)) {
      CB->setCalledFunction(Compatible[pick(IB.Rand, Compatible.size())]);
      return;
    }

    // Otherwise splice the callee's body in. Self-recursion would grow without
    // bound, so leave it alone.
    Function *Callee = CB->getCalledFunction();
    if (!Callee || Callee->isDeclaration() || Callee == &F)
      return;
    InlineFunctionInfo IFI;
    InlineFunction(*CB, IFI);
  }
};

} // namespace

namespace {

/// Erase a call, giving its uses poison. The call is gone rather than
/// rewritten because a scrub has no business inventing a replacement.
void eraseCall(CallInst *CI) {
  if (!CI->getType()->isVoidTy())
    CI->replaceAllUsesWith(PoisonValue::get(CI->getType()));
  CI->eraseFromParent();
}

/// Break every cycle in the module's direct call graph by erasing a call that
/// closes one. Alive2 cannot reason about a recursive call: it reports the
/// callee as "function did not return", and every refinement check downstream
/// of that is noise rather than a codegen result.
bool breakRecursion(Module &M) {
  bool Changed = false;
  // 0 = unvisited, 1 = on the current DFS path, 2 = done.
  DenseMap<Function *, unsigned> Color;

  std::function<void(Function &)> visit = [&](Function &F) {
    Color[&F] = 1;
    SmallVector<CallInst *, 8> Cycle;
    for (Instruction &I : instructions(F)) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      Function *Callee = CI->getCalledFunction();
      // An indirect call cannot be shown to close a cycle, and a declaration
      // has no body to recurse through.
      if (!Callee || Callee->isDeclaration())
        continue;
      unsigned C = Color.lookup(Callee);
      if (C == 1)
        Cycle.push_back(CI); // back edge: this call closes a cycle
      else if (C == 0)
        visit(*Callee);
    }
    for (CallInst *CI : Cycle) {
      eraseCall(CI);
      Changed = true;
    }
    Color[&F] = 2;
  };

  for (Function &F : M)
    if (!F.isDeclaration() && Color.lookup(&F) == 0)
      visit(F);
  return Changed;
}

} // namespace

bool mutator::scrubForAlive2(Module &M) {
  bool Changed = breakRecursion(M);

  // FuzzMutate invents a poison pointer when it needs one and has none. A load
  // or store through it is immediate UB, so Alive2 would discard the mutant as
  // "Source function is always UB".
  for (Function &F : M)
    for (Instruction &I : make_early_inc_range(instructions(F))) {
      Value *Ptr = getLoadStorePointerOperand(&I);
      if (!Ptr || !isa<UndefValue>(Ptr))
        continue;
      if (!I.getType()->isVoidTy())
        I.replaceAllUsesWith(PoisonValue::get(I.getType()));
      I.eraseFromParent();
      Changed = true;
    }

  auto dropNoAlias = [&](Function &F) {
    if (F.hasRetAttribute(Attribute::NoAlias)) {
      F.removeRetAttr(Attribute::NoAlias);
      Changed = true;
    }
    for (Argument &A : F.args())
      if (A.hasAttribute(Attribute::NoAlias)) {
        A.removeAttr(Attribute::NoAlias);
        Changed = true;
      }
  };

  for (Function &F : M) {
    dropNoAlias(F);
    for (Instruction &I : instructions(F)) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->isVolatile()) {
          LI->setVolatile(false);
          Changed = true;
        }
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        if (SI->isVolatile()) {
          SI->setVolatile(false);
          Changed = true;
        }
      }

      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (CB->hasRetAttr(Attribute::NoAlias)) {
          CB->removeRetAttr(Attribute::NoAlias);
          Changed = true;
        }
        for (unsigned A = 0, E = CB->arg_size(); A != E; ++A)
          if (CB->paramHasAttr(A, Attribute::NoAlias)) {
            CB->removeParamAttr(A, Attribute::NoAlias);
            Changed = true;
          }
      }

      if (isa<FPMathOperator>(&I)) {
        FastMathFlags F = I.getFastMathFlags();
        // Alive2 refuses any fast-math flag on an int-to-FP conversion.
        if ((isa<SIToFPInst>(I) || isa<UIToFPInst>(I)) && F.any()) {
          I.copyFastMathFlags(FastMathFlags());
          Changed = true;
        } else if (F.allowReassoc() || F.allowReciprocal() ||
                   F.allowContract() || F.approxFunc()) {
          I.setHasAllowReassoc(false);
          I.setHasAllowReciprocal(false);
          I.setHasAllowContract(false);
          I.setHasApproxFunc(false);
          Changed = true;
        }
      }
    }
  }
  return Changed;
}

/// ConstantInt and ConstantFP are uniqued per LLVMContext rather than owned by
/// a module, so constants harvested from the seed stay valid in every clone of
/// it. That is what lets the pool be gathered once, up front.
mutator::ConstantPool mutator::harvestConstants(const Module &M) {
  ConstantPool Pool;
  SmallPtrSet<Constant *, 32> Seen;
  for (const Function &F : M)
    for (const Instruction &I : instructions(F))
      for (const Use &U : I.operands())
        if (auto *C = dyn_cast<Constant>(U.get()))
          if ((isa<ConstantInt>(C) || isa<ConstantFP>(C)) &&
              Seen.insert(const_cast<Constant *>(C)).second)
            Pool.push_back(const_cast<Constant *>(C));
  return Pool;
}

std::unique_ptr<IRMutationStrategy>
mutator::createStrategy(StringRef Name, const ConstantPool &Pool,
                        bool Alive2Safe) {
  if (Name == "flags")
    return std::make_unique<FlagsStrategy>();
  if (Name == "const")
    return std::make_unique<ConstStrategy>(Pool);
  if (Name == "opcode")
    return std::make_unique<OpcodeStrategy>();
  if (Name == "int-width")
    return std::make_unique<IntWidthStrategy>();
  if (Name == "break-use")
    return std::make_unique<BreakUseStrategy>();
  if (Name == "attrs")
    return std::make_unique<AttrsStrategy>(/*AllowRange=*/!Alive2Safe);
  if (Name == "wrap")
    return std::make_unique<WrapStrategy>();
  if (Name == "move")
    return std::make_unique<MoveStrategy>();
  if (Name == "splice")
    return std::make_unique<SpliceStrategy>();
  return nullptr;
}
