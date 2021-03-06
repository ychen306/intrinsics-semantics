#ifndef MATCH_MANAGER_H
#define MATCH_MANAGER_H

#include "InstSema.h"
#include "llvm/IR/BasicBlock.h"
#include <algorithm>

// This class pulls all operation that we are interested in
// and tries to match all of them while trying to avoid
// matching the same operation twice on the same value
class MatchManager {
  // record matches for each operation
  llvm::DenseMap<const Operation *, std::vector<Operation::Match>> OpMatches;

  void match(llvm::Value *V);

  static bool sortByOutput(const Operation::Match &A,
                           const Operation::Match &B);

public:
  MatchManager(llvm::ArrayRef<const InstBinding *> Insts, llvm::BasicBlock &BB);

  llvm::ArrayRef<Operation::Match> getMatches(const Operation *Op) const;

  llvm::ArrayRef<Operation::Match>
  getMatchesForOutput(const Operation *Op, llvm::Value *Output) const;
};

void getIntermediateInsts(const Operation::Match &,
                          llvm::SmallPtrSetImpl<llvm::Instruction *> &);

#endif // end MATCH_MANAGER_H
