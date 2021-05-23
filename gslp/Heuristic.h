#ifndef HEURISTIC_H
#define HEURISTIC_H

#include "llvm/ADT/DenseMap.h"

class Packer;
class VectorPack;
class VectorPackContext;
class OperandPack;
class CandidatePackSet;

namespace llvm {
class Value;
class Instruction;
}

class Heuristic {
  llvm::DenseMap<const OperandPack *, float> OrderedCosts;
  llvm::DenseMap<llvm::Instruction *, float> ScalarCosts;

  Packer *Pkr;
  const VectorPackContext *VPCtx;
  const CandidatePackSet *Candidates;

  float getCost(const VectorPack *VP);
public:
  Heuristic(Packer *Pkr, const VectorPackContext *VPCtx,
            const CandidatePackSet *Candidates)
      : Pkr(Pkr), VPCtx(VPCtx), Candidates(Candidates) {}
  //float getCost(const Frontier *);
  float getCost(const OperandPack *OP);
  float getCost(llvm::Value *);
};

#endif // HEURISTIC_H
