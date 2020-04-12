#include "IRModel.h"
#include "IRVec.h"
#include "Packer.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScopedNoAliasAA.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/TypeBasedAliasAnalysis.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GlobPattern.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static cl::opt<std::string>
    TrainDir(cl::Positional,
             cl::desc("Specify a train directory of bitcode files"),
             cl::value_desc("train directory"));

namespace llvm {
void initializePackerBuilderPass(PassRegistry &);
}

namespace {
// An nop pass we run to collect Packers, which requires many other analyses
class PackerBuilder : public FunctionPass {
public:
  static std::vector<std::unique_ptr<Packer>> Packers;
  static char ID; // Pass identification, replacement for typeid
  PackerBuilder() : FunctionPass(ID) {
    initializePackerBuilderPass(*PassRegistry::getPassRegistry());
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<AAResultsWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
    AU.addRequired<BlockFrequencyInfoWrapperPass>();
  }
  bool runOnFunction(llvm::Function &) override;
};

} // namespace

std::vector<std::unique_ptr<Packer>> PackerBuilder::Packers = {};

static IRInstTable VecBindingTable;

bool PackerBuilder::runOnFunction(llvm::Function &F) {
  auto *AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
  auto *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  auto *TTI = &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
  auto *BFI = &getAnalysis<BlockFrequencyInfoWrapperPass>().getBFI();

  auto *DL = &F.getParent()->getDataLayout();

  // FIXME: make the list supported insts a parameter
  Packers.push_back(std::make_unique<Packer>(VecBindingTable.getBindings(), F,
                                             AA, DL, SE, TTI, BFI));
  return false;
}

char PackerBuilder::ID = 0;

INITIALIZE_PASS_BEGIN(PackerBuilder, "pic", "pic", false, false)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(BlockFrequencyInfoWrapperPass)
INITIALIZE_PASS_END(PackerBuilder, "pic", "pic", false, false)

static float trainOnPacker(PackModel &Model, Packer &Packer,
                           std::vector<torch::Tensor> &Losses,
                           int SamplesPerInst = 4) {
  auto PackDistr = Packer.runModel(Model);
  auto *F = Packer.getFunction();
  float TotalCost = 0;
  int NumSamples = 0;
  for (auto &I : make_range(inst_begin(*F), inst_end(*F))) {
    for (int i = 0; i < SamplesPerInst; i++) {
      VectorPackSet Packs(F);
      PackSample PS = Packer.samplePackForInst(&I, Packs, PackDistr);
      if (PS.VP)
        Packs.tryAdd(PS.VP);
      float Cost = Packer.evalSeedPacks(Packs, 4);
      TotalCost += Cost;
      Losses.push_back(PS.LogProb * Cost);
      NumSamples += SamplesPerInst;
    }
  }

  return TotalCost;
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);

  std::error_code EC;
  ExitOnError ExitOnErr("Error");
  auto CheckError = [&]() { ExitOnErr(errorCodeToError(EC)); };

  sys::fs::directory_iterator DirEnd;
  sys::fs::directory_iterator DirIt(TrainDir, EC);
  CheckError();

  Expected<GlobPattern> BCPatOrErr = GlobPattern::create("*.bc");
  if (BCPatOrErr)
    ExitOnErr(BCPatOrErr.takeError());
  auto &BCPat = BCPatOrErr.get();

  LLVMContext Ctx;

  std::vector<std::unique_ptr<Module>> Modules;
  for (;;) {
    if (DirIt == DirEnd)
      break;

    std::string FilePath = DirIt->path();
    if (BCPat.match(FilePath)) {
      SMDiagnostic Diag;
      std::unique_ptr<Module> M = parseIRFile(FilePath, Diag, Ctx);
      if (!M)
        Diag.print("Trainer failed to load bitcode:", errs());
      else {
        dbgs() << "Parsed module: " << FilePath << '\n';
        Modules.push_back(std::move(M));
      }
    }

    DirIt.increment(EC);
    CheckError();
  }

  // Add the alias analysis pipeline
  legacy::PassManager Passes;
  Passes.add(createTypeBasedAAWrapperPass());
  Passes.add(createScopedNoAliasAAWrapperPass());
  Passes.add(createGlobalsAAWrapperPass());
  Passes.add(createBasicAAWrapperPass());
  Passes.add(new PackerBuilder());

  PackModel Model(32, VecBindingTable.getBindings());
  torch::optim::Adam Optimizer(Model->parameters(),
                               torch::optim::AdamOptions(1e-3));
  Optimizer.zero_grad();

  for (auto &M : Modules)
    Passes.run(*M);

  errs() << "Num packers: " << PackerBuilder::Packers.size() << '\n';
  errs() << "Num vector insts: " << VecBindingTable.getBindings().size() << '\n';

  int NumEpochs = 100;

  for (int Epoch = 0; Epoch < NumEpochs; Epoch++) {
    float EpochCost = 0;
    std::vector<torch::Tensor> Losses;
    for (std::unique_ptr<Packer> &Packer : PackerBuilder::Packers) {
      float AvgCost = trainOnPacker(Model, *Packer, Losses);
      errs() << "AvgCost: " << AvgCost << '\n';
      EpochCost += AvgCost;

    }
    errs() << "EPOCH COST: " << EpochCost / (float)Losses.size() << '\n';
    Optimizer.zero_grad();
    auto Loss = torch::stack(Losses).mean();
    Loss.backward();
    Optimizer.step();
  }
  return 0;
}
