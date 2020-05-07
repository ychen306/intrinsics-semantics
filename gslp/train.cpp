#include "IRModel.h"
#include "IRVec.h"
#include "Packer.h"
#include "Policy.h"
#include "Solver.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScopedNoAliasAA.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/TypeBasedAliasAnalysis.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GlobPattern.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static cl::opt<std::string>
    TrainDir(cl::Positional,
             cl::desc("Specify a train directory of bitcode files"),
             cl::value_desc("train directory"));

static cl::opt<std::string>
    OutModelPath("o", cl::desc("Specify a file path for the trained model"),
                 cl::value_desc("output model path"), cl::init("pack.pt"));

static cl::opt<std::string>
    InitModelPath("init",
                  cl::desc("Specify a file path to initialize the model"),
                  cl::value_desc("init model path"), cl::init(""));

static cl::opt<unsigned>
    EmbeddingSize("emb-size", cl::desc("Specify size of the embedding"),
                  cl::value_desc("model embedding sizes"), cl::init(32));

static cl::opt<float> LearningRate("lr", cl::desc("Specify learning rate"),
                                   cl::value_desc("learning rate"),
                                   cl::init(1e-3));

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
        // Set target feature. FIXME:
        // make it configurable depending on which machine we want to tune for
        for (auto &F : *M) {
          if (F.empty())
            continue;
          // F.addFnAttr(
          //"target-features",
          //"+64bit,+adx,+aes,+avx,+avx2,+bmi,+bmi2,+clflushopt,+cmov,+cx16,+cx8,+f16c,+fma,+fsgsbase,+fxsr,+invpcid,+lzcnt,+mmx,+movbe,+pclmul,+popcnt,+prfchw,+rdrnd,+rdseed,+rtm,+sahf,+sgx,+sse,+sse2,+sse3,+sse4.1,+sse4.2,+ssse3,+x87,+xsave,+xsavec,+xsaveopt,+xsaves,-avx512bf16,-avx512bitalg,-avx512bw,-avx512cd,-avx512dq,-avx512er,-avx512f,-avx512ifma,-avx512pf,-avx512vbmi,-avx512vbmi2,-avx512vl,-avx512vnni,-avx512vp2intersect,-avx512vpopcntdq,-cldemote,-clwb,-clzero,-enqcmd,-fma4,-gfni,-lwp,-movdir64b,-movdiri,-mwaitx,-pconfig,-pku,-prefetchwt1,-ptwrite,-rdpid,-sha,-shstk,-sse4a,-tbm,-vaes,-vpclmulqdq,-waitpkg,-wbnoinvd,-xop");
          // F.addFnAttr("target-cpu", "skylake");
        }
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

  PackingModel Model(EmbeddingSize, VecBindingTable.getBindings(), 8);
  // if (!InitModelPath.empty()) {
  //  errs() << "Initializing model using " << InitModelPath << '\n';
  //  loadModel(Model, InitModelPath);
  //}

  torch::Device Device(torch::kCPU);
  if (torch::cuda::is_available())
    Device = torch::Device(torch::kCUDA);

  Model->to(Device);

  torch::optim::Adam Optimizer(Model->parameters(),
                               torch::optim::AdamOptions(LearningRate));
  Optimizer.zero_grad();

  for (auto &M : Modules)
    Passes.run(*M);

  errs() << "Num packers: " << PackerBuilder::Packers.size() << '\n';
  errs() << "Num vector insts: " << VecBindingTable.getBindings().size()
         << '\n';

  int NumEpochs = 100;

  RolloutEvaluator Evaluator;

  for (int Epoch = 0; Epoch < NumEpochs; Epoch++) {
    for (auto &Pkr : PackerBuilder::Packers) {
      if (Pkr->getFunction()->getName() != "binvcrhs")
        continue;
      torch::NoGradGuard Guard;
      Model->eval();
      for (auto &BB : *Pkr->getFunction()) {
        UCTNodeFactory Factory;
        UCTNode *Root = Factory.getNode(
            std::make_unique<Frontier>(&BB, Pkr->getContext(&BB)));
        NeuralPackingPolicy Policy(Model, Pkr.get(),
                                   8 /*iters of message passing*/, Device,
                                   8 /*batch size*/, 1 /*num threads*/);
        {
          UCTSearch MCTS(100 /*exploration factor*/,
                         100 /*how much we trust the policy*/, &Factory,
                         Pkr.get(), &Policy, &Evaluator, Pkr->getTTI());
          Timer T("mcts", "time takes to run 10 iter of MCTS");

          // Frontier Frt(&BB, Pkr->getContext(&BB));

          T.startTimer();

          MCTS.run(Root, 1000);
          errs() << "!!! search done\n";
          // std::vector<const Frontier *> Frts(128, &Frt);
          // for (unsigned i = 0; i < 10; i++)
          //  Model->forward(&Frt, Pkr.get(), Device, 8);
          // Model->batch_forward(Frts, Pkr.get(), Device, 8);

          T.stopTimer();

          auto Elapsed = T.getTotalTime();
          Elapsed.print(Elapsed, errs());
          Policy.waitForInflight();
        }
        return 0;
      }
    }
  }
  return 0;
}
