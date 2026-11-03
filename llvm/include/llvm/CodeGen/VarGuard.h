#ifndef LLVM_CODEGEN_VARGUARD_H
#define LLVM_CODEGEN_VARGUARD_H
// header guard, this prevents the header from nbeing included twice
// in  the same compiled file
// 'if LLVM_CODE... is not defined,
// define it and include this file's contents

#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

// everyting inside belong to LLVM's namespace
// so the real fullnames are llvm::....
namespace llvm {

//  This is saying that these classes, exist, but i am not defining them here
//  to avoid including more heavy headers,
class BasicBlock;
class Function;
class Module;
class TargetLoweringBase;
class TargetMachine;

class VarGuardLayoutInfo {

  // VarGuardLayoutInfo
  friend class VarGuardPass;
  friend class VarGuardLayoutAnalysis;
  friend class VarGuard;

  static constexpr unsigned DefaultVGBufferSize = 8;

  using VarGuardLayoutMap =
      DenseMap<const AllocaInst *, MachineFrameInfo::SSPLayoutKind>;

  VarGuardLayoutMap Layout;

  unsigned VarGuardBufferSize = DefaultVGBufferSize;

  bool RequireVarGuard = false;

  bool HasPrologue = false;

  bool HasIRCheck = false;

public:
  void copyToVGMachineFrameInfo(MachineFrameInfo &MFI) const;
};

class VarGuardLayoutAnalysis
    : public AnalysisInfoMixin<VarGuardLayoutAnalysis> {
  friend AnalysisInfoMixin<VarGuardLayoutAnalysis>;

  using VarGuardLayoutMap = VarGuardLayoutInfo::VarGuardLayoutMap;

  static AnalysisKey Key;

public:
  using Result = VarGuardLayoutInfo;
  Result run(Function &F, FunctionAnalysisManager &FAM);

  static bool requiresVarGuard(Function *F,
                               VarGuardLayoutMap *Layout = nullptr);
};

class VarGuardPass : public RequiredPassInfoMixin<VarGuardPass> {
  const TargetMachine *TM;

public:
  explicit VarGuardPass(const TargetMachine &TM) : TM(&TM) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

} // end namespace llvm

#endif // LLVM_CODEGEN_VARGUARD_H
