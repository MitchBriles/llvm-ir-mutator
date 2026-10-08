//===-- Ops.cpp - Expanded fuzzerop operation table ----------------------===//

#include "Ops.h"
#include "Config.h"

#include "llvm/FuzzMutate/Operations.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

using namespace llvm;
using namespace llvm::fuzzerop;
using namespace mutator;

namespace {

unsigned pick(RandomEngine &R, unsigned N) {
  return std::uniform_int_distribution<unsigned>(0, N - 1)(R);
}

template <typename T> const T &pickOne(RandomEngine &R, ArrayRef<T> Xs) {
  return Xs[pick(R, Xs.size())];
}

/// Replace a (possibly vector) type's element type, keeping the lane count.
Type *withElement(Type *Ty, Type *NewElt) {
  if (auto *VT = dyn_cast<VectorType>(Ty))
    return VectorType::get(NewElt, VT->getElementCount());
  return NewElt;
}

IRBuilder<> builderAt(BasicBlock::iterator IP) {
  return IRBuilder<>(IP);
}

//===----------------------------------------------------------------------===//
// Casts
//
// A cast descriptor has to invent a destination type. It derives one from the
// source so the result is always legal, and returns nullptr when no legal
// destination exists -- the injector treats that as "nothing was built".
//===----------------------------------------------------------------------===//

OpDescriptor intCastDescriptor(unsigned W, Instruction::CastOps Op,
                               RandomEngine &R) {
  auto Build = [Op, &R](ArrayRef<Value *> Srcs,
                        BasicBlock::iterator IP) -> Value * {
    Type *SrcTy = Srcs[0]->getType();
    unsigned SrcBits = SrcTy->getScalarSizeInBits();
    SmallVector<unsigned, 8> Legal;
    for (unsigned Bits : config::CommonWidths)
      if (Op == Instruction::Trunc ? Bits < SrcBits : Bits > SrcBits)
        Legal.push_back(Bits);
    if (Legal.empty())
      return nullptr;
    Type *DstTy =
        withElement(SrcTy, IntegerType::get(SrcTy->getContext(),
                                            pickOne<unsigned>(R, Legal)));
    return builderAt(IP).CreateCast(Op, Srcs[0], DstTy, "C");
  };
  return {W, {anyIntOrVecIntType()}, Build};
}

OpDescriptor intToFPDescriptor(unsigned W, Instruction::CastOps Op,
                               RandomEngine &R) {
  auto Build = [Op, &R](ArrayRef<Value *> Srcs,
                        BasicBlock::iterator IP) -> Value * {
    LLVMContext &C = Srcs[0]->getContext();
    Type *Floats[] = {Type::getHalfTy(C), Type::getFloatTy(C),
                      Type::getDoubleTy(C), Type::getFP128Ty(C)};
    Type *DstTy = withElement(Srcs[0]->getType(), pickOne<Type *>(R, Floats));
    return builderAt(IP).CreateCast(Op, Srcs[0], DstTy, "C");
  };
  return {W, {anyIntOrVecIntType()}, Build};
}

OpDescriptor fpToIntDescriptor(unsigned W, Instruction::CastOps Op,
                               RandomEngine &R) {
  auto Build = [Op, &R](ArrayRef<Value *> Srcs,
                        BasicBlock::iterator IP) -> Value * {
    LLVMContext &C = Srcs[0]->getContext();
    Type *DstTy = withElement(
        Srcs[0]->getType(),
        IntegerType::get(C, pickOne<unsigned>(R, config::CommonWidths)));
    return builderAt(IP).CreateCast(Op, Srcs[0], DstTy, "C");
  };
  return {W, {anyFloatOrVecFloatType()}, Build};
}

OpDescriptor fpResizeDescriptor(unsigned W, Instruction::CastOps Op,
                                RandomEngine &R) {
  auto Build = [Op, &R](ArrayRef<Value *> Srcs,
                        BasicBlock::iterator IP) -> Value * {
    LLVMContext &C = Srcs[0]->getContext();
    Type *All[] = {Type::getHalfTy(C), Type::getFloatTy(C),
                   Type::getDoubleTy(C), Type::getFP128Ty(C)};
    unsigned SrcBits = Srcs[0]->getType()->getScalarSizeInBits();
    SmallVector<Type *, 4> Legal;
    for (Type *T : All) {
      unsigned Bits = T->getScalarSizeInBits();
      if (Op == Instruction::FPTrunc ? Bits < SrcBits : Bits > SrcBits)
        Legal.push_back(T);
    }
    if (Legal.empty())
      return nullptr;
    Type *DstTy = withElement(Srcs[0]->getType(), pickOne<Type *>(R, Legal));
    return builderAt(IP).CreateCast(Op, Srcs[0], DstTy, "C");
  };
  return {W, {anyFloatOrVecFloatType()}, Build};
}

OpDescriptor ptrToIntDescriptor(unsigned W, RandomEngine &R) {
  auto Build = [&R](ArrayRef<Value *> Srcs,
                    BasicBlock::iterator IP) -> Value * {
    Type *DstTy = IntegerType::get(Srcs[0]->getContext(),
                                   pickOne<unsigned>(R, config::CommonWidths));
    return builderAt(IP).CreatePtrToInt(Srcs[0], DstTy, "C");
  };
  return {W, {anyPtrType()}, Build};
}

OpDescriptor intToPtrDescriptor(unsigned W) {
  auto Build = [](ArrayRef<Value *> Srcs, BasicBlock::iterator IP) -> Value * {
    return builderAt(IP).CreateIntToPtr(
        Srcs[0], PointerType::get(Srcs[0]->getContext(), 0), "C");
  };
  return {W, {anyIntType()}, Build};
}

/// Bitcast is only legal between equally-sized types, so this pairs each
/// integer width with the float type of the same size and vice versa.
OpDescriptor bitcastDescriptor(unsigned W) {
  auto Build = [](ArrayRef<Value *> Srcs, BasicBlock::iterator IP) -> Value * {
    Type *SrcTy = Srcs[0]->getType();
    LLVMContext &C = SrcTy->getContext();
    unsigned Bits = SrcTy->getScalarSizeInBits();
    Type *Elt = nullptr;
    if (SrcTy->isIntOrIntVectorTy()) {
      if (Bits == 16)
        Elt = Type::getHalfTy(C);
      else if (Bits == 32)
        Elt = Type::getFloatTy(C);
      else if (Bits == 64)
        Elt = Type::getDoubleTy(C);
      else if (Bits == 128)
        Elt = Type::getFP128Ty(C);
    } else if (SrcTy->isFPOrFPVectorTy()) {
      Elt = IntegerType::get(C, Bits);
    }
    if (!Elt)
      return nullptr;
    return builderAt(IP).CreateBitCast(Srcs[0], withElement(SrcTy, Elt), "C");
  };
  return {W, {anyType()}, Build};
}

//===----------------------------------------------------------------------===//
// freeze and memory
//===----------------------------------------------------------------------===//

OpDescriptor freezeDescriptor(unsigned W) {
  auto Build = [](ArrayRef<Value *> Srcs, BasicBlock::iterator IP) -> Value * {
    return builderAt(IP).CreateFreeze(Srcs[0], "FR");
  };
  return {W, {anyType()}, Build};
}

OpDescriptor loadDescriptor(unsigned W, RandomEngine &R) {
  auto Build = [&R](ArrayRef<Value *> Srcs,
                    BasicBlock::iterator IP) -> Value * {
    auto Pool = config::typePool(Srcs[0]->getContext());
    Type *Ty = Pool[pick(R, Pool.size())];
    return builderAt(IP).CreateLoad(Ty, Srcs[0], "L");
  };
  return {W, {sizedPtrType()}, Build};
}

OpDescriptor storeDescriptor(unsigned W) {
  auto Build = [](ArrayRef<Value *> Srcs, BasicBlock::iterator IP) -> Value * {
    builderAt(IP).CreateStore(Srcs[0], Srcs[1]);
    return nullptr; // void: nothing to sink
  };
  return {W, {anyType(), sizedPtrType()}, Build};
}

//===----------------------------------------------------------------------===//
// Intrinsics
//
// Upstream FuzzMutate has no intrinsic descriptors at all, so this whole table
// is unreachable territory for it.
//===----------------------------------------------------------------------===//

/// ctpop, bitreverse and friends: one integer operand, result of the same type.
OpDescriptor intUnaryIntrinsic(unsigned W, Intrinsic::ID ID) {
  auto Build = [ID](ArrayRef<Value *> Srcs,
                    BasicBlock::iterator IP) -> Value * {
    // bswap is only defined for widths that are a non-zero multiple of 16.
    unsigned Bits = Srcs[0]->getType()->getScalarSizeInBits();
    if (ID == Intrinsic::bswap && (Bits < 16 || Bits % 16 != 0))
      return nullptr;
    return builderAt(IP).CreateUnaryIntrinsic(ID, Srcs[0], nullptr, "I");
  };
  return {W, {anyIntOrVecIntType()}, Build};
}

/// ctlz, cttz and abs take a trailing i1 immediate poison flag.
OpDescriptor intFlaggedIntrinsic(unsigned W, Intrinsic::ID ID,
                                 RandomEngine &R) {
  auto Build = [ID, &R](ArrayRef<Value *> Srcs,
                        BasicBlock::iterator IP) -> Value * {
    IRBuilder<> B = builderAt(IP);
    Value *Flag = B.getInt1(pick(R, 2) != 0);
    return B.CreateIntrinsic(ID, {Srcs[0]->getType()}, {Srcs[0], Flag}, nullptr,
                             "I");
  };
  return {W, {anyIntOrVecIntType()}, Build};
}

/// smin/smax/umin/umax and the saturating arithmetic family: two same-typed
/// integer operands.
OpDescriptor intBinaryIntrinsic(unsigned W, Intrinsic::ID ID) {
  auto Build = [ID](ArrayRef<Value *> Srcs,
                    BasicBlock::iterator IP) -> Value * {
    return builderAt(IP).CreateBinaryIntrinsic(ID, Srcs[0], Srcs[1], nullptr,
                                               "I");
  };
  return {W, {anyIntOrVecIntType(), matchFirstType()}, Build};
}

/// fshl and fshr: three same-typed integer operands.
OpDescriptor funnelShiftDescriptor(unsigned W, Intrinsic::ID ID) {
  auto Build = [ID](ArrayRef<Value *> Srcs,
                    BasicBlock::iterator IP) -> Value * {
    return builderAt(IP).CreateIntrinsic(
        ID, {Srcs[0]->getType()}, {Srcs[0], Srcs[1], Srcs[2]}, nullptr, "I");
  };
  return {W, {anyIntOrVecIntType(), matchFirstType(), matchFirstType()}, Build};
}

/// The with.overflow family, whose result is a {iN, i1} struct.
OpDescriptor overflowDescriptor(unsigned W, Intrinsic::ID ID) {
  auto Build = [ID](ArrayRef<Value *> Srcs,
                    BasicBlock::iterator IP) -> Value * {
    if (!Srcs[0]->getType()->isIntOrIntVectorTy())
      return nullptr;
    return builderAt(IP).CreateBinaryIntrinsic(ID, Srcs[0], Srcs[1], nullptr,
                                               "O");
  };
  return {W, {anyIntOrVecIntType(), matchFirstType()}, Build};
}

OpDescriptor fpUnaryIntrinsic(unsigned W, Intrinsic::ID ID) {
  auto Build = [ID](ArrayRef<Value *> Srcs,
                    BasicBlock::iterator IP) -> Value * {
    return builderAt(IP).CreateUnaryIntrinsic(ID, Srcs[0], nullptr, "I");
  };
  return {W, {anyFloatOrVecFloatType()}, Build};
}

OpDescriptor fpBinaryIntrinsic(unsigned W, Intrinsic::ID ID) {
  auto Build = [ID](ArrayRef<Value *> Srcs,
                    BasicBlock::iterator IP) -> Value * {
    return builderAt(IP).CreateBinaryIntrinsic(ID, Srcs[0], Srcs[1], nullptr,
                                               "I");
  };
  return {W, {anyFloatOrVecFloatType(), matchFirstType()}, Build};
}

OpDescriptor fmaDescriptor(unsigned W) {
  auto Build = [](ArrayRef<Value *> Srcs, BasicBlock::iterator IP) -> Value * {
    return builderAt(IP).CreateIntrinsic(Intrinsic::fma, {Srcs[0]->getType()},
                                         {Srcs[0], Srcs[1], Srcs[2]}, nullptr,
                                         "I");
  };
  return {
      W, {anyFloatOrVecFloatType(), matchFirstType(), matchFirstType()}, Build};
}

OpDescriptor isFPClassDescriptor(unsigned W, RandomEngine &R) {
  auto Build = [&R](ArrayRef<Value *> Srcs,
                    BasicBlock::iterator IP) -> Value * {
    IRBuilder<> B = builderAt(IP);
    // The test mask is an immediate; 0x3ff is "any class".
    return B.CreateIntrinsic(Intrinsic::is_fpclass, {Srcs[0]->getType()},
                             {Srcs[0], B.getInt32(pick(R, 0x3ff) + 1)}, nullptr,
                             "I");
  };
  return {W, {anyFloatOrVecFloatType()}, Build};
}

} // namespace

std::vector<OpDescriptor> mutator::upstreamOps() {
  std::vector<OpDescriptor> Ops;
  describeFuzzerIntOps(Ops);
  describeFuzzerFloatOps(Ops);
  describeFuzzerControlFlowOps(Ops);
  describeFuzzerPointerOps(Ops);
  describeFuzzerAggregateOps(Ops);
  describeFuzzerVectorOps(Ops);
  return Ops;
}

std::vector<OpDescriptor> mutator::allOps(RandomEngine &R) {
  std::vector<OpDescriptor> Ops = upstreamOps();

  // The two sets InjectorIRStrategy::getDefaultOps() never asks for.
  describeFuzzerUnaryOperations(Ops); // fneg
  describeFuzzerOtherOps(Ops);        // select

  // Casts.
  Ops.push_back(intCastDescriptor(2, Instruction::Trunc, R));
  Ops.push_back(intCastDescriptor(2, Instruction::ZExt, R));
  Ops.push_back(intCastDescriptor(2, Instruction::SExt, R));
  Ops.push_back(intToFPDescriptor(1, Instruction::SIToFP, R));
  Ops.push_back(intToFPDescriptor(1, Instruction::UIToFP, R));
  Ops.push_back(fpToIntDescriptor(1, Instruction::FPToSI, R));
  Ops.push_back(fpToIntDescriptor(1, Instruction::FPToUI, R));
  Ops.push_back(fpResizeDescriptor(1, Instruction::FPExt, R));
  Ops.push_back(fpResizeDescriptor(1, Instruction::FPTrunc, R));
  Ops.push_back(ptrToIntDescriptor(1, R));
  Ops.push_back(intToPtrDescriptor(1));
  Ops.push_back(bitcastDescriptor(1));

  // freeze and memory.
  Ops.push_back(freezeDescriptor(3));
  Ops.push_back(loadDescriptor(2, R));
  Ops.push_back(storeDescriptor(2));

  // Integer intrinsics.
  for (Intrinsic::ID ID :
       {Intrinsic::ctpop, Intrinsic::bswap, Intrinsic::bitreverse})
    Ops.push_back(intUnaryIntrinsic(1, ID));
  for (Intrinsic::ID ID : {Intrinsic::ctlz, Intrinsic::cttz, Intrinsic::abs})
    Ops.push_back(intFlaggedIntrinsic(1, ID, R));
  for (Intrinsic::ID ID :
       {Intrinsic::smin, Intrinsic::smax, Intrinsic::umin, Intrinsic::umax,
        Intrinsic::sadd_sat, Intrinsic::uadd_sat, Intrinsic::ssub_sat,
        Intrinsic::usub_sat})
    Ops.push_back(intBinaryIntrinsic(1, ID));
  for (Intrinsic::ID ID : {Intrinsic::fshl, Intrinsic::fshr})
    Ops.push_back(funnelShiftDescriptor(1, ID));
  for (Intrinsic::ID ID :
       {Intrinsic::sadd_with_overflow, Intrinsic::uadd_with_overflow,
        Intrinsic::ssub_with_overflow, Intrinsic::usub_with_overflow,
        Intrinsic::smul_with_overflow, Intrinsic::umul_with_overflow})
    Ops.push_back(overflowDescriptor(1, ID));

  // Floating-point intrinsics.
  for (Intrinsic::ID ID : {Intrinsic::fabs, Intrinsic::sqrt, Intrinsic::floor,
                           Intrinsic::ceil, Intrinsic::rint, Intrinsic::trunc})
    Ops.push_back(fpUnaryIntrinsic(1, ID));
  for (Intrinsic::ID ID :
       {Intrinsic::minnum, Intrinsic::maxnum, Intrinsic::minimum,
        Intrinsic::maximum, Intrinsic::copysign, Intrinsic::pow})
    Ops.push_back(fpBinaryIntrinsic(1, ID));
  Ops.push_back(fmaDescriptor(1));
  Ops.push_back(isFPClassDescriptor(1, R));

  return Ops;
}
