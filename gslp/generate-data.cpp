#include "IRModel.h"
#include "IRVec.h"
#include "Packer.h"
#include "Policy.h"
#include "Serialize.h"
#include "Solver.h"
#include "SupervisionGenerator.h"
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
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static cl::opt<std::string>
    TrainDir(cl::Positional,
             cl::desc("Specify a train directory of bitcode files"),
             cl::value_desc("train directory"), cl::Required);

static cl::opt<std::string>
    ModelPath("model", cl::desc("Specify a file path to the model"),
              cl::value_desc("model path"), cl::init(""));

static cl::opt<unsigned>
    EmbeddingSize("emb-size", cl::desc("Specify size of the embedding"),
                  cl::value_desc("model embedding sizes"), cl::init(32));

static cl::opt<unsigned> MaxNumLanes(
    "max-num-lanes",
    cl::desc(
        "Specify maximum number of lanes the vectorizer is allowed to pack"),
    cl::value_desc("Maximum number of lanes"), cl::init(8));

static cl::opt<std::string> ArchivePath(
    "o",
    cl::desc(
        "Specify the output directory where will archive the sampled data"),
    cl::value_desc("Output directory"), cl::init("decision-archive"));

static cl::opt<unsigned> ArchiveBlockSize("archive-block-size",
                                          cl::value_desc("Archive block size"),
                                          cl::init(50));

static cl::opt<unsigned> ParamC("c",
                                cl::desc("Specify the exploration factor (C)"),
                                cl::value_desc("C"), cl::init(0.25));

static cl::opt<unsigned>
    ParamW("w", cl::desc("Specify the bias factor for the policy network (W)"),
           cl::value_desc("W"), cl::init(100));

static cl::opt<unsigned> SamplePerBlock(
    "samples",
    cl::desc("Specify the number of decisions we sample when dumping "
             "optimizing decisions of a basic block"),
    cl::value_desc("Number of decisions to sample per basic block"),
    cl::init(20));

static cl::opt<unsigned>
    NumSimulations("simulations", cl::value_desc("Number of MCTS simulations"),
                   cl::init(5000));

static cl::opt<unsigned> MaxSearchDist(
    "max-search-dist",
    cl::value_desc("Maximum distatance between two packable instructions"),
    cl::init(50));

static cl::opt<unsigned>
    NumThreads("threads", cl::desc("Number of threads to use"), cl::init(4));

namespace llvm {
void initializeGeneratorWrapperPass(PassRegistry &);
}

namespace {
// An nop pass we run to collect Packers, which requires many other analyses
class GeneratorWrapper : public FunctionPass {
public:
  static char ID; // Pass identification, replacement for typeid
  static std::unique_ptr<SupervisionGenerator> SG;
  static std::unique_ptr<ThreadPool> Threads;

  // Keep track of number of functions we processed
  static std::mutex Lock;
  static std::condition_variable Cond;
  static unsigned NumFuncs;

  GeneratorWrapper() : FunctionPass(ID) {
    initializeGeneratorWrapperPass(*PassRegistry::getPassRegistry());
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

static IRInstTable VecBindingTable;

static void runGeneratorOnFunction(SupervisionGenerator *SG, Function *F,
                                   Packer *Pkr) {
  for (auto &BB : *F)
    SG->run(nullptr /*policy*/, Pkr, &BB);
  delete Pkr;
}

bool GeneratorWrapper::runOnFunction(llvm::Function &F) {
  auto *AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
  auto *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  auto *TTI = &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
  auto *BFI = &getAnalysis<BlockFrequencyInfoWrapperPass>().getBFI();

  auto *DL = &F.getParent()->getDataLayout();

  // FIXME: make the list supported insts a parameter
  // runGenerator(SG.get(), &F,
  //    std::make_unique<Packer>(
  //      VecBindingTable.getBindings(), F, AA, DL, SE, TTI, BFI));
  Threads->async(
      runGeneratorOnFunction, SG.get(), &F,
      new Packer(VecBindingTable.getBindings(), F, AA, DL, SE, TTI, BFI));
  return false;
}

std::unique_ptr<SupervisionGenerator> GeneratorWrapper::SG;
std::unique_ptr<ThreadPool> GeneratorWrapper::Threads;

char GeneratorWrapper::ID = 0;

INITIALIZE_PASS_BEGIN(GeneratorWrapper, "pic", "pic", false, false)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(BlockFrequencyInfoWrapperPass)
INITIALIZE_PASS_END(GeneratorWrapper, "pic", "pic", false, false)

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);

  errs() << "Num vector insts: " << VecBindingTable.getBindings().size()
    << '\n';

  std::error_code EC;
  ExitOnError ExitOnErr("Error: ");
  auto CheckError = [&]() { ExitOnErr(errorCodeToError(EC)); };

  torch::NoGradGuard Guard;

  EC = sys::fs::create_directory(ArchivePath);
  CheckError();

  PackingModel Model(EmbeddingSize, VecBindingTable.getBindings(), MaxNumLanes);

  torch::Device Device(torch::kCPU);
  if (torch::cuda::is_available())
    Device = torch::Device(torch::kCUDA);

  Model->to(Device);
  Model->eval();

  PolicyArchiver Archiver(ArchiveBlockSize, ArchivePath);
  RolloutEvaluator Evaluator;
  GeneratorWrapper::SG.reset(
      new SupervisionGenerator(Archiver, &Evaluator, Model, MaxSearchDist,
        SamplePerBlock, ParamC, ParamW, NumSimulations));
  GeneratorWrapper::Threads.reset(
      new ThreadPool(hardware_concurrency(NumThreads)));

  // Add the alias analysis pipeline
  legacy::PassManager Passes;
  Passes.add(createTypeBasedAAWrapperPass());
  Passes.add(createScopedNoAliasAAWrapperPass());
  Passes.add(createGlobalsAAWrapperPass());
  Passes.add(createBasicAAWrapperPass());
  Passes.add(new GeneratorWrapper());

  sys::fs::directory_iterator DirEnd;
  sys::fs::directory_iterator DirIt(TrainDir, EC);
  CheckError();

  Expected<GlobPattern> BCPatOrErr = GlobPattern::create("*.bc");
  if (BCPatOrErr)
    ExitOnErr(BCPatOrErr.takeError());
  auto &BCPat = BCPatOrErr.get();

  LLVMContext Ctx;

  std::vector<std::string> ModulePaths;
  for (;;) {
    if (DirIt == DirEnd)
      break;

    std::string FilePath = DirIt->path();
    if (BCPat.match(FilePath))
      ModulePaths.push_back(FilePath);

    DirIt.increment(EC);
    CheckError();
  }


  unsigned i = 0;
  for (auto &FilePath : ModulePaths) {
    errs() << "\rProcessing module: " 
      << FilePath
      << ", " << i++ << "/" << ModulePaths.size();
    SMDiagnostic Diag;
    std::unique_ptr<Module> M = parseIRFile(FilePath, Diag, Ctx);
    if (!M)
      Diag.print("Trainer failed to load bitcode:", errs());
    else {
      // Set target feature. FIXME:
      // make it configurable depending on which machine we want to tune for
      for (auto &F : *M) {
        if (F.empty())
          continue;
        F.addFnAttr(
            "target-features",
            "+64bit,+adx,+aes,+avx,+avx2,+bmi,+bmi2,+clflushopt,+cmov,+cx16,+"
            "cx8,+f16c,+fma,+fsgsbase,+fxsr,+invpcid,+lzcnt,+mmx,+movbe,+"
            "pclmul,+popcnt,+prfchw,+rdrnd,+rdseed,+rtm,+sahf,+sgx,+sse,+"
            "sse2,+sse3,+sse4.1,+sse4.2,+ssse3,+x87,+xsave,+xsavec,+xsaveopt,"
            "+xsaves,-avx512bf16,-avx512bitalg,-avx512bw,-avx512cd,-avx512dq,"
            "-avx512er,-avx512f,-avx512ifma,-avx512pf,-avx512vbmi,-"
            "avx512vbmi2,-avx512vl,-avx512vnni,-avx512vp2intersect,-"
            "avx512vpopcntdq,-cldemote,-clwb,-clzero,-enqcmd,-fma4,-gfni,-"
            "lwp,-movdir64b,-movdiri,-mwaitx,-pconfig,-pku,-prefetchwt1,-"
            "ptwrite,-rdpid,-sha,-shstk,-sse4a,-tbm,-vaes,-vpclmulqdq,-"
            "waitpkg,-wbnoinvd,-xop");
        F.addFnAttr("target-cpu", "skylake");
      }
      Passes.run(*M);
      GeneratorWrapper::Threads->wait();
    }
  }
  errs() << '\n';
}
