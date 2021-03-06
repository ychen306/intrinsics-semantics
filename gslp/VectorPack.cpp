#include "VectorPack.h"
#include "MatchManager.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/VectorUtils.h"

using namespace llvm;

// FIXME: we need to generalize the definition of an operand pack
// because some of the input lanes are "DONT CARES" (e.g. _mm_div_pd)
std::vector<OperandPack> VectorPack::computeOperandPacksForGeneral() {
  auto &Sig = Producer->getSignature();
  unsigned NumInputs = Sig.numInputs();
  auto LaneOps = Producer->getLaneOps();
  unsigned NumLanes = LaneOps.size();
  std::vector<OperandPack> OperandPacks(NumInputs);

  struct BoundInput {
    InputSlice Slice;
    Value *V;
    // Order by offset of the slice
    bool operator<(const BoundInput &Other) const {
      return Slice < Other.Slice;
    }
  };

  // Figure out which input packs we need
  for (unsigned i = 0; i < NumInputs; i++) {
    std::vector<BoundInput> InputValues;
    // Size of one element in this input vector
    unsigned ElementSize = 0;
    // Find output lanes that uses input `i` and record those uses
    for (unsigned j = 0; j < NumLanes; j++) {
      ArrayRef<InputSlice> BoundSlices = LaneOps[j].getBoundSlices();
      for (unsigned k = 0; k < BoundSlices.size(); k++) {
        auto &BS = BoundSlices[k];
        if (BS.InputId != i)
          continue;
        ElementSize = BS.size();
        InputValues.push_back(
            {BS, Matches[j] ? Matches[j]->Inputs[k] : nullptr});
      }
    }
    assert(ElementSize != 0);

    // Sort the input values by their slice offset
    std::sort(InputValues.begin(), InputValues.end());

    unsigned CurOffset = 0;
    unsigned Stride = InputValues[0].Slice.size();
    auto &OP = OperandPacks[i];
    for (const BoundInput &BV : InputValues) {
      while (CurOffset < BV.Slice.Lo) {
        OP.push_back(nullptr);
        CurOffset += Stride;
      }
      assert(CurOffset == BV.Slice.Lo);
      OP.push_back(BV.V);
      CurOffset += Stride;
    }
    unsigned InputSize = Sig.InputBitwidths[i];
    while (CurOffset < InputSize) {
      OP.push_back(nullptr);
      CurOffset += Stride;
    }
    assert(OP.size() * Stride == InputSize);

    // Compute the type of don't care vector as special cases
    if (!OP.front() && is_splat(OP)) {
      auto *BB = VPCtx->getBasicBlock();
      OP.Ty = FixedVectorType::get(
          IntegerType::get(BB->getContext(), ElementSize), OP.size());
    }
  }

  return OperandPacks;
}

std::vector<OperandPack> VectorPack::computeOperandPacksForLoad() {
  // Only need the single *scalar* pointer, doesn't need packed operand
  return {};
}

std::vector<OperandPack> VectorPack::computeOperandPacksForStore() {
  std::vector<OperandPack> OperandPacks(1);
  auto &OP = OperandPacks[0];
  // Don't care about the pointers,
  // only the values being stored need to be packed first
  for (auto *S : Stores)
    OP.push_back(S->getValueOperand());
  return OperandPacks;
}

std::vector<OperandPack> VectorPack::computeOperandPacksForPhi() {
  auto *FirstPHI = PHIs[0];
  unsigned NumIncomings = FirstPHI->getNumIncomingValues();
  // We need as many packs as there are incoming edges
  std::vector<OperandPack> OperandPacks(NumIncomings);
  for (unsigned i = 0; i < NumIncomings; i++) {
    auto *BB = FirstPHI->getIncomingBlock(i);
    // all of the values coming from BB should be packed
    for (auto *PH : PHIs)
      OperandPacks[i].push_back(PH->getIncomingValueForBlock(BB));
  }
  return OperandPacks;
}

static Type *getScalarTy(ArrayRef<const Operation::Match *> Matches) {
  for (auto &M : Matches)
    if (M)
      return M->Output->getType();
  llvm_unreachable("Matches can't be all null");
  return nullptr;
}

Value *VectorPack::emitVectorGeneral(ArrayRef<Value *> Operands,
                                     IntrinsicBuilder &Builder) const {
  auto *VecInst = Producer->emit(Operands, Builder);
  // Fix the output type
  auto *VecType = FixedVectorType::get(getScalarTy(Matches), Matches.size());
  return Builder.CreateBitCast(VecInst, VecType);
}

// Shameless stolen from llvm's SLPVectorizer
Value *VectorPack::emitVectorLoad(ArrayRef<Value *> Operands,
                                  IntrinsicBuilder &Builder) const {
  auto *FirstLoad = Loads[0];
  auto &DL = FirstLoad->getParent()->getModule()->getDataLayout();
  auto *ScalarLoadTy = FirstLoad->getType();

  // Figure out type of the vector that we are loading
  auto *ScalarPtr = FirstLoad->getPointerOperand();
  auto *ScalarTy = cast<PointerType>(ScalarPtr->getType())->getElementType();
  auto *VecTy = FixedVectorType::get(ScalarTy, Loads.size());

  // Cast the scalar pointer to a vector pointer
  unsigned AS = FirstLoad->getPointerAddressSpace();
  Value *VecPtr = Builder.CreateBitCast(ScalarPtr, VecTy->getPointerTo(AS));

  // Emit the load
  auto *VecLoad =
      Builder.CreateAlignedLoad(VecTy, VecPtr, FirstLoad->getAlign());

  std::vector<Value *> Values;
  for (auto *LI : Loads)
    if (LI)
      Values.push_back(LI);
  return propagateMetadata(VecLoad, Values);
}

Value *VectorPack::emitVectorStore(ArrayRef<Value *> Operands,
                                   IntrinsicBuilder &Builder) const {
  auto *FirstStore = Stores[0];

  // This is the value we want to store
  Value *VecValue = Operands[0];

  // Figure out the store alignment
  unsigned Alignment = FirstStore->getAlignment();
  unsigned AS = FirstStore->getPointerAddressSpace();

  // Cast the scalar pointer to vector pointer
  assert(Operands.size() == 1);
  Value *ScalarPtr = FirstStore->getPointerOperand();
  Value *VecPtr =
      Builder.CreateBitCast(ScalarPtr, VecValue->getType()->getPointerTo(AS));

  // Emit the vector store
  StoreInst *VecStore = Builder.CreateStore(VecValue, VecPtr);

  // Fix the vector store alignment
  auto &DL = FirstStore->getParent()->getModule()->getDataLayout();
  if (!Alignment)
    Alignment =
        DL.getABITypeAlignment(FirstStore->getValueOperand()->getType());

  VecStore->setAlignment(Align(Alignment));
  std::vector<Value *> Stores_(Stores.begin(), Stores.end());
  return propagateMetadata(VecStore, Stores_);
}

Value *VectorPack::emitVectorPhi(ArrayRef<Value *> Operands,
                                 IntrinsicBuilder &Builder) const {
  auto *BB = VPCtx->getBasicBlock();
  Builder.SetInsertPoint(&*BB->begin());
  auto *FirstPHI = PHIs[0];
  unsigned NumIncomings = FirstPHI->getNumIncomingValues();

  auto *VecTy = FixedVectorType::get(FirstPHI->getType(), PHIs.size());
  auto *VecPHI = Builder.CreatePHI(VecTy, NumIncomings);

  std::set<BasicBlock *> Visited;
  // Values in operands follow the order of ::getUserPack,
  // which follows the basic block order of the first phi.
  for (unsigned i = 0; i < NumIncomings; i++) {
    auto *BB = FirstPHI->getIncomingBlock(i);
    // Apparently sometimes a phi node can have more than one
    // incoming value for the same basic block...
    if (Visited.count(BB)) {
      VecPHI->addIncoming(VecPHI->getIncomingValueForBlock(BB), BB);
      continue;
    }
    auto *VecIncoming = Operands[i];
    VecPHI->addIncoming(VecIncoming, BB);
    Visited.insert(BB);
  }
  assert(VecPHI->getNumIncomingValues() == FirstPHI->getNumIncomingValues());
  return VecPHI;
}

void VectorPack::computeOperandPacks() {
  switch (Kind) {
  case General:
    canonicalizeOperandPacks(computeOperandPacksForGeneral());
    break;
  case Load:
    canonicalizeOperandPacks(computeOperandPacksForLoad());
    break;
  case Store:
    canonicalizeOperandPacks(computeOperandPacksForStore());
    break;
  case Phi:
    canonicalizeOperandPacks(computeOperandPacksForPhi());
    break;
  }
}

Value *VectorPack::emit(ArrayRef<Value *> Operands,
                        IntrinsicBuilder &Builder) const {
  IRBuilderBase::InsertPointGuard Guard(Builder);

  // FIXME: choose insert point
  switch (Kind) {
  case General:
    return emitVectorGeneral(Operands, Builder);
  case Load:
    return emitVectorLoad(Operands, Builder);
  case Store:
    return emitVectorStore(Operands, Builder);
  case Phi:
    return emitVectorPhi(Operands, Builder);
  }
}

void VectorPack::computeCost(TargetTransformInfo *TTI) {
  Cost = 0;
  // 1) First figure out cost of the vector instruction
  switch (Kind) {
  case General:
    Cost = Producer->getCost(TTI, getBasicBlock()->getContext());
    break;
  case Load: {
    auto *LI = Loads[0];
    auto *VecTy = FixedVectorType::get(LI->getType(), Loads.size());
    Cost = TTI->getMemoryOpCost(Instruction::Load, VecTy, LI->getAlign(), 0,
                                TTI::TCK_RecipThroughput, LI);
    break;
  }
  case Store: {
    auto *SI = Stores[0];
    auto *VecTy =
        FixedVectorType::get(SI->getValueOperand()->getType(), Stores.size());
    Cost = TTI->getMemoryOpCost(Instruction::Store, VecTy, SI->getAlign(), 0,
                                TTI::TCK_RecipThroughput, SI);
    break;
  }
  case Phi:
    Cost = 0;
  }

  ProducingCost = Cost;
}

void VectorPack::computeOrderedValues() {
  switch (Kind) {
  case General:
    for (auto *M : Matches)
      if (M)
        OrderedValues.push_back(M->Output);
      else
        OrderedValues.push_back(nullptr);
    break;
  case Load:
    for (auto *LI : Loads)
      OrderedValues.push_back(LI);
    break;
  case Store:
    for (auto *SI : Stores)
      OrderedValues.push_back(SI);
    break;
  case Phi:
    for (auto *PHI : PHIs)
      OrderedValues.push_back(PHI);
    break;
  }
}

// Choose a right place to gather an operand
void VectorPack::setOperandGatherPoint(unsigned OperandId,
                                       IntrinsicBuilder &Builder) const {
  if (Kind != Phi) {
    auto *LeaderVal = *getOrderedValues().begin();
    Builder.SetInsertPoint(cast<Instruction>(LeaderVal));
  } else {
    // We need to gather the input before the execution gets to this block
    auto *FirstPHI = PHIs[0];
    auto *BB = FirstPHI->getIncomingBlock(OperandId);
    Builder.SetInsertPoint(BB->getTerminator());
  }
}

raw_ostream &operator<<(raw_ostream &OS, const VectorPack &VP) {
  StringRef ProducerName = "";
  if (auto *Producer = VP.getProducer())
    ProducerName = Producer->getName();
  OS << "PACK<" << ProducerName << ">: (\n";
  for (auto *V : VP.getOrderedValues())
    if (V)
      OS << *V << '\n';
    else
      OS << "undef\n";
  OS << ")\n";
  return OS;
}

raw_ostream &operator<<(raw_ostream &OS, const OperandPack &OP) {
  OS << "[\n";
  for (auto *V : OP)
    if (V) {
      errs() << *V << "\n";
    } else
      errs() << "undef\n";
  OS << "\n]";
  return OS;
}

FixedVectorType *getVectorType(const OperandPack &OP) {
  if (OP.Ty)
    return OP.Ty;
  Type *ScalarTy = nullptr;
  for (auto *V : OP)
    if (V) {
      ScalarTy = V->getType();
      break;
    }
  assert(ScalarTy && "Operand pack can't be all empty");
  return OP.Ty = FixedVectorType::get(ScalarTy, OP.size());
}

FixedVectorType *getVectorType(const VectorPack &VP) {
  unsigned NumLanes = VP.getElements().count();
  auto *FirstLane = *VP.elementValues().begin();
  return FixedVectorType::get(FirstLane->getType(), NumLanes);
}

bool isConstantPack(const OperandPack &OP) {
  for (auto *V : OP)
    if (V && !isa<Constant>(V))
      return false;
  return true;
}
