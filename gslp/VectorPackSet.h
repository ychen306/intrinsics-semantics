#ifndef VECTOR_PACK_SET_H
#define VECTOR_PACK_SET_H

/*
 * VectorPackSet manages a set of compatible vector packs and
 * is responsible for lowering a set of packs to LLVM IR.
 */

#include "VectorPack.h"

class LocalDependenceAnalysis;
class Packer;

class VectorPackSet {
  llvm::Function *F;
  std::vector<const VectorPack *> AllPacks;
  // FIXME : rename Packs to BB2Packs;
  llvm::DenseMap<llvm::BasicBlock *, std::vector<const VectorPack *>> Packs;
  llvm::DenseMap<llvm::BasicBlock *, llvm::BitVector> PackedValues;
  llvm::DenseMap<llvm::Value *, const VectorPack *> ValueToPackMap;

  // This tells us where a value is located in a pack
  struct VectorPackIndex {
    const VectorPack *VP;
    unsigned Idx;

    bool operator<(const VectorPackIndex &Other) const {
      return std::tie(VP, Idx) < std::tie(Other.VP, Other.Idx);
    }
    bool operator==(const VectorPackIndex &Other) const {
      return VP == Other.VP && Idx == Other.Idx;
    }
  };

  // Mapping a value to the pack that produce them.
  using ValueIndexTy = llvm::DenseMap<const llvm::Value *, VectorPackIndex>;
  // Mapping VectorPack -> their materialized values.
  using PackToValueTy = llvm::DenseMap<const VectorPack *, llvm::Value *>;

  // Get the vector value representing OpndPack.
  static llvm::Value *gatherOperandPack(const OperandPack &OpndPack,
                                        const ValueIndexTy &ValueIndex,
                                        const PackToValueTy &MaterializedPacks,
                                        IntrinsicBuilder &Builder);
  void add(const VectorPack *VP);

public:
  VectorPackSet(llvm::Function *F) : F(F) {}
  VectorPackSet(const VectorPackSet &Other) = default;
  VectorPackSet &operator=(const VectorPackSet &Other) = default;

  bool isCompatibleWith(const VectorPack &VP) const;

  // Add VP to this set if it doesn't conflict with existing packs.
  // return if successful
  bool tryAdd(const VectorPack *VP);

  // Generate vector code from the packs
  void codegen(IntrinsicBuilder &Builder, Packer &Pkr);
};

#endif // end VECTOR_PACK_SET_H
