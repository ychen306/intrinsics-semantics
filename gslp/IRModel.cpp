#include "IRModel.h"
#include "MatchManager.h"
#include "Packer.h"
#include "Preprocessing.h"
#include "Solver.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/FormatVariadic.h"
#include <map>
#include <torch/nn/module.h>
#include <torch/torch.h>

using namespace llvm;
using namespace torch;

namespace {
class OpcodeTable {
  static unsigned getUnknownTypeId() { return 0; }
  static unsigned getConstId() { return 1; }
  static unsigned getCastId() { return 2; }
  static unsigned getBitwidth(llvm::Type *Ty) {
    if (auto *IntTy = dyn_cast<IntegerType>(Ty))
      return IntTy->getBitWidth();
    if (Ty->isFloatTy())
      return 32;
    if (Ty->isDoubleTy())
      return 64;
    return 0; // don't care
  }

  static std::vector<unsigned> Bitwidths;
  static std::vector<unsigned> Opcodes;
  std::map<std::pair<unsigned, unsigned>, unsigned> ValueTypeIds;

public:
  OpcodeTable() {
    for (unsigned BW : Bitwidths)
      for (unsigned Opc : Opcodes) {
        unsigned TypeId = ValueTypeIds.size();
        ValueTypeIds[{Opc, BW}] = TypeId;
      }
  }
  unsigned getNumValueTypes() const {
    // # of value types = <# inst opcode> * <# bitwidths> + <constant> + <cast>
    // <unknown>
    return Opcodes.size() * Bitwidths.size() + 1 + 1 + 1;
  }

  unsigned getValueTypeId(Value *V) const {
    if (isa<ConstantInt>(V) || isa<ConstantFP>(V))
      return getConstId();
    if (auto *I = dyn_cast<Instruction>(V)) {
      if (I->isCast())
        return getCastId();
      llvm::Type *Ty;
      if (auto *SI = dyn_cast<StoreInst>(I))
        Ty = SI->getValueOperand()->getType();
      else
        Ty = I->getType();
      auto It = ValueTypeIds.find({I->getOpcode(), getBitwidth(Ty)});
      if (It != ValueTypeIds.end())
        return It->second;
    }
    return getUnknownTypeId();
  }

} OpTable;

torch::Tensor buildAdjacencyMat(llvm::ArrayRef<DiEdge> Edges, unsigned N,
                                unsigned M, bool Flip = false) {
  auto CooIndices = torch::empty({2, (int64_t)Edges.size()},
                                 TensorOptions().dtype(torch::kInt64));
  auto CooIndicesRef = CooIndices.accessor<int64_t, 2>();
  for (unsigned i = 0; i < Edges.size(); i++) {
    if (Flip) {
      CooIndicesRef[1][i] = (int64_t)Edges[i].Src;
      CooIndicesRef[0][i] = (int64_t)Edges[i].Dest;
    } else {
      CooIndicesRef[0][i] = (int64_t)Edges[i].Src;
      CooIndicesRef[1][i] = (int64_t)Edges[i].Dest;
    }
  }
  return torch::sparse_coo_tensor(CooIndices,
                                  torch::ones({(int64_t)Edges.size()}), {N, M});
}

template <typename OutStreamTy>
void dumpShape(torch::Tensor X, OutStreamTy &Os) {
  for (auto N : X.sizes()) {
    Os << " " << N;
  }
  Os << '\n';
}

} // end anonymous namespace

std::vector<unsigned> OpcodeTable::Bitwidths = {8, 16, 32, 64};
// TODO: support PHI
std::vector<unsigned> OpcodeTable::Opcodes = {
    /*Instruction::PHI, */ Instruction::Load,
    Instruction::Store,
    Instruction::Add,
    Instruction::FAdd,
    Instruction::Sub,
    Instruction::FSub,
    Instruction::Mul,
    Instruction::FMul,
    Instruction::UDiv,
    Instruction::SDiv,
    Instruction::FDiv,
    Instruction::URem,
    Instruction::SRem,
    Instruction::FRem,
    Instruction::Shl,
    Instruction::LShr,
    Instruction::AShr,
    Instruction::And,
    Instruction::Or,
    Instruction::Xor};

void IRIndex::trackValue(Value *V) {
  if (Value2IdMap.count(V))
    return;
  unsigned Id = Values.size();
  Values.push_back(V);
  Value2IdMap[V] = Id;
}

IRIndex::IRIndex(llvm::Function &F) {
  for (auto &I : make_range(inst_begin(F), inst_end(F))) {
    trackValue(&I);
    for (Value *Operand : I.operands())
      trackValue(Operand);
  }
}

IRIndex::IRIndex(const Frontier *Frt) {
  BasicBlock *BB = Frt->getBasicBlock();
  for (auto &I : *BB) {
    if (!Frt->isFree(&I))
      continue;
    trackValue(&I);
    for (Value *Operand : I.operands())
      trackValue(Operand);
  }
}

class BatchedGraphBuilder {
  unsigned N, M;
  std::vector<DiEdge> Edges;

protected:
  void addEdge(unsigned U, unsigned V) { Edges.emplace_back(U + N, V + M); }
  void finishBatch(unsigned NN, unsigned MM) {
    N += NN;
    M += MM;
  }

public:
  BatchedGraphBuilder() : N(0), M(0) {}
  torch::Tensor getBatched(bool Flip = false) const {
    return buildAdjacencyMat(Edges, N, M, Flip);
  }
};

static torch::Tensor getValueTypes(llvm::ArrayRef<IRIndex> Indexes) {
  std::vector<int64_t> ValueTypes;
  for (auto &Index : Indexes) {
    unsigned N = Index.getNumValues();
    for (unsigned i = 0; i < N; i++)
      ValueTypes.push_back(OpTable.getValueTypeId(Index.get(i)));
  }
  return torch::from_blob(ValueTypes.data(), {(int64_t)ValueTypes.size()},
                          torch::TensorOptions().dtype(torch::kInt64))
      .clone();
}

PackingModelImpl::PackingModelImpl(unsigned EmbSize,
                                   llvm::ArrayRef<InstBinding *> InstPool,
                                   unsigned MaxNumLanes)
    : EmbSize(EmbSize), InstPool(InstPool), MaxNumLanes(MaxNumLanes) {
  // ======= Parameters for initializing the states =======
  OpcodeEmb = register_module(
      "opcode_embedding",
      nn::Embedding(nn::EmbeddingOptions(OpTable.getNumValueTypes(), EmbSize)));
  InitUse = register_parameter("init_use", torch::randn(EmbSize));

  // ======= Message passing =======
  StateToUseMsg1 = register_module("state2msg1", nn::Linear(EmbSize, EmbSize));
  StateToUseMsg2 = register_module("state2msg2", nn::Linear(EmbSize, EmbSize));
  StateToMemMsg = register_module("state2mem", nn::Linear(EmbSize, EmbSize));
  StateToIndependentMsg =
      register_module("state2ind", nn::Linear(EmbSize, EmbSize));
  StateToUnresolvedMsg =
      register_module("state2unresolved", nn::Linear(EmbSize, EmbSize));

  UnresolvedToMsg = register_module("use2msg", nn::Linear(EmbSize, EmbSize));

  // ======= Read out =======
  // Opcode = nop + <insts from inst pool> + <stores from 2 to max vl> + <loads
  // ...>
  unsigned NumPackOps = InstPool.size() + 1 + (MaxNumLanes - 2 + 1);
  StateToOpcode =
      register_module("state2inst", nn::Linear(EmbSize, NumPackOps));
  StateToEmb = register_module("state2emb", nn::Linear(EmbSize, EmbSize));
  for (unsigned i = 0; i < MaxNumLanes; i++)
    StateToLaneEmbs.push_back(register_module(formatv("state2lane{0}", i),
                                              nn::Linear(EmbSize, EmbSize)));

  // ======= RNN for aggregating state and messages =======
  // Input = operand1 x operand2 x <left mem> x <right mem> x <independent> x
  // <unresolved use>
  ValueGRU = register_module(
      "value_gru", nn::GRUCell(nn::GRUCellOptions(EmbSize * 6, EmbSize)));
  UseGRU = register_module("use_gru", nn::GRUCell(nn::GRUCellOptions(
                                          EmbSize * MaxNumLanes, EmbSize)));
}

std::vector<PackDistribution>
PackingModelImpl::batch_forward(llvm::ArrayRef<const Frontier *> Frontiers,
                                Packer *Pkr, torch::Device Device,
                                unsigned NumIters) {
  unsigned N = 0, NumUnresolvedUses = 0;
  std::vector<IRIndex> Indexes;
  FrontierPreprocessor<BatchedGraphBuilder> Preprocessor(MaxNumLanes);

  for (auto *Frt : Frontiers) {
    IRIndex Index(Frt);
    unsigned NumValues;
    unsigned NumUses;
    Preprocessor.process(Frt, Index, Pkr, NumValues, NumUses);
    N += NumValues;
    NumUnresolvedUses += NumUses;
    Indexes.push_back(std::move(Index));
  }

  auto UseGraph1 = Preprocessor.use1().getBatched().to(Device);
  auto UseGraph2 = Preprocessor.use2().getBatched().to(Device);
  auto LeftMemRefGraph = Preprocessor.memRefs().getBatched().to(Device);
  auto RightMemRefGraph =
      Preprocessor.memRefs().getBatched(true /*flip*/).to(Device);
  auto IndependenceGraph = Preprocessor.independence().getBatched().to(Device);
  auto InvUnresolvedGraph =
      Preprocessor.invUnresolved().getBatched().to(Device);
  std::vector<torch::Tensor> UnresolvedUseGraphs;
  for (auto &G : Preprocessor.unresolved())
    UnresolvedUseGraphs.push_back(G.getBatched().to(Device));

  auto ValueTypes = getValueTypes(Indexes).to(Device);

  // Initialize the states
  auto H_value = OpcodeEmb->forward(ValueTypes).view({N, EmbSize});
  auto H_use = InitUse.repeat({NumUnresolvedUses, 1});

  // Pass message from values to unresolved uses
  auto SendToUses = [&](torch::Tensor H_value) -> torch::Tensor {
    std::vector<torch::Tensor> Messages;
    for (auto &G : UnresolvedUseGraphs)
      Messages.push_back(torch::mm(G, H_value));
    return torch::cat(Messages, 1 /*dim*/);
  };

  auto Zeros = torch::zeros({N, EmbSize}).to(Device);

  // Pass message from values and unresolved uses to values themselves
  auto SendToValues = [&](torch::Tensor H_value,
                          torch::Tensor H_use) -> torch::Tensor {
    auto Msg1 = torch::mm(UseGraph1, StateToUseMsg1->forward(H_value));
    auto Msg2 = torch::mm(UseGraph2, StateToUseMsg2->forward(H_value));
    auto MemMsg = StateToMemMsg->forward(H_value);
    auto LeftMemMsg = torch::mm(LeftMemRefGraph, MemMsg);
    auto RightMemMsg = torch::mm(RightMemRefGraph, MemMsg);
    auto Independent =
        torch::mm(IndependenceGraph, StateToIndependentMsg(H_value));
    auto Unresolved =
        NumUnresolvedUses
            ? torch::mm(InvUnresolvedGraph, UnresolvedToMsg->forward(H_use))
            : Zeros;
    return torch::cat(
        {Msg1, Msg2, LeftMemMsg, RightMemMsg, Independent, Unresolved},
        1 /*dim*/);
  };

  for (unsigned i = 0; i < NumIters; i++) {
    if (NumUnresolvedUses)
      H_use = UseGRU->forward(SendToUses(H_value), H_use);
    H_value = ValueGRU->forward(SendToValues(H_value, H_use), H_value);
  }

  // Read out the probs in batch
  auto OpProb = StateToOpcode->forward(H_value);
  std::vector<torch::Tensor> LaneProbs;
  auto Emb = StateToEmb->forward(H_value);

  // Unpack the probs
  std::vector<PackDistribution> PDs;
  unsigned Offset = 0;
  for (auto &Index : Indexes) {
    unsigned N = Index.getNumValues();
    auto Slice = [Offset, N](torch::Tensor X) -> torch::Tensor {
      return X.slice(0 /*dim*/, Offset, Offset + N);
    };
    PackDistribution PD(std::move(Index));
    PD.OpProb = Slice(OpProb);
    for (auto &StateToLaneEmb : StateToLaneEmbs)
      PD.LaneProbs.push_back(StateToLaneEmb->forward(Slice(H_value))
                                 .mm(Slice(Emb).t())
                                 .softmax(1 /*dim*/));
    Offset += N;
    PDs.push_back(std::move(PD));
  }
  return PDs;
}

PackDistribution PackingModelImpl::forward(const Frontier *Frt, Packer *Pkr,
                                           torch::Device Device,
                                           unsigned NumIters) {
  std::vector<PackDistribution> PDs = batch_forward(Frt, Pkr, Device, NumIters);
  return std::move(PDs[0]);
}
