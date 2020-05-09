#ifndef POLICY_H
#define POLICY_H
#include "IRModel.h"
#include "Solver.h"
#include "llvm/Support/thread.h"

class Packer;

class NeuralPackingPolicy : public PackingPolicy {
  PackingModel Model;
  Packer *Pkr;
  unsigned NumIters;
  torch::Device Device;
  unsigned BatchSize;

  std::condition_variable QueueCond;
  std::mutex QueueLock;
  std::queue<std::vector<UCTNode *>> Queue;
  std::vector<llvm::thread> Threads;

  std::condition_variable IdlingCond;
  std::mutex IdlingLock;
  unsigned NumIdlingThreads;

  std::atomic<bool> Shutdown;

  // Worklist of nodes we want to evaluate.
  std::vector<UCTNode *> Nodes;

  void evalNodes();

public:
  NeuralPackingPolicy(PackingModel Model, Packer *Pkr, unsigned NumIters,
                      torch::Device Device, unsigned BatchSize = 128,
                      unsigned NumThreads = 1)
      : PackingPolicy(Model->getMaxNumLanes()), Model(Model), Pkr(Pkr),
        NumIters(NumIters), Device(Device), BatchSize(BatchSize), NumIdlingThreads(NumThreads) {
    Nodes.reserve(BatchSize);
    for (unsigned i = 0; i < NumThreads; i++)
      Threads.emplace_back([this]() { evalNodes(); });
    Shutdown = false;
  }
  ~NeuralPackingPolicy();
  void predictAsync(UCTNode *) override;
  void predict(UCTNode *, std::vector<float> &) override;
  void cancel() override;
};

#endif // end POLICY_H
