#include "Solver.h"
#include "MatchManager.h"
#include "VectorPackSet.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<unsigned> MaxSearchDist(
    "max-search-dist",
    cl::value_desc(
        "Max distance with which we consider two instructions packable."),
    cl::init(20));

Frontier::Frontier(BasicBlock *BB, Packer *Pkr)
    : Pkr(Pkr), BB(BB), VPCtx(Pkr->getContext(BB)), BBIt(BB->rbegin()),
      UnresolvedScalars(VPCtx->getNumValues(), false),
      FreeInsts(VPCtx->getNumValues(), true),
      UsableInsts(VPCtx->getNumValues(), false) {
  // Find external uses of any instruction `I` in `BB`
  // and mark `I` as an unresolved scalar.
  for (auto &I : *BB) {
    bool AllUsersResolved = true;
    unsigned InstId = VPCtx->getScalarId(&I);
    for (User *U : I.users()) {
      auto UserInst = dyn_cast<Instruction>(U);
      if (UserInst) {
        if (UserInst->getParent() != BB)
          // Mark that `I` has a scalar use.
          UnresolvedScalars.set(InstId);
        else
          // `I` is used by some other instruction in `BB`
          AllUsersResolved = false;
      }
    }

    if (AllUsersResolved || isa<PHINode>(&I))
      UsableInsts.set(InstId);
  }
}

Instruction *Frontier::getNextFreeInst() const {
  if (BBIt != BB->rend())
    return &*BBIt;
  return nullptr;
}

void Frontier::freezeOneInst(Instruction *I) {
  unsigned InstId = VPCtx->getScalarId(I);
  assert(FreeInsts.test(InstId));
  FreeInsts.reset(InstId);
  UnresolvedScalars.reset(InstId);
  UsableInsts.reset(InstId);

  // See if freezing `I` makes any of its operands *usable*
  for (Value *Operand : I->operands()) {
    auto OI = dyn_cast<Instruction>(Operand);
    if (!OI || OI->getParent() != BB)
      continue;

    bool Usable = true;
    if (!isFree(OI))
      continue;

    // An instruction is usable if all of its users are frozen
    for (User *U : OI->users()) {
      auto *UserInst = dyn_cast<Instruction>(U);
      if (UserInst && UserInst->getParent() == BB && isFree(UserInst)) {
        Usable = false;
        break;
      }
    }
    if (Usable)
      UsableInsts.set(VPCtx->getScalarId(OI));
  }
}

void Frontier::advanceBBIt() {
  for (auto E = BB->rend(); BBIt != E; ++BBIt)
    if (FreeInsts.test(VPCtx->getScalarId(&*BBIt)))
      break;
}

bool Frontier::resolved(const OperandPack &OP) const {
  for (Value *V : OP) {
    if (!V)
      continue;
    auto *I = dyn_cast<Instruction>(V);
    if (!I || I->getParent() != BB)
      continue;
    if (FreeInsts[VPCtx->getScalarId(V)])
      return false;
  }
  return true;
}

float Frontier::advanceInplace(Instruction *I, TargetTransformInfo *TTI) {
  float Cost = 0;
  freezeOneInst(I);
  advanceBBIt();

  // Go over unresolved packs and see if we've resolved any lanes
  SmallVector<unsigned, 2> ResolvedPackIds;
  for (unsigned i = 0; i < UnresolvedPacks.size(); i++) {
    auto *OP = UnresolvedPacks[i];
    auto *VecTy = getVectorType(*OP);
    assert(VecTy->getNumElements() == OP->size());

    // Special case: we can build OP by broadcasting `I`.
    if (is_splat(*OP) && (*OP)[0] == I) {
      Cost += TTI->getShuffleCost(TargetTransformInfo::SK_Broadcast, VecTy, 0);
      ResolvedPackIds.push_back(i);
      continue;
    }

    // FIXME: Consider the case of *partial* resuse here.
    // E.g. If we have two operand packs (a,b) and (b,a) then we can
    // just explicitly pack (a,b) with insertion and get (b,a) with permutation.
    for (unsigned i = 0; i < OP->size(); i++) {
      // Pay the insert cost
      if ((*OP)[i] == I)
        Cost +=
            2*TTI->getVectorInstrCost(Instruction::InsertElement, VecTy, i);
    }
    if (resolved(*OP))
      ResolvedPackIds.push_back(i);
  }

  // If `I` uses any free instructions,
  // add those to the set of unresolved scalars.
  for (Value *Operand : I->operands()) {
    auto *I2 = dyn_cast<Instruction>(Operand);
    if (!I2 || I2->getParent() != BB)
      continue;
    unsigned InstId = VPCtx->getScalarId(I2);
    if (FreeInsts.test(InstId))
      UnresolvedScalars.set(InstId);
  }

  remove(UnresolvedPacks, ResolvedPackIds);
  std::sort(UnresolvedPacks.begin(), UnresolvedPacks.end());
  return Cost;
}

// Check whether there are lanes in `OpndPack` that are produced by `VP`.
// Also resolve such lanes.
bool Frontier::resolveOperandPack(const VectorPack &VP, const OperandPack &OP) {
  bool Produced = false;
  for (unsigned LaneId = 0; LaneId < OP.size(); LaneId++) {
    auto *V = OP[LaneId];
    if (!V)
      continue;
    auto *I = dyn_cast<Instruction>(V);
    if (!I || I->getParent() != BB)
      continue;
    if (VP.getElements().test(VPCtx->getScalarId(I))) {
      Produced = true;
    }
  }
  return Produced;
}

// Return the cost of gathering from `VP` to `OpndPack`
static unsigned getGatherCost(const VectorPack &VP, const OperandPack &OpndPack,
                              TargetTransformInfo *TTI) {
  if (isConstantPack(OpndPack))
    return 0;

  auto VPVals = VP.getOrderedValues();
  if (VPVals.size() == OpndPack.size()) {
    bool Exact = true;
    for (unsigned i = 0; i < VPVals.size(); i++)
      Exact &= (VPVals[i] == OpndPack[i]);

    // Best case:
    // If `VP` produces `OpndPack` exactly then we don't pay any thing
    if (Exact)
      return 0;

    // Second best case:
    // `VP` produces a permutation of `OpndPack`
    if (std::is_permutation(VPVals.begin(), VPVals.end(), OpndPack.begin()))
      return TTI->getShuffleCost(TargetTransformInfo::SK_PermuteSingleSrc,
                                 getVectorType(VP));
  }

  return 2;
}

// FIXME: this doesn't work when there are lanes in VP that cover multiple
// instructions.
float Frontier::advanceInplace(const VectorPack *VP, TargetTransformInfo *TTI) {
  float Cost = VP->getCost();
  Type *VecTy;
  // It doesn't make sense to get the value type of a store,
  // which returns nothing.
  if (!VP->isStore())
    VecTy = getVectorType(*VP);

  // Tick off instructions taking part in `VP` and pay the scalar extract cost.
  ArrayRef<Value *> OutputLanes = VP->getOrderedValues();
  for (unsigned LaneId = 0; LaneId < OutputLanes.size(); LaneId++) {
    if (!OutputLanes[LaneId])
      continue;
    auto *I = dyn_cast<Instruction>(OutputLanes[LaneId]);
    if (!I)
      continue;
    unsigned InstId = VPCtx->getScalarId(I);

    // Pay the extract cost
    if (UnresolvedScalars.test(InstId))
      Cost +=
          TTI->getVectorInstrCost(Instruction::ExtractElement, VecTy, LaneId);
  }

  // FIXME: instead of doing this, which is broken if some intermediate values have external user,
  // directly subtract cost of dead instructions. We have enough information to check if a value is dead.
  auto ReplacedInsts = VP->getReplacedInsts();
  std::sort(ReplacedInsts.begin(), ReplacedInsts.end(),
            [](Instruction *I, Instruction *J) { return J->comesBefore(I); });
  for (auto *I : ReplacedInsts)
    freezeOneInst(I);

  advanceBBIt();

  SmallVector<unsigned, 2> ResolvedPackIds;
  if (!VP->isStore()) {
    for (unsigned i = 0; i < UnresolvedPacks.size(); i++) {
      auto *OP = UnresolvedPacks[i];
      if (resolveOperandPack(*VP, *OP)) {
        Cost += getGatherCost(*VP, *OP, TTI);
        if (resolved(*OP))
          ResolvedPackIds.push_back(i);
      }
    }
  }

  // Track the unresolved operand packs used by `VP`
  for (auto *OpndPack : VP->getOperandPacks()) {
    auto *OperandTy = getVectorType(*OpndPack);
    for (unsigned LaneId = 0; LaneId < OpndPack->size(); LaneId++) {
      auto *V = (*OpndPack)[LaneId];
      if (!V)
        continue;
      if (isa<Constant>(V))
        continue;
      auto *I = dyn_cast<Instruction>(V);
      if (!I || I->getParent() != BB) {
        // Assume I is always scalar and pay the insert cost.
        Cost += 2* TTI->getVectorInstrCost(Instruction::InsertElement, OperandTy,
                                        LaneId);
      }
    }
    if (!resolved(*OpndPack) &&
        !std::binary_search(UnresolvedPacks.begin(), UnresolvedPacks.end(),
                            OpndPack))
      UnresolvedPacks.push_back(OpndPack);
  }

  remove(UnresolvedPacks, ResolvedPackIds);
  std::sort(UnresolvedPacks.begin(), UnresolvedPacks.end());
  return Cost;
}

float Frontier::advanceInplace(ShuffleTask ST, TargetTransformInfo *TTI) {
  auto It = std::lower_bound(UnresolvedPacks.begin(), UnresolvedPacks.end(), ST.Output);
  assert(It != UnresolvedPacks.end());
  assert(*It == ST.Output);
  std::swap(*It, UnresolvedPacks.back());
  UnresolvedPacks.pop_back();
  for (auto *OP : ST.Inputs)
    UnresolvedPacks.push_back(OP);
  std::sort(UnresolvedPacks.begin(), UnresolvedPacks.end());
  return ST.getCost(TTI);
}

std::unique_ptr<Frontier>
Frontier::advance(const VectorPack *VP, float &Cost,
                  llvm::TargetTransformInfo *TTI) const {
  auto Next = std::make_unique<Frontier>(*this);
  Cost = Next->advanceInplace(VP, TTI);
  return Next;
}

std::unique_ptr<Frontier>
Frontier::advance(llvm::Instruction *I, float &Cost,
                  llvm::TargetTransformInfo *TTI) const {
  auto Next = std::make_unique<Frontier>(*this);
  Cost = Next->advanceInplace(I, TTI);
  return Next;
}

std::unique_ptr<Frontier>
Frontier::advance(ShuffleTask ST, float &Cost, TargetTransformInfo *TTI) const {
  auto Next = std::make_unique<Frontier>(*this);
  Cost = Next->advanceInplace(ST, TTI);
  return Next;
}

PartialPack::PartialPack(bool IsLoad, bool IsStore, BasicBlock *BB,
                         unsigned NumLanes, Packer *Pkr)
    : IsLoad(IsLoad), IsStore(IsStore), BB(BB), VPCtx(Pkr->getContext(BB)),
      Elements(VPCtx->getNumValues()), Depended(VPCtx->getNumValues()),
      NumLanes(NumLanes), LaneId(0), Producer(nullptr),
      LoadDAG(Pkr->getLoadDAG(BB)), StoreDAG(Pkr->getStoreDAG(BB)),
      LDA(Pkr->getLDA(BB)), MM(Pkr->getMatchManager(BB)), TTI(Pkr->getTTI()) {}

PartialPack::PartialPack(const InstBinding *Inst, BasicBlock *BB, Packer *Pkr)
    : IsLoad(false), IsStore(false), BB(BB), VPCtx(Pkr->getContext(BB)),
      Elements(VPCtx->getNumValues()), Depended(VPCtx->getNumValues()),
      NumLanes(Inst->getLaneOps().size()), LaneId(0), Producer(Inst),
      LoadDAG(Pkr->getLoadDAG(BB)), StoreDAG(Pkr->getStoreDAG(BB)),
      LDA(Pkr->getLDA(BB)), MM(Pkr->getMatchManager(BB)), TTI(Pkr->getTTI()) {}

std::vector<Instruction *>
PartialPack::getUsableInsts(const Frontier *Frt) const {
  assert(!isFilled());
  std::vector<Instruction *> UsableInsts;

  auto IsUsable = [&](Instruction *I) -> bool {
    return Frt->isUsable(I) &&
           checkIndependence(LDA, *VPCtx, I, Elements, Depended);
  };

  if (IsLoad || IsStore) {
    const ConsecutiveAccessDAG *AccessDAG;
    if (IsLoad)
      AccessDAG = &LoadDAG;
    else
      AccessDAG = &StoreDAG;
    // For the first lane of a load/store pack, we want to make sure that
    // starting from the the first instruction we can both reach the focus and
    // fill the enough lanes.
    if (LaneId == 0) {
      for (auto &AccessAndNext : *AccessDAG) {
        auto *Access = AccessAndNext.first;
        if (IsUsable(Access))
          UsableInsts.push_back(Access);
      }
    } else {
      auto *LastAccess = FilledLanes[LaneId - 1];
      auto It = AccessDAG->find(LastAccess);
      if (It == AccessDAG->end()) {
        return {};
      }
      for (auto *NextAccess : It->second) {
        if (IsUsable(NextAccess))
          UsableInsts.push_back(NextAccess);
      }
    }
  } else {
    assert(Producer);
    // Find all matched operation at a given lane that's also independent
    const Operation *Op = Producer->getLaneOps()[LaneId].getOperation();
    for (auto &M : MM.getMatches(Op)) {
      auto *I = dyn_cast<Instruction>(M.Output);
      if (!I)
        continue;
      if (IsUsable(I))
        UsableInsts.push_back(I);
    }
  }

  return UsableInsts;
}

std::unique_ptr<PartialPack> PartialPack::fillOneLane(Instruction *I) const {
  auto Next = std::make_unique<PartialPack>(*this);
  Next->Elements.set(VPCtx->getScalarId(I));
  Next->Depended |= LDA.getDepended(I);
  if (auto *LI = dyn_cast<LoadInst>(I))
    Next->Loads.push_back(LI);
  else if (auto *SI = dyn_cast<StoreInst>(I))
    Next->Stores.push_back(SI);
  else {
    const Operation *Op = Producer->getLaneOps()[LaneId].getOperation();
    ArrayRef<Operation::Match> Matches = MM.getMatchesForOutput(Op, I);
    assert(!Matches.empty());
    Next->Matches.push_back(&Matches[0]);
  }
  Next->FilledLanes.push_back(I);
  ++Next->LaneId;

  return Next;
}

VectorPack *PartialPack::getPack() const {
  if (Elements.count() != NumLanes)
    return nullptr;

  if (IsLoad) {
    return VPCtx->createLoadPack(Loads, Elements, Depended, TTI);
  } else if (IsStore)
    return VPCtx->createStorePack(Stores, Elements, Depended, TTI);
  return VPCtx->createVectorPack(Matches, Elements, Depended, Producer, TTI);
}

// If we already have a UCTNode for the same frontier, reuse that node.
UCTNode *UCTNodeFactory::getNode(std::unique_ptr<Frontier> Frt) {
  decltype(FrontierToNodeMap)::iterator It;
  bool Inserted;
  std::tie(It, Inserted) = FrontierToNodeMap.try_emplace(Frt.get(), nullptr);
  assert(Inserted || !It->second->getPartialPack());
  if (Inserted) {
    It->first = Frt.get();
    auto *NewNode = new UCTNode(Frt.get());
    Nodes.push_back(std::unique_ptr<UCTNode>(NewNode));
    It->second = NewNode;
    Frontiers.push_back(std::move(Frt));
  }
  return It->second;
  auto *NewNode = new UCTNode(Frt.get());
  Nodes.push_back(std::unique_ptr<UCTNode>(NewNode));
  Frontiers.push_back(std::move(Frt));
  return Nodes.back().get();
}

UCTNode *UCTNodeFactory::getNode(const Frontier *Frt,
                                 std::unique_ptr<PartialPack> PP) {
  Nodes.push_back(std::unique_ptr<UCTNode>(new UCTNode(Frt, std::move(PP))));
  return Nodes.back().get();
}

static bool isPartialPackFeasible(const PartialPack &PP, const Frontier *Frt) {
  if (PP.isFilled())
    return true;
  for (auto *I : PP.getUsableInsts(Frt)) {
    std::unique_ptr<PartialPack> PPExtended = PP.fillOneLane(I);
    if (isPartialPackFeasible(*PPExtended, Frt)) {
      return true;
    }
  }
  return false;
}

// Assuming all elements of `OP` are loads, try to find an extending load pack.
static VectorPack *findExtendingLoadPack(const OperandPack &OP, BasicBlock *BB,
                                         Packer *Pkr) {
  // errs() << "Finding vector load to extend: {\n";
  // for (auto *V : OP)
  //  if (V)
  //    errs() << "\t" << *V << '\n';
  // errs() << "}\n\n";
  auto *VPCtx = Pkr->getContext(BB);
  auto &LoadDAG = Pkr->getLoadDAG(BB);
  auto &LDA = Pkr->getLDA(BB);

  // The set of loads producing elements of `OP`
  SmallPtrSet<Instruction *, 8> LoadSet;
  for (auto *V : OP) {
    if (!V)
      continue;
    if (auto *I = dyn_cast<Instruction>(V))
      LoadSet.insert(I);
  }

  // The loads might jumbled.
  // In other words, any one of the lanes could be the leading load
  for (auto *V : OP) {
    if (!V)
      continue;
    auto *LeadLI = cast<LoadInst>(V);
    BitVector Elements(VPCtx->getNumValues());
    BitVector Depended(VPCtx->getNumValues());
    Elements.set(VPCtx->getScalarId(LeadLI));
    Depended |= LDA.getDepended(LeadLI);
    std::vector<LoadInst *> Loads{LeadLI};

    LoadInst *CurLoad = LeadLI;
    while (Elements.count() < LoadSet.size()) {
      auto It = LoadDAG.find(CurLoad);
      // End of the chain
      if (It == LoadDAG.end())
        break;

      LoadInst *NextLI = nullptr;
      // Only use the next load in the load set
      for (auto *Next : It->second) {
        if (LoadSet.count(Next)) {
          NextLI = cast<LoadInst>(Next);
          break;
        }
      }
      if (!NextLI) {
        // load a don't care to fill the gap
        Loads.push_back(nullptr);
        CurLoad = cast<LoadInst>(*It->second.begin());
        continue;
      }
      if (!checkIndependence(LDA, *VPCtx, NextLI, Elements, Depended))
        break;
      Loads.push_back(NextLI);
      Elements.set(VPCtx->getScalarId(NextLI));
      Depended |= LDA.getDepended(NextLI);
      CurLoad = NextLI;
    }
    if (Elements.count() == LoadSet.size()) {
      // Pad
      while (Loads.size() < PowerOf2Ceil(OP.size()))
        Loads.push_back(nullptr);
      return VPCtx->createLoadPack(Loads, Elements, Depended, Pkr->getTTI());
    }
  }
  // errs() << "Failed!\n";
  return nullptr;
}

static std::vector<const VectorPack *> findExtensionPacks(const Frontier &Frt) {
  auto *Pkr = Frt.getPacker();
  auto *BB = Frt.getBasicBlock();
  auto &LDA = Pkr->getLDA(BB);
  auto *VPCtx = Pkr->getContext(BB);
  auto *TTI = Pkr->getTTI();
  auto &LoadDAG = Pkr->getLoadDAG(BB);
  auto &MM = Pkr->getMatchManager(BB);

  std::vector<const VectorPack *> Extensions;
  for (auto *OP : Frt.getUnresolvedPacks()) {
    unsigned NumLanes = OP->size();
    BitVector Elements(VPCtx->getNumValues());
    BitVector Depended(VPCtx->getNumValues());
    bool Extensible = true;
    bool AllLoads = true;
    for (unsigned i = 0; i < NumLanes; i++) {
      auto *V = (*OP)[i];
      // TODO: deal with nop lane
      if (!V) {
        continue;
      }
      auto *I = dyn_cast<Instruction>(V);
      if (!I) {
        AllLoads = false;
        continue;
      }
      if (I && (I->getParent() != BB || !Frt.isUsable(I))) {
        Extensible = false;
        break;
      }
      assert(Frt.isFree(I));
      unsigned InstId = VPCtx->getScalarId(I);
      if (!checkIndependence(LDA, *VPCtx, I, Elements, Depended)) {
        Extensible = false;
        break;
      }
      if (!isa<LoadInst>(I))
        AllLoads = false;
      Elements.set(InstId);
      Depended |= LDA.getDepended(I);
    }

    if (!Extensible)
      continue;

    if (AllLoads) {
      if (auto *LoadVP = findExtendingLoadPack(*OP, BB, Pkr))
        Extensions.push_back(LoadVP);
      continue;
    }

    for (auto *Inst : Pkr->getInsts()) {
      ArrayRef<BoundOperation> LaneOps = Inst->getLaneOps();
      if (LaneOps.size() != NumLanes)
        continue;

      std::vector<const Operation::Match *> Lanes;
      for (unsigned i = 0; i < NumLanes; i++) {
        auto *V = (*OP)[i];
        ArrayRef<Operation::Match> Matches =
            MM.getMatchesForOutput(LaneOps[i].getOperation(), V);
        if (Matches.empty())
          break;
        // FIXME: consider multiple matches for the same operation
        Lanes.push_back(&Matches[0]);
      }

      if (Lanes.size() == NumLanes) {
        Extensions.push_back(
            VPCtx->createVectorPack(Lanes, Elements, Depended, Inst, TTI));
        // break;
      }
    }
  }
  return Extensions;
}

// Fill out the children node
void UCTNode::expand(unsigned MaxNumLanes, UCTNodeFactory *Factory,
                     llvm::TargetTransformInfo *TTI) {
  assert(Transitions.empty() && "expanded already");
  auto *Pkr = getPacker();

  if (!PP) {
    // We are not working w/ any partial pack, start partial packs!
    auto *BB = Frt->getBasicBlock();
    for (auto *V : Frt->usableInsts()) {
      auto *I = dyn_cast<Instruction>(V);
      if (!I)
        continue;
      float Cost;
      auto *Next = Factory->getNode(Frt->advance(I, Cost, TTI));
      Transitions.emplace_back(I, Next, Cost);
    }

    //// Also consider the extension packs
    // std::vector<const VectorPack *> Extensions = findExtensionPacks(*Frt);
    // for (auto *VP : Extensions) {
    //  float Cost;
    //  auto *Next = Factory->getNode(Frt->advance(VP, Cost, TTI));
    //  Transitions.emplace_back(VP, Next, Cost);
    //}

    static std::vector<unsigned> VL{2, 4, 8, 16, 32};
    // Make a pack that contain the next free inst
    for (unsigned i : VL) {
      if (i > MaxNumLanes)
        continue;
      auto NewPP = std::make_unique<PartialPack>(true, false, BB, i, Pkr);
      if (isPartialPackFeasible(*NewPP, Frt))
        Transitions.emplace_back(Factory->getNode(Frt, std::move(NewPP)));
    }
    for (unsigned i : VL) {
      if (i > MaxNumLanes)
        continue;
      auto NewPP = std::make_unique<PartialPack>(false, true, BB, i, Pkr);
      if (isPartialPackFeasible(*NewPP, Frt))
        Transitions.emplace_back(Factory->getNode(Frt, std::move(NewPP)));
    }
    for (auto *Inst : getPacker()->getInsts()) {
      if (Inst->getLaneOps().size() > MaxNumLanes)
        continue;
      auto NewPP = std::make_unique<PartialPack>(Inst, BB, Pkr);
      if (isPartialPackFeasible(*NewPP, Frt)) {
        Transitions.emplace_back(Factory->getNode(Frt, std::move(NewPP)));
      }
    }
  } else {
    // We are filling out a partial pack
    std::vector<Instruction *> UsableInsts = PP->getUsableInsts(Frt);

    assert(!UsableInsts.empty());

    for (auto *I : UsableInsts) {
      std::unique_ptr<PartialPack> NextPP = PP->fillOneLane(I);
      if (!isPartialPackFeasible(*NextPP, Frt)) {
        continue;
      }
      if (auto *VP = NextPP->getPack()) {
        // Finished filling out this pack; move to the next frontier.
        float Cost;
        std::unique_ptr<Frontier> NextFrt = Frt->advance(VP, Cost, TTI);
        Transitions.emplace_back(VP, Factory->getNode(std::move(NextFrt)),
                                 Cost);
      } else {
        Transitions.emplace_back(Factory->getNode(Frt, std::move(NextPP)));
      }
    }
  }
}

// Do one iteration of MCTS
void UCTSearch::run(UCTNode *Root, unsigned NumIters) {
  struct FullTransition {
    UCTNode *Parent;
    UCTNode::Transition *T;
  };

  if (Root->expanded() && Root->transitions().size() == 1)
    NumIters = 1;

  std::vector<FullTransition> Path;
  for (unsigned Iter = 0; Iter < NumIters; Iter++) {
    Path.clear();

    // ========= 1) Selection ==========
    UCTNode *CurNode = Root;

    // Traverse down to a leaf node.
    while (CurNode->expanded()) {
      auto &Transitions = CurNode->transitions();
      // Transition weight given by some prior predictor (i.e., the apprentice)
      auto TransitionWeight = CurNode->transitionWeight();
      bool HasPredictions = TransitionWeight.size() > 0;

      auto ScoreTransition = [&](unsigned i) -> float {
        auto &T = Transitions[i];
        float Score = CurNode->score(T, C);
        if (HasPredictions)
          Score += W * TransitionWeight[i] / (float)(T.visitCount() + 1);
        return Score;
      };

      UCTNode::Transition *BestT = &Transitions[0];
      float MaxUCTScore = 0;
      if (BestT->visited())
        MaxUCTScore = ScoreTransition(0);

      for (unsigned i = 0; i < Transitions.size(); i++) {
        auto &T = Transitions[i];
        if (!T.visited()) {
          BestT = &T;
          break;
        }

        float UCTScore = ScoreTransition(i);
        if (UCTScore > MaxUCTScore) {
          MaxUCTScore = UCTScore;
          BestT = &T;
        }
      }

      Path.push_back(FullTransition{CurNode, BestT});
      CurNode = BestT->Next;
    }

    float LeafCost = 0;
    // ========= 2) Expansion ==========
    if (!CurNode->isTerminal()) {
      // ======= 3) Evaluation/Simulation =======
      LeafCost = evalLeafNode(CurNode);
      if (CurNode->visitCount() >= ExpandThreshold) {
        // FIXME: make max num lanes a parameter of MCTS ctor
        CurNode->expand(Policy ? Policy->getMaxNumLanes() : 8, Factory, TTI);
        auto &Transitions = CurNode->transitions();
        // Bias future exploration on this node if there is a prior
        if (Policy && Transitions.size() > 1)
          Policy->predictAsync(CurNode);
      }
    }

    // ========= 4) Backpropagation ===========
    CurNode->update(LeafCost);
    float TotalCost = LeafCost;
    for (FullTransition &FT : make_range(Path.rbegin(), Path.rend())) {
      TotalCost += FT.T->Cost;
      FT.Parent->update(TotalCost);
      FT.T->Count += 1;
    }
  }
}

// Find the subset of `Extensions` that are compatible with the partial pack
// `PP`
static std::vector<const VectorPack *>
findCompatibleExtensions(const PartialPack &PP,
                         ArrayRef<const VectorPack *> Extensions) {
  ArrayRef<Instruction *> FilledLanes = PP.getFilledLanes();
  std::vector<const VectorPack *> CompatibleExts;
  for (auto *VP : Extensions) {
    ArrayRef<Value *> OutputLanes = VP->getOrderedValues();
    if (OutputLanes.size() != PP.getNumLanes())
      continue;

    // Check if the prefixes match
    bool Compatible = true;
    for (unsigned i = 0; i < FilledLanes.size(); i++) {
      if (FilledLanes[i] != OutputLanes[i]) {
        Compatible = false;
        break;
      }
    }
    if (Compatible)
      CompatibleExts.push_back(VP);
  }
  return CompatibleExts;
}

// Uniformly random rollout
float RolloutEvaluator::evaluate(unsigned MaxNumLanes, unsigned EnumCap,
                                 const Frontier *Frt, const PartialPack *PP,
                                 PackEnumerationCache &EnumCache, Packer *Pkr) {
  Frontier FrtScratch = *Frt;
  BasicBlock *BB = Frt->getBasicBlock();
  float Cost = 0;
  auto *TTI = Pkr->getTTI();

  // If we are still filling out a partial pack,
  // use do a random rollout to fill out the partial pack.
  if (PP) {
    std::unique_ptr<PartialPack> PPScratch;
    auto Extensions = findExtensionPacks(*Frt);
    auto CompatibleExtensions = Extensions;
    for (;;) {
      auto UsableInsts = PP->getUsableInsts(Frt);
      CompatibleExtensions =
          findCompatibleExtensions(*PP, CompatibleExtensions);
      unsigned LaneId = PP->getFilledLanes().size();
      DenseSet<Value *> ExtendingInsts;
      for (auto *VP : CompatibleExtensions)
        ExtendingInsts.insert(VP->getOrderedValues()[LaneId]);

      std::vector<std::unique_ptr<PartialPack>> NextPPs;
      for (auto *I : UsableInsts) {
        auto NextPP = PP->fillOneLane(I);
        if (isPartialPackFeasible(*NextPP, Frt)) {
          if (ExtendingInsts.count(I))
            NextPPs.push_back(std::move(NextPP));
        }
      }
      // If there's no compatible extensions then we just fill the next pack
      // randomly. Otherwise we only go with instruction that can lead to
      // extensions
      if (NextPPs.empty()) {
        for (auto *I : UsableInsts) {
          auto NextPP = PP->fillOneLane(I);
          if (isPartialPackFeasible(*NextPP, Frt)) {
            NextPPs.push_back(std::move(NextPP));
          }
        }
      }

      assert(!NextPPs.empty());
      PPScratch = std::move(NextPPs[rand_int(NextPPs.size())]);
      // PPScratch = std::move(NextPPs[0]);
      auto *VP = PPScratch->getPack();
      if (VP) {
        Cost += FrtScratch.advanceInplace(VP, TTI);
        break;
      }
      PP = PPScratch.get();
    }
  }

  // errs() << "<<<<<< unresolved packs:\n";
  // for (auto *OP : FrtScratch.getUnresolvedPacks()) {
  //  if (OP->size() != 4)
  //    continue;
  //  for (auto *V : *OP)
  //    errs() << " " << *V;
  //  errs() << '\n';
  //}

  for (;;) {
    std::vector<const VectorPack *> Extensions = findExtensionPacks(FrtScratch);

    if (!Extensions.empty()) {
      auto *VP = Extensions[rand_int(Extensions.size())];
      // auto *VP = Extensions[0];
      // errs() << "Extending with: " << *VP << '\n';
      Cost += FrtScratch.advanceInplace(VP, TTI);
    } else {
      for (auto *V : FrtScratch.usableInsts()) {
        auto *I = dyn_cast<Instruction>(V);
        if (!I)
          continue;
        // errs() << "Scalarizing " << *I << '\n';
        Cost += FrtScratch.advanceInplace(I, TTI);
        break;
      }
    }
    // errs() << "\tnew cost: "<< Cost <<'\n';
    if (FrtScratch.getUnresolvedPacks().empty() &&
        FrtScratch.numUnresolvedScalars() == 0)
      break;
  }
  // errs() << ">>>>>>>\n\n";
  return Cost;
}

class SlotSet {
  std::vector<LoadInst *> Slots;
  unsigned MinId, MaxId;
  unsigned NumElems = 0;
  unsigned HasValue = false;
public:
  LoadInst *operator[](unsigned i) const {
    return Slots[i];
  }
  bool try_insert(unsigned i, LoadInst *LI) {
    if (i >= Slots.size())
      Slots.resize(i + 1);
    if (!Slots[i] || Slots[i] == LI) {
      Slots[i] = LI;
      if (HasValue) {
        MinId = std::min(i, MinId);
        MaxId = std::max(i, MaxId);
      } else {
        MinId = MaxId = i;
        HasValue = true;
      }
      NumElems ++;
      return true;
    }
    return false;
  }

  double utilization() const {
    return (double)NumElems / (MaxId - MinId + 1);
  }

  unsigned num_elems() const {
    return NumElems;
  }

  unsigned minId() const {
    return MinId;
  }

  unsigned maxId() const {
    return MaxId;
  }
};

// Try to coalesce main pack with some other packs
static VectorPack *tryCoalesceLoads(const VectorPack *MainPack,
                                    ArrayRef<VectorPack *> OtherPacks,
                                    Packer *Pkr) {
  auto *BB = MainPack->getBasicBlock();
  auto &LayoutInfo = Pkr->getLoadInfo(BB);
  // Full, can't coalesce
  if (MainPack->getOrderedValues().size() == MainPack->getElements().count())
    return nullptr;

  auto *SomeLoad = *MainPack->elementValues().begin();
  assert(Loads[0]);
  auto *Leader = LayoutInfo.get(cast<Instruction>(SomeLoad)).Leader;
  BitVector Elements = MainPack->getElements();
  BitVector Depended = MainPack->getDepended();
  SlotSet Slots;
  for (auto *V : MainPack->elementValues()) {
    auto *LI = cast<LoadInst>(V);
    unsigned SlotId = LayoutInfo.get(LI).Id;
    Slots.try_insert(SlotId, LI);
  }

  for (auto *Other : OtherPacks) {
    auto &Info = LayoutInfo.get(cast<Instruction>(Other->getOrderedValues()[0]));
    // Cannot coalesce with loads accessing a different object
    if (Info.Leader != Leader)
      continue;
    // Cannot coalesce if not independent
    if (Depended.anyCommon(Other->getElements()) ||
        Other->getDepended().anyCommon(Elements))
      continue;

    auto Temp = Slots;
    bool Coalesced = true;
    for (auto *V : Other->elementValues()) {
      auto *LI = cast<LoadInst>(V);
      unsigned SlotId = LayoutInfo.get(LI).Id;
      // Can only coalesce if the slot if empty
      bool Ok = Temp.try_insert(SlotId, LI);;
      if (!Ok) {
        Coalesced = false;
        break;
      }
    }
    if (Coalesced && Temp.utilization() > Slots.utilization()) {
      Slots = Temp;
      Depended |= Other->getDepended();
      Elements |= Other->getElements();
    }
  }

  if (Elements == MainPack->getElements())
    return nullptr;

  std::vector<LoadInst *> Loads;
  for (unsigned i = Slots.minId(), e = Slots.maxId(); i != e; i++) {
    Loads.push_back(Slots[i]);
  }

  return Pkr->getContext(BB)->createLoadPack(Loads, Elements, Depended, Pkr->getTTI());
}


static std::vector<VectorPack *> findExtensionPacks2(const Frontier &Frt) {
  auto *Pkr = Frt.getPacker();
  auto *BB = Frt.getBasicBlock();
  auto &LDA = Pkr->getLDA(BB);
  auto *VPCtx = Pkr->getContext(BB);
  auto *TTI = Pkr->getTTI();
  auto &LoadDAG = Pkr->getLoadDAG(BB);
  auto &MM = Pkr->getMatchManager(BB);

  // Put load extensions in a separate category.
  // We don't extend with a load pack if we can extend with other arithmetic
  // packs
  std::vector<VectorPack *> LoadExtensions;

  std::vector<VectorPack *> Extensions;
  for (auto *OP : Frt.getUnresolvedPacks()) {
    ////////
    errs() << "Looking for a pack to extend:{\n";
    for (auto *V : *OP)
      if (V)
        errs() << *V << '\n';
      else
        errs() << "undef\n";
    errs() << "}\n";
    ///////
    if (!Extensions.empty())
      break;

    unsigned NumLanes = OP->size();
    BitVector Elements(VPCtx->getNumValues());
    BitVector Depended(VPCtx->getNumValues());
    bool Extensible = true;
    bool AllLoads = true;
    bool HasUndef = false;
    for (unsigned i = 0; i < NumLanes; i++) {
      auto *V = (*OP)[i];
      if (!V) {
        HasUndef = true;
        continue;
      }
      auto *I = dyn_cast<Instruction>(V);
      if (!I) {
        AllLoads = false;
        continue;
      }
      if (!I || I->getParent() != BB || !Frt.isUsable(I)) {
        Extensible = false;
        break;
      }
      unsigned InstId = VPCtx->getScalarId(I);
      if (!checkIndependence(LDA, *VPCtx, I, Elements, Depended)) {
        Extensible = false;
        break;
      }
      if (!isa<LoadInst>(I))
        AllLoads = false;
      Elements.set(InstId);
      Depended |= LDA.getDepended(I);
    }

    errs() << "Extensible? " << Extensible
      << ", AllLoads? " << AllLoads 
      << '\n';

    if (!Extensible)
      continue;

    if (AllLoads) {
      if (auto *LoadVP = findExtendingLoadPack(*OP, BB, Pkr))
        LoadExtensions.push_back(LoadVP);
      continue;
    }

    if (HasUndef)
      continue;

    for (auto *Inst : Pkr->getInsts()) {
      ArrayRef<BoundOperation> LaneOps = Inst->getLaneOps();
      if (LaneOps.size() != NumLanes)
        continue;

      std::vector<const Operation::Match *> Lanes;
      for (unsigned i = 0; i < NumLanes; i++) {
        ArrayRef<Operation::Match> Matches =
            MM.getMatchesForOutput(LaneOps[i].getOperation(), (*OP)[i]);
        errs() << "!!! num matches" << Matches.size() << " \n";
        if (Matches.empty())
          break;
        // FIXME: consider multiple matches for the same operation
        Lanes.push_back(&Matches[0]);
      }

      if (Lanes.size() == NumLanes) {
        Extensions.push_back(
            VPCtx->createVectorPack(Lanes, Elements, Depended, Inst, TTI));
      }
    }
  }

  if (!Extensions.empty())
    return Extensions;

  if (!LoadExtensions.empty()) {
    auto *LoadVP = LoadExtensions[0];
    if (auto *Coalesced = tryCoalesceLoads(
            LoadVP, ArrayRef<VectorPack *>(LoadExtensions).slice(1),
            Pkr)) {
      return {Coalesced, LoadVP};
    }
    return {LoadVP};
  }

  return {};
}


static VectorPack *findExtensionPack(const Frontier &Frt) {
  {
    auto Exts = findExtensionPacks2(Frt);
    if (Exts.empty())
      return nullptr;
    return Exts[0];
  }
  auto *Pkr = Frt.getPacker();
  auto *BB = Frt.getBasicBlock();
  auto &LDA = Pkr->getLDA(BB);
  auto *VPCtx = Pkr->getContext(BB);
  auto *TTI = Pkr->getTTI();
  auto &LoadDAG = Pkr->getLoadDAG(BB);
  auto &MM = Pkr->getMatchManager(BB);

  std::vector<VectorPack *> Extensions;
  for (auto *OP : Frt.getUnresolvedPacks()) {
    // errs() << "Looking for a pack to extend:{\n";
    // for (auto *V : *OP)
    //  if (V) errs() << *V << '\n';
    // errs() << "}\n";

    unsigned NumLanes = OP->size();
    BitVector Elements(VPCtx->getNumValues());
    BitVector Depended(VPCtx->getNumValues());
    bool Extensible = true;
    bool AllLoads = true;
    for (unsigned i = 0; i < NumLanes; i++) {
      auto *V = (*OP)[i];
      // TODO: support nop lane
      if (!V) {
        Extensible = false;
        break;
      }
      auto *I = dyn_cast<Instruction>(V);
      if (!I) {
        AllLoads = false;
        continue;
      }
      if (!I || I->getParent() != BB || !Frt.isUsable(I)) {
        Extensible = false;
        break;
      }
      unsigned InstId = VPCtx->getScalarId(I);
      if (!checkIndependence(LDA, *VPCtx, I, Elements, Depended)) {
        Extensible = false;
        break;
      }
      if (!isa<LoadInst>(I))
        AllLoads = false;
      Elements.set(InstId);
      Depended |= LDA.getDepended(I);
    }

    // errs() << "Extensible? " << Extensible
    //  << ", AllLoads? " << AllLoads << '\n';

    if (!Extensible)
      continue;

    if (AllLoads) {
      if (auto *LoadVP = findExtendingLoadPack(*OP, BB, Pkr))
        Extensions.push_back(LoadVP);
      continue;
    }
    for (auto *Inst : Pkr->getInsts()) {
      ArrayRef<BoundOperation> LaneOps = Inst->getLaneOps();
      if (LaneOps.size() != NumLanes)
        continue;

      std::vector<const Operation::Match *> Lanes;
      for (unsigned i = 0; i < NumLanes; i++) {
        ArrayRef<Operation::Match> Matches =
            MM.getMatchesForOutput(LaneOps[i].getOperation(), (*OP)[i]);
        if (Matches.empty())
          break;
        // FIXME: consider multiple matches for the same operation
        Lanes.push_back(&Matches[0]);
      }

      if (Lanes.size() == NumLanes) {
        Extensions.push_back(
            VPCtx->createVectorPack(Lanes, Elements, Depended, Inst, TTI));
      }
    }
  }

  if (Extensions.empty())
    return nullptr;

  // Take the extension pack with the lowest local cost
  std::sort(Extensions.begin(), Extensions.end(),
            [](const VectorPack *A, const VectorPack *B) {
              return A->getCost() < B->getCost();
            });
  return Extensions[0];
}

float estimateCost(Frontier Frt, VectorPack *VP) {
  auto *Pkr = Frt.getPacker();
  auto *BB = Frt.getBasicBlock();
  auto &LDA = Pkr->getLDA(BB);
  auto *VPCtx = Pkr->getContext(BB);
  auto *TTI = Pkr->getTTI();

  float Cost = Frt.advanceInplace(VP, TTI);
  for (;;) {
    auto *ExtVP = findExtensionPack(Frt);
    if (!ExtVP)
      break;
    Cost += Frt.advanceInplace(ExtVP, TTI);
    //errs() << "!!! Extending with: "<< *ExtVP << ", COST AFTER EXTENSION = " << Cost << '\n';
  }

  while (Frt.numUnresolvedScalars() != 0 || Frt.getUnresolvedPacks().size()) {
    for (auto *V : Frt.usableInsts()) {
      if (auto *I = dyn_cast<Instruction>(V)) {
        Cost += Frt.advanceInplace(I, TTI);
        //errs() << "!!! Scalarizing "<< *I << ", COST AFTER = " << Cost << '\n';
        break;
      }
    }
  }

  //errs() << "!!! est cost : " << Cost << " of  " << *VP << '\n';
  return Cost;
}

static float estimateAllScalarCost(const Frontier &Frt,
                                   TargetTransformInfo *TTI) {
  // errs() << "Finding vector load to extend: {\n";
  // for (auto *V : OP)
  //  if (V)
  //    errs() << "\t" << *V << '\n';
  // errs() << "}\n\n";
  auto *BB = Frt.getBasicBlock();
  float Cost = 0;
  // Pay insertion cost
  for (auto *OP : Frt.getUnresolvedPacks()) {
    auto *VecTy = getVectorType(*OP);
    for (unsigned i = 0, e = OP->size(); i != e; i++) {
      auto *V = (*OP)[i];
      if (!V)
        continue;
      auto *I = dyn_cast<Instruction>(V);
      if (!I || I->getParent() != BB || !Frt.isFree(I))
        continue;
      if (i == 0 && is_splat(*OP)) {
        Cost +=
            TTI->getShuffleCost(TargetTransformInfo::SK_Broadcast, VecTy, 0);
        break;
      }
      Cost += 2*TTI->getVectorInstrCost(Instruction::InsertElement, VecTy, i);
    }
  }
  return Cost;
}

class DPSolver {
  struct Solution {
    float Cost;
    const VectorPack *VP;

    // Default solution is no extension
    Solution() = default;
    Solution(float Cost, VectorPack *VP) : Cost(Cost), VP(VP) {}
  };
  TargetTransformInfo *TTI;

  DenseMap<const Frontier *, Solution, FrontierHashInfo> Solutions;
  std::vector<std::unique_ptr<Frontier>> Frontiers;

  Solution solveImpl(const Frontier &Frt) {
    // Figure out the cost of not adding any extensions.
    Solution Sol;
    Sol.VP = nullptr;
    Sol.Cost = 0;
    Sol.Cost = estimateAllScalarCost(Frt, TTI);
    // auto FrtScratch = Frt;
    // while (FrtScratch.numUnresolvedScalars() != 0 ||
    // FrtScratch.getUnresolvedPacks().size()) {
    //  for (auto *V : FrtScratch.usableInsts()) {
    //    if (auto *I = dyn_cast<Instruction>(V)) {
    //      Sol.Cost += FrtScratch.advanceInplace(I, TTI);
    //      break;
    //    }
    //  }
    //}

    // Figure out the cost of adding one extension
    auto Extensions = findExtensionPacks2(Frt);
    //errs() << "NUM EXTENSIONS: " << Extensions.size() << '\n';
    for (const VectorPack *ExtVP : Extensions) {
      float LocalCost;
      auto NextFrt = Frt.advance(ExtVP, LocalCost, TTI);

      float TotalCost = solve(std::move(NextFrt)).Cost + LocalCost;
      errs () << " EXTENDING WITH " << *ExtVP
       << ", transition cost : " << LocalCost
       << ", local cost : " << ExtVP->getCost()
       << ", total cost : " << TotalCost
       << ", num elems: " << ExtVP->getOrderedValues().size()
       << ", best cost so far: " << Sol.Cost
       << '\n';

      if (Sol.Cost > TotalCost) {
        Sol.Cost = TotalCost;
        Sol.VP = ExtVP;
      }
    }
    return Sol;
  }

public:
  DPSolver(TargetTransformInfo *TTI) : TTI(TTI), Solutions(1000000) {}

  Solution solve(const Frontier &Frt) {
    auto It = Solutions.find(&Frt);
    // Solved already
    if (It != Solutions.end())
      return It->second;

    auto Sol = solveImpl(Frt);
    Frontiers.push_back(std::make_unique<Frontier>(Frt));
    return Solutions[Frontiers.back().get()] = Sol;
  }

  Solution solve(std::unique_ptr<Frontier> Frt) {
    auto It = Solutions.find(Frt.get());
    // Solved already
    if (It != Solutions.end())
      return It->second;

    auto Sol = solveImpl(*Frt);
    Frontiers.push_back(std::move(Frt));
    return Solutions[Frontiers.back().get()] = Sol;
  }
};

std::vector<VectorPack *> getSeedStorePacks(const Frontier &Frt, StoreInst *SI,
                                            unsigned VL) {
  if (!Frt.isUsable(SI)) {
    return {};
  }

  auto *Pkr = Frt.getPacker();
  auto *BB = Frt.getBasicBlock();
  auto &LDA = Pkr->getLDA(BB);
  auto *VPCtx = Pkr->getContext(BB);
  auto *TTI = Pkr->getTTI();
  auto &StoreDAG = Pkr->getStoreDAG(BB);

  std::vector<VectorPack *> Seeds;

  std::function<void(std::vector<StoreInst *>, BitVector, BitVector)>
      Enumerate = [&](std::vector<StoreInst *> Stores, BitVector Elements,
                      BitVector Depended) {
        if (Stores.size() == VL) {
          Seeds.push_back(
              VPCtx->createStorePack(Stores, Elements, Depended, TTI));
          return;
        }

        auto It = StoreDAG.find(Stores.back());
        if (It == StoreDAG.end()) {
          return;
        }
        for (auto *Next : It->second) {
          auto *NextSI = cast<StoreInst>(Next);
          if (!Frt.isUsable(NextSI)) {
            continue;
          }
          if (!checkIndependence(LDA, *VPCtx, NextSI, Elements, Depended)) {
            continue;
          }
          auto StoresExt = Stores;
          auto ElementsExt = Elements;
          auto DependedExt = Depended;
          StoresExt.push_back(NextSI);
          ElementsExt.set(VPCtx->getScalarId(NextSI));
          DependedExt |= LDA.getDepended(NextSI);
          Enumerate(StoresExt, ElementsExt, DependedExt);
        }
      };

  std::vector<StoreInst *> Stores{SI};
  BitVector Elements(VPCtx->getNumValues());
  BitVector Depended(VPCtx->getNumValues());

  Elements.set(VPCtx->getScalarId(SI));
  Depended |= LDA.getDepended(SI);

  Enumerate(Stores, Elements, Depended);
  return Seeds;
}

VectorPack *getSeedStorePack(const Frontier &Frt, StoreInst *SI, unsigned VL) {
  auto Seeds = getSeedStorePacks(Frt, SI, VL);
  if (Seeds.empty())
    return nullptr;
  return Seeds[0];
}

float optimizeBottomUp(VectorPackSet &Packs, Packer *Pkr, BasicBlock *BB) {
  Frontier Frt(BB, Pkr);
  auto &StoreDAG = Pkr->getStoreDAG(BB);

  DenseMap<Instruction *, unsigned> StoreChainLen;
  std::function<unsigned(Instruction *)> GetChainLen =
      [&](Instruction *I) -> unsigned {
    if (StoreChainLen.count(I))
      return StoreChainLen[I];
    auto It = StoreDAG.find(I);
    if (It == StoreDAG.end())
      return StoreChainLen[I] = 1;
    unsigned MaxLen = 0;
    for (auto *Next : It->second) {
      MaxLen = std::max<unsigned>(MaxLen, GetChainLen(Next));
    }
    return StoreChainLen[I] = MaxLen + 1;
  };

  std::vector<StoreInst *> Stores;
  for (auto &StoreAndNext : StoreDAG)
    Stores.push_back(cast<StoreInst>(StoreAndNext.first));

  // Sort stores by store chain length
  std::sort(Stores.begin(), Stores.end(), [&](StoreInst *A, StoreInst *B) {
    return GetChainLen(A) > GetChainLen(B);
  });

  errs() << "??? num stores: " << Stores.size() << '\n';

  auto *TTI = Pkr->getTTI();

  DPSolver Solver(TTI);

  std::vector<unsigned> VL{64, 32, 16, 8, 4, 2};
  float Cost = 0;
  float BestEst = 0;

  for (unsigned i : VL) {
    for (auto *SI : Stores) {
      auto *SeedVP = getSeedStorePack(Frt, SI, i);
      if (SeedVP) {
#if 0
        float Est = estimateCost(Frt, SeedVP);
#else
        float LocalCost;
        auto Sol = Solver.solve(Frt.advance(SeedVP, LocalCost, TTI));
        float Est = LocalCost + Sol.Cost;
#endif
        errs() << "Estimated cost of " << *SeedVP
          <<  " is " << Est
          << ", local cost: " << LocalCost
          <<", trans cost: "<< Sol.Cost
          << '\n';
        if (Est < BestEst) {
#if 0
           Cost += Frt.advanceInplace(SeedVP, TTI);
           Packs.tryAdd(SeedVP);
           BestEst = Est;

#else
          //////////////
          Cost += Frt.advanceInplace(SeedVP, TTI);
          Packs.tryAdd(SeedVP);
          while (auto *ExtVP = Solver.solve(Frt).VP) {
    //errs() << "!!! Adding : " << *ExtVP << '\n';
    //errs() << "\t updated cost: " << Cost << '\n';
            Cost += Frt.advanceInplace(ExtVP, TTI);
            Packs.tryAdd(ExtVP);
          }
          BestEst = estimateAllScalarCost(Frt, TTI);
          /////////////
#endif
        }
      }
    }
  }
  for (;;) {
#if 0
    auto *ExtVP = findExtensionPack(Frt);
#else
    auto *ExtVP = Solver.solve(Frt).VP;
#endif

    if (!ExtVP)
      break;
    Cost += Frt.advanceInplace(ExtVP, TTI);
    //errs() << "!!! Adding : " << *ExtVP << '\n';
    //errs() << "\t updated cost: " << Cost << '\n';
    Packs.tryAdd(ExtVP);
  }

  while (Frt.numUnresolvedScalars() != 0 || Frt.getUnresolvedPacks().size()) {
    for (auto *V : Frt.usableInsts()) {
      if (auto *I = dyn_cast<Instruction>(V)) {
        // errs() << "!!! scalarizing: " << *I << '\n';
        Cost += Frt.advanceInplace(I, TTI);
        // errs() << "\t updated cost: " << Cost << '\n';
        break;
      }
    }
  }

  return Cost;
}
