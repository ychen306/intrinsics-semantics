#include "Solver.h"
#include "Heuristic.h"
#include "Packer.h"
#include "Plan.h"
#include "VectorPackSet.h"

using namespace llvm;

template <typename AccessType>
VectorPack *createMemPack(VectorPackContext *VPCtx,
                          ArrayRef<AccessType *> Accesses,
                          const BitVector &Elements, const BitVector &Depended,
                          TargetTransformInfo *TTI);

template <>
VectorPack *createMemPack(VectorPackContext *VPCtx,
                          ArrayRef<StoreInst *> Stores,
                          const BitVector &Elements, const BitVector &Depended,
                          TargetTransformInfo *TTI) {
  return VPCtx->createStorePack(Stores, Elements, Depended, TTI);
}

template <>
VectorPack *createMemPack(VectorPackContext *VPCtx, ArrayRef<LoadInst *> Loads,
                          const BitVector &Elements, const BitVector &Depended,
                          TargetTransformInfo *TTI) {
  return VPCtx->createLoadPack(Loads, Elements, Depended, TTI);
}

template <typename AccessType>
std::vector<VectorPack *> getSeedMemPacks(Packer *Pkr, BasicBlock *BB,
                                          AccessType *Access, unsigned VL) {
  auto &LDA = Pkr->getLDA(BB);
  auto *VPCtx = Pkr->getContext(BB);
  auto *TTI = Pkr->getTTI();
  bool IsStore = std::is_same<AccessType, StoreInst>::value;
  auto &AccessDAG = IsStore ? Pkr->getStoreDAG(BB) : Pkr->getLoadDAG(BB);

  std::vector<VectorPack *> Seeds;

  std::function<void(std::vector<AccessType *>, BitVector, BitVector)>
      Enumerate = [&](std::vector<AccessType *> Accesses, BitVector Elements,
                      BitVector Depended) {
        if (Accesses.size() == VL) {
          Seeds.push_back(createMemPack<AccessType>(VPCtx, Accesses, Elements,
                                                    Depended, TTI));
          return;
        }

        auto It = AccessDAG.find(Accesses.back());
        if (It == AccessDAG.end()) {
          return;
        }
        for (auto *Next : It->second) {
          auto *NextAccess = cast<AccessType>(Next);
          if (!checkIndependence(LDA, *VPCtx, NextAccess, Elements, Depended)) {
            continue;
          }
          auto AccessesExt = Accesses;
          auto ElementsExt = Elements;
          auto DependedExt = Depended;
          AccessesExt.push_back(NextAccess);
          ElementsExt.set(VPCtx->getScalarId(NextAccess));
          DependedExt |= LDA.getDepended(NextAccess);
          Enumerate(AccessesExt, ElementsExt, DependedExt);
        }
      };

  std::vector<AccessType *> Accesses{Access};
  BitVector Elements(VPCtx->getNumValues());
  BitVector Depended(VPCtx->getNumValues());

  Elements.set(VPCtx->getScalarId(Access));
  Depended |= LDA.getDepended(Access);

  Enumerate(Accesses, Elements, Depended);
  return Seeds;
}

std::vector<const VectorPack *> enumerate(BasicBlock *BB, Packer *Pkr) {
  auto &LDA = Pkr->getLDA(BB);
  auto *VPCtx = Pkr->getContext(BB);
  auto &LayoutInfo = Pkr->getStoreInfo(BB);


  std::vector<const VectorPack *> Packs;
  for (auto &I : *BB) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      for (unsigned VL : {2, 4, 8, 16 /*, 32, 64*/})
        for (auto *VP : getSeedMemPacks(Pkr, BB, LI, VL))
          Packs.push_back(VP);
    }
  }
  return Packs;
}

// Run the bottom-up heuristic starting from `OP`
void runBottomUpFromOperand(const OperandPack *OP, Plan &P,
                            const VectorPackContext *VPCtx, Heuristic &H,
                            bool OverridePacked = false) {
  std::vector<const OperandPack *> Worklist{OP};
  while (!Worklist.empty()) {
    assert(P.verifyCost());
    auto *OP = Worklist.back();
    Worklist.pop_back();

    // The packs we are adding
    SmallVector<const VectorPack *, 4> NewPacks = H.solve(OP).Packs;
    // Union of the values covered by this solution
    BitVector Elements(VPCtx->getNumValues());
    // The packs we are replacing
    SmallPtrSet<const VectorPack *, 4> OldPacks;

    for (const VectorPack *VP : NewPacks) {
      Elements |= VP->getElements();
      for (auto *V : VP->elementValues())
        if (auto *VP2 = P.getProducer(dyn_cast<Instruction>(V)))
          OldPacks.insert(VP2);
    }

    bool Feasible = true;
    if (!OverridePacked) {
      // We only consider this solution if
      // the values we are packing is a superset of the
      // values packed in the original plan
      unsigned N = Elements.count();
      for (auto *VP2 : OldPacks)
        if ((Elements |= VP2->getElements()).count() > N) {
          Feasible = false;
          break;
        }
    }
    if (!Feasible)
      continue;

    for (auto *VP2 : OldPacks)
      P.remove(VP2);
    for (const VectorPack *VP : NewPacks) {
      P.add(VP);
      ArrayRef<const OperandPack *> Operands = VP->getOperandPacks();
      Worklist.insert(Worklist.end(), Operands.begin(), Operands.end());
    }
  }
}

void improvePlan(Packer *Pkr, Plan &P, const CandidatePackSet *CandidateSet) {
  std::vector<const VectorPack *> Seeds;
  auto *BB = P.getBasicBlock();
  for (auto &I : *BB)
    if (auto *SI = dyn_cast<StoreInst>(&I))
      for (unsigned VL : {2, 4, 8, 16, 32})
        for (auto *VP : getSeedMemPacks(Pkr, BB, SI, VL))
          Seeds.push_back(VP);

  auto *VPCtx = Pkr->getContext(P.getBasicBlock());
  Heuristic H(Pkr, VPCtx, CandidateSet);

  auto Improve = [&](Plan P2, ArrayRef<const OperandPack *> OPs,
                     bool Override) -> bool {
    for (auto *OP : OPs)
      runBottomUpFromOperand(OP, P2, VPCtx, H, Override);
    if (P2.cost() < P.cost()) {
      P = P2;
      return true;
    }
    return false;
  };

  bool Optimized;
  do {
    errs() << "COST: " << P.cost() << '\n';
    Optimized = false;
    for (auto *VP : Seeds) {
      Plan P2 = P;
      for (auto *V : VP->elementValues())
        if (auto *VP2 = P2.getProducer(cast<Instruction>(V)))
          P2.remove(VP2);
      P2.add(VP);
      auto *OP = VP->getOperandPacks().front();
      auto *Odd = VPCtx->odd(OP);
      auto *Even = VPCtx->even(OP);
      auto *OO = VPCtx->odd(Odd);
      auto *OE = VPCtx->even(Odd);
      auto *EO = VPCtx->odd(Even);
      auto *EE = VPCtx->even(Even);
      if (Improve(P2, {OP}, false) || Improve(P2, {OP}, true) ||
          Improve(P2, {Even, Odd}, false) || Improve(P2, {Even, Odd}, true) ||
          Improve(P2, {OO, OE, EO, EE}, false) || Improve(P2, {OO, OE, EO, EE}, true)) {
        Optimized = true;
        break;
      }
    }
    if (Optimized)
      continue;
    for (auto I = P.operands_begin(), E = P.operands_end(); I != E; ++I) {
      const OperandPack *OP = I->first;
      Plan P2 = P;
      auto *Odd = VPCtx->odd(OP);
      auto *Even = VPCtx->even(OP);
      auto *OO = VPCtx->odd(Odd);
      auto *OE = VPCtx->even(Odd);
      auto *EO = VPCtx->odd(Even);
      auto *EE = VPCtx->even(Even);
      if (Improve(P2, {OP}, false) || Improve(P2, {OP}, true) ||
          Improve(P2, {Even, Odd}, false) || Improve(P2, {Even, Odd}, true) ||
          Improve(P2, {OO, OE, EO, EE}, false) || Improve(P2, {OO, OE, EO, EE}, true)) {
        Optimized = true;
        break;
      }
    }
    if (Optimized)
      continue;
    for (auto *VP : P) {
      for (auto *VP2 : P) {
        if (VP == VP2 ||
            VP2->getDepended().anyCommon(VP->getElements()) ||
            VP->getDepended().anyCommon(VP2->getElements()))
          continue;
        OperandPack Concat;
        for (auto *V : VP->getOrderedValues())
          Concat.push_back(V);
        for (auto *V : VP2->getOrderedValues())
          Concat.push_back(V);
        auto *OP = VPCtx->getCanonicalOperandPack(Concat);
        auto OPI = Pkr->getProducerInfo(VPCtx, OP);
        if (!OPI.Feasible)
          continue;
        Plan P2 = P;
        P2.remove(VP);
        P2.remove(VP2);
        if (Improve(P2, {OP}, false) || Improve(P2, {OP}, true)) {
          Optimized = true;
          break;
        }
      }
      if (Optimized) break;
    }
  } while (Optimized);

#ifndef NDEBUG
  Plan P2(Pkr, BB);
  for (auto *VP : P)
    P2.add(VP);
  if (P2.cost() != P.cost())
    errs() << "!!! " << P2.cost() << ", " << P.cost() << '\n';
  assert(P2.cost() == P.cost());
#endif

  for (auto I = P.operands_begin(), E = P.operands_end(); I != E; ++I) {
    const OperandPack *OP = I->first;
    bool Foo = false;
    for (auto *V : *OP) {
      auto *I = dyn_cast_or_null<Instruction>(V);
      if (I && !P.getProducer(I)) {
        Foo = true;
        break;
      }
    }
    if (!Foo)
      continue;
    errs() << "op without producer: " << *OP << "\n\t[";
    for (auto *V : *OP) {
      auto *I = dyn_cast_or_null<Instruction>(V);
      if (I && !P.getProducer(I)) {
        errs() << " 0";
      } else
        errs() << " 1";
    }
    errs() << "]\n";
  }
}

float optimizeBottomUp(VectorPackSet &Packs, Packer *Pkr, BasicBlock *BB) {
  CandidatePackSet CandidateSet;
  CandidateSet.Packs = enumerate(BB, Pkr);
  auto *VPCtx = Pkr->getContext(BB);
  CandidateSet.Inst2Packs.resize(VPCtx->getNumValues());
  for (auto *VP : CandidateSet.Packs)
    for (unsigned i : VP->getElements().set_bits())
      CandidateSet.Inst2Packs[i].push_back(VP);

  Plan P(Pkr, BB);
  improvePlan(Pkr, P, &CandidateSet);
  for (auto *VP : P) {
    Packs.tryAdd(VP);
  }
  return P.cost();
}
