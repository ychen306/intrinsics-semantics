#include "GraphUtil.h"
#include "IRModel.h"
#include "IRVec.h"
#include "ModelUtil.h"
#include "Serialize.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GlobPattern.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include <torch/torch.h>

using namespace llvm;

static cl::opt<std::string> InputFileName(cl::Positional,
                                          cl::value_desc("Input file name"));

static cl::opt<unsigned>
    MaxNumLanes("max-num-lanes",
                cl::value_desc("Max number of lanes in a vector"), cl::init(8));

static cl::opt<unsigned> BatchSize("batch-size", cl::value_desc("Batch size"),
                                   cl::init(32));

static cl::opt<unsigned>
    NumWorkers("num-workers", cl::value_desc("Number of data-loader workers"),
               cl::init(1));

static cl::opt<unsigned>
    EmbSize("emb-size", cl::value_desc("Size of embedding"), cl::init(64));

static cl::opt<unsigned> MsgPassingIters(
    "msg-passing-iters",
    cl::value_desc("Number of iterations we do message passing"), cl::init(8));

namespace {

class PackingDataset
    : public torch::data::Dataset<PackingDataset, const PolicySupervision *> {
  std::vector<PolicySupervision> Supervisions;

public:
  PackingDataset(PolicyReader &Reader);
  const PolicySupervision *get(size_t i) override { return &Supervisions[i]; }
  c10::optional<size_t> size() const override { return Supervisions.size(); }
};

// Add a util func that allows us to take a whole array of edges
struct GraphBatcher : public BatchedGraphBuilder {
  void addGraph(llvm::ArrayRef<DiEdge> NewEdges, unsigned N, unsigned M);
};

} // end anonymous namespace

PackingDataset::PackingDataset(PolicyReader &Reader) {
  PolicySupervision PS;
  while (Reader.read(PS))
    Supervisions.push_back(std::move(PS));
}

void GraphBatcher::addGraph(llvm::ArrayRef<DiEdge> NewEdges, unsigned N,
                            unsigned M) {
  for (auto &E : NewEdges)
    addEdge(E.Src, E.Dest);
  finishBatch(N, M);
}

static std::pair<BatchedFrontier, std::vector<const PolicySupervision *>>
batch(std::vector<const PolicySupervision *> Supervisions) {
  GraphBatcher Use1;
  GraphBatcher Use2;
  GraphBatcher MemRef;
  GraphBatcher RightMemRef;
  GraphBatcher Independence;
  GraphBatcher InvUnresolved;
  std::vector<GraphBatcher> Unresolved(MaxNumLanes);
  std::vector<int64_t> ValueTypes;

  BatchedFrontier Batched;

  unsigned NumValues = 0;
  unsigned NumUses = 0;
  // Here we go.
  for (auto *PS : Supervisions) {
    const ProcessedFrontier &Frt = PS->Frt;
    unsigned N = Frt.NumValues, M = Frt.NumUses;
    Batched.NumValues.push_back(N);
    Batched.NumUses.push_back(M);
    NumValues += N;
    NumUses += M;

    Use1.addGraph(Frt.Use1, N, N);
    Use2.addGraph(Frt.Use2, N, N);
    MemRef.addGraph(Frt.MemRefs, N, N);
    Independence.addGraph(Frt.Independence, N, N);
    InvUnresolved.addGraph(Frt.InvUnresolved, N, M);
    for (unsigned i = 0; i < MaxNumLanes; i++)
      Unresolved[i].addGraph(Frt.Unresolved[i], M, N);

    ValueTypes.insert(ValueTypes.end(), Frt.ValueTypes.begin(),
                      Frt.ValueTypes.end());
  }

  auto ValueTypeTensor =
      torch::from_blob(ValueTypes.data(), {(int64_t)ValueTypes.size()},
                       torch::TensorOptions().dtype(torch::kInt64))
          .clone();

  Batched.TotalValues = NumValues;
  Batched.TotalUses = NumUses;
  Batched.Use1 = Use1.getBatched();
  Batched.Use2 = Use2.getBatched();
  Batched.LeftMemRef = MemRef.getBatched();
  Batched.RightMemRef = MemRef.getBatched(true /*flip edges*/);
  Batched.Independence = Independence.getBatched();
  Batched.InvUnresolved = InvUnresolved.getBatched();
  Batched.ValueTypes = ValueTypeTensor;
  for (unsigned i = 0; i < MaxNumLanes; i++)
    Batched.Unresolved.push_back(Unresolved[i].getBatched());

  return {std::move(Batched), std::move(Supervisions)};
}

template <typename OutStreamTy>
void dumpShape(torch::Tensor X, OutStreamTy &Os) {
  for (auto N : X.sizes()) {
    Os << " " << N;
  }
  Os << '\n';
}

static torch::Tensor computeProb(PackingModel Model, const PackDistribution &PD,
                                 const PolicySupervision *S) {
  std::vector<torch::Tensor> Prob;
  auto &Frt = S->Frt;
  unsigned FocusId = Frt.FocusId;
  for (const auto &Pack : S->Packs) {
    if (Pack.K == ProcessedVectorPack::Scalar) {
      // Scalar (i.e., nop).
      Prob.push_back(PD.OpProb[FocusId][Model->getNopId()]);
    } else {
      // T involves an actual pack.
      // We pretend we can sample the opcode and lanes independently
      auto PackProb = PD.OpProb[FocusId][Pack.InstId];
      unsigned i = 0;
      for (uint64_t j : Pack.Lanes)
        PackProb *= PD.LaneProbs[i++][FocusId][j];
      Prob.push_back(PackProb);
    }
  }
  auto Predicted = torch::stack(Prob);
  return Predicted / Predicted.sum();
}

static std::vector<torch::Tensor>
computeProbInBatch(PackingModel Model, torch::Device Device,
                   llvm::ArrayRef<PackDistribution> PDs,
                   llvm::ArrayRef<const PolicySupervision *> Supervisions) {
  BatchPackProbability BPP(MaxNumLanes, Device);
  for (unsigned i = 0; i < PDs.size(); i++) {
    const auto &PD = PDs[i];
    const auto &Frt = Supervisions[i]->Frt;

    BPP.start(PD, Frt.FocusId);

    for (auto &Pack : Supervisions[i]->Packs) {
      unsigned OpId;
      ;
      if (Pack.K == ProcessedVectorPack::Scalar)
        OpId = Model->getNopId();
      else
        OpId = Pack.InstId;
      BPP.addPack(OpId, Pack.Lanes);
    }

    BPP.finish();
  }
  return BPP.get();
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);

  int FD;
  ExitOnError ExitOnErr("Error: ");
  std::error_code EC = sys::fs::openFileForRead(InputFileName, FD);
  ExitOnErr(errorCodeToError(EC));

  PolicyReader Reader(FD);
  PackingDataset Dataset(Reader);

  // What a beautiful piece of code.
  using TransformTy = torch::data::transforms::BatchLambda<
      std::vector<const PolicySupervision *>,
      std::pair<BatchedFrontier, std::vector<const PolicySupervision *>>>;
  auto DataLoaderOpt =
      torch::data::DataLoaderOptions().batch_size(BatchSize).workers(
          NumWorkers);
  auto DataLoader =
      torch::data::make_data_loader<torch::data::samplers::RandomSampler>(
          Dataset.map(TransformTy(batch)), DataLoaderOpt);

  // FIXME:
  // make the number of instruction a configurable thing (w/ a config file?)
  // and allow constructing a model w/ just the number of instruction w/o
  // telling it what the instructions are.
  static IRInstTable VecBindingTable;
  PackingModel Model(EmbSize, VecBindingTable.getBindings(), MaxNumLanes);
  torch::Device Device(torch::kCPU);
  if (torch::cuda::is_available())
    Device = torch::Device(torch::kCUDA);

  Model->to(Device);

  for (auto &Batch : (*DataLoader)) {
    errs() << "!!!\n";
    auto &Frt = Batch.first;
    auto &Sup = Batch.second;
    std::vector<PackDistribution> PDs = Model->batch_forward(
        Frt, Device, None /* We don't have IR indexes */, MsgPassingIters);

    auto Probs = computeProbInBatch(Model, Device, PDs, Sup);
    std::vector<torch::Tensor> Losses;
    for (unsigned i = 0; i < PDs.size(); i++) {
      auto Target =
          torch::from_blob(const_cast<float *>(Sup[i]->Prob.data()),
                           {(int64_t)Sup[i]->Prob.size()},
                           torch::TensorOptions().dtype(torch::kFloat32));
      auto Predicted = Probs[i];
      auto Loss = -Target.dot(Predicted.log());
      std::cerr << Loss << '\n';
      Losses.push_back(Loss);
    }
    torch::stack(Losses).sum().backward();
  }
}