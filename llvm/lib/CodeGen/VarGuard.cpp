#include "llvm/CodeGen/VarGuard.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/EHPersonalities.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/User.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <optional>

using namespace llvm;

// when llvm debug/stat/remark systems need this pass's name,
// use 'var-guard'
#define DEBUG_TYPE "var-guard" // defines the preprocessor macro

// NumFunProtected counts protected functions
// NumAddrTaken counts address-taken locals
// NumVarProtected counts protected variables
STATISTIC(NumFunProtected, "Number of functions protected");
STATISTIC(NumAddrTaken,
          "Number of local variables that have their address taken");
// STATISTIC(NumVarProtected, "Number of variables protected");

using VarGuardSlotMap =
    DenseMap<Instruction *, AllocaInst *>;

// static: visible only in this .cpp file
// cl::opt<boo> llvm command-line option of type bool
// "enable-selectiondag-sp" the command-line flag name
// cl::Hidden -> do not show in normal help output
// So it creates a hidden llvm option:
static cl::opt<bool>
    EnableVarGuardSelectionDAGSP("enable-varguard-selectiondag-sp",
                                 cl::init(true), cl::Hidden);
// creates another hidden options controlling whether stack checks are inserted
// before certain noreturn calls - what are noreturn calls?
static cl::opt<bool>
    DisableVarGuardCheckNoReturn("disable-varguard-check-noreturn-call",
                                 cl::init(false), cl::Hidden);

// forward declarations of helper functions
// These inform C++ that these functins exist later in this file.
// You can call them before their full body appears
// static here means: This helper is local to this .cpp file.
// Other files cannot call it directly.
static VarGuardSlotMap InsertVarGuards(const TargetLowering &TLI,
									const LibcallLoweringInfo &Libcalls,
									Function *F, DomTreeUpdater *DTU,
									bool &HasPrologue, bool &HasIRCheck);
static BasicBlock *CreateVarGuardFailBB(Function *F, const LibcallLoweringInfo &Libcalls);
static bool InsertChecks(const TargetLowering &TLI, 
		const LibcallLoweringInfo &Libcalls, Function *F,
		DomTreeUpdater *DTU, VarGuardSlotMap slotMap);
static bool VisitUsesAndInsertChecks(const TargetLowering &TLI,
		const LibcallLoweringInfo &Libcalls, Function *F,
		DomTreeUpdater *DTU, Instruction *Ins, AllocaInst *VGslot, BasicBlock *&fb);

// should the backend / selectionDag emit a stack protector check for this block?
// it returns true only if all thre conditions are true
// if a stack protector prolgue was created, no ir-level check was already inserted and
// this basic block ends with a ret instruction.
// VarGuardLayoutInfo::shouldEmitVGCheck - thsi defines a method that was
// declared inside class VarGuardLayoutInfo
// :: mean this functin belongs to VarGuardLayoutInfo
// const at the end means thsi method will not modify the VarGuardLayoutInfo
// object There are two possible places to emit the stack protector check:
// IR pass emits it now or backend/selectiondag emits it later
bool VarGuardLayoutInfo::shouldEmitVGCheck(const BasicBlock &BB) const {
  return HasPrologue && !HasIRCheck && isa<ReturnInst>(BB.getTerminator());
}
/*
 * copies the stack-protector layout decisions from IR-level analyis into
 * backend stack-frame objects. IR alloca -> backend frame index for each
 * backend stack object, it asks: did this stack object come from an aloca that
 * VarGuardLayoutAnalysis marked? if yes, it stores the stack-protector category
 * into MachineFrameInfo
 */
void VarGuardLayoutInfo::copyToVGMachineFrameInfo(MachineFrameInfo &MFI) const {
  if (Layout.empty())
    return;

  for (int I = 0, E = MFI.getObjectIndexEnd(); I != E; ++I) {
    if (MFI.isDeadObjectIndex(I))
      continue;

    const AllocaInst *AI = MFI.getObjectAllocation(I);
    if (!AI)
      continue;

    VarGuardLayoutMap::const_iterator LI = Layout.find(AI);
    if (LI == Layout.end())
      continue;

    MFI.setObjectSSPLayout(I, LI->second);
  }
}

VarGuardLayoutInfo VarGuardLayoutAnalysis::run(Function &F,
                                               FunctionAnalysisManager &FAM) {
  VarGuardLayoutInfo Info;
  Info.RequireVarGuard =
      VarGuardLayoutAnalysis::requiresVarGuard(&F, &Info.Layout);
  Info.VarGuardBufferSize = F.getFnAttributeAsParsedInteger(
      "stack-protector-buffer-size", VarGuardLayoutInfo::DefaultVGBufferSize);
  return Info;
}

AnalysisKey VarGuardLayoutAnalysis::Key;

PreservedAnalyses VarGuardPass::run(Function &F, FunctionAnalysisManager &FAM) {

	/* VarGuardLayoutAnalysis first computes whethere the function needs stack protection and records layout infor before it is used.
	 * This asks the funcion analysis manager(FAM) give me the VarGuardLayoutAnalysis result or this function. [how does FAM have access to the SSP... function?]
	 */
	auto &Info = FAM.getResult<VarGuardLayoutAnalysis>(F);
	/* If a dominator tree has already been computed, give it to me.
	 * If not, do not compute it now. [what is a dominator tree?]
	 */
	auto *DT = FAM.getCachedResult<DominatorTreeAnalysis>(F);
	// This creates a helper that can update the dominator tree if the pass
	// changes to CFG
	// why? Stack protection insertion may split blocks or add branches, so the 
	// CFG can change --
	// wat? don't understand
	DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Lazy);

	// If analysis says this function does not need protection, 
	// do nothing and preseve everything.
	if (!Info.RequireVarGuard)
		return PreservedAnalyses::all();

	// --------skip---------
	// if this function uses funclet-based exception handling, 
	// skipp stack protector insertion.
	if (F.hasPersonalityFn()) {
		EHPersonality Personality = classifyEHPersonality(F.getPersonalityFn());
		if (isFuncletEHPersonality(Personality))
			return PreservedAnalyses::all();
	}
	// 
	// --------------------
	// we are in a function pass, but we're getting a module-level analysis
	// So LLVM gives you a proxy from: ? what is a proxy...?
	auto &MAMProxy = FAM.getResult<ModuleAnalysisManagerFunctionProxy>(F);

	// This asks for information about runtime library calls.
	// and for this task in question, LLVM may need calls like the __stack_chk_fail()
	// so it needs target/module-specific libcall lowering info
	
	const LibcallLoweringModuleAnalysisResult *LibcallLowering = MAMProxy.getCachedResult<LibcallLoweringModuleAnalysis>(*F.getParent());
	
	// If I can't find the libcall lowering info,
	// I cannot safely insert stack protector code.
	// what is the libcall info?
	
	if (!LibcallLowering) {
		F.getContext().emitError("'" + LibcallLoweringModuleAnalysis::name() + "' analysis required");
		return PreservedAnalyses::all();
	}
	// ask for the target machine
	// for this function, what subtarget am I compiling for?
	// ins't it a bit redundant to ask that for every function.
	const TargetSubtargetInfo *STI = TM->getSubtargetImpl(F);
	// This gets the object that knows how to lower target-specific operations.
	const TargetLowering *TLI = STI->getTargetLowering();

	// This gets the actual libcall rules for this subtarget
	const LibcallLoweringInfo &Libcalls = LibcallLowering->getLibcallLowering(*STI);
	
	// 
	++NumFunProtected;

	//bool changed = InsertVarGuards(*TLI, Libcalls, &F, DT ? &DTU : nullptr, Info.HasPrologue, Info.HasIRCheck);
	errs() << "calling insert varguards\n";
	VarGuardSlotMap slotMap = InsertVarGuards(*TLI, Libcalls, &F, DT ? &DTU : nullptr, Info.HasPrologue, Info.HasIRCheck);

	// write check 
	//
	//
	//
	// read check
	//InsertChecks(slotMap);
	errs() << "calling insert checks\n";
	InsertChecks(*TLI, Libcalls, &F, DT ? &DTU : nullptr, slotMap);

	
	

	PreservedAnalyses PA;
	//PA.preserve<VarGuardLayoutAnalysis>();
	PA.preserve<DominatorTreeAnalysis>();
	return PA;
}

/// \param [out] IsLarge is set to true if a protectable array is found and
/// it is "large" ( >= vg-buffer-size).  In the case of a structure with
/// multiple arrays, this gets set if any of them is large.
static bool containsGuardableArray(Type *Ty, Module *M,
                                   unsigned VarGuardBufferSize, bool &IsLarge,
                                   bool Strong, bool InStruct) {
  if (!Ty)
    return false;
  if (ArrayType *AT = dyn_cast<ArrayType>(Ty)) {
    if (!AT->getElementType()->isIntegerTy(8)) {
      // If we're on a non-Darwin platform or we're inside of a structure, don't
      // add stack protectors unless the array is a character array.
      // However, in strong mode any array, regardless of type and size,
      // triggers a protector.
      if (!Strong && (InStruct || !M->getTargetTriple().isOSDarwin()))
        return false;
    }

    // If an array has more than VarGuardBufferSize bytes of allocated space,
    // then we emit stack protectors.
    if (VarGuardBufferSize <= M->getDataLayout().getTypeAllocSize(AT)) {
      IsLarge = true;
      return true;
    }

    if (Strong)
      return true;
  }
  const StructType *ST = dyn_cast<StructType>(Ty);
  if (!ST)
    return false;

  bool NeedsProtector = false;
  for (Type *ET : ST->elements())
    if (containsGuardableArray(ET, M, VarGuardBufferSize, IsLarge, Strong,
                               true)) {
      // If the element is a protectable array and is large (>=
      // VarGuardBufferSize) then we are done.  If the protectable array is not
      // large, then keep looking in case a subsequent element is a large array.
      if (IsLarge)
        return true;
      NeedsProtector = true;
    }

  return NeedsProtector;
}

/// Maximum remaining allocation size observed for a phi node, and how often
/// the allocation size has already been decreased. We only allow a limited
/// number of decreases.
struct PhiInfo {
  TypeSize AllocSize;
  unsigned NumDecreased = 0;
  static constexpr unsigned MaxNumDecreased = 3;
  PhiInfo(TypeSize AllocSize) : AllocSize(AllocSize) {}
};
using PhiMap = SmallDenseMap<const PHINode *, PhiInfo, 16>;

/// Check whether a stack allocation has its address taken.
static bool hasAddressTaken(const Instruction *AI, TypeSize AllocSize,
                            Module *M, PhiMap &VisitedPHIs) {
  const DataLayout &DL = M->getDataLayout();
  for (const User *U : AI->users()) {
    const auto *I = cast<Instruction>(U);
    // If this instruction accesses memory make sure it doesn't access beyond
    // the bounds of the allocated object.
    std::optional<MemoryLocation> MemLoc = MemoryLocation::getOrNone(I);
    if (MemLoc && MemLoc->Size.hasValue() &&
        !TypeSize::isKnownGE(AllocSize, MemLoc->Size.getValue()))
      return true;
    switch (I->getOpcode()) {
    case Instruction::Store:
      if (AI == cast<StoreInst>(I)->getValueOperand())
        return true;
      break;

    case Instruction::AtomicCmpXchg:
      // cmpxchg conceptually includes both a load and store from the same
      // location. So, like store, the value being stored is what matters.
      if (AI == cast<AtomicCmpXchgInst>(I)->getNewValOperand())
        return true;
      break;
    case Instruction::AtomicRMW:
      if (AI == cast<AtomicRMWInst>(I)->getValOperand())
        return true;
      break;
    case Instruction::PtrToInt:
      if (AI == cast<PtrToIntInst>(I)->getOperand(0))
        return true;
      break;
    case Instruction::Call: {
      // Ignore intrinsics that do not become real instructions.
      // TODO: Narrow this to intrinsics that have store-like effects.
      const auto *CI = cast<CallInst>(I);
      if (!CI->isDebugOrPseudoInst() && !CI->isLifetimeStartOrEnd())
        return true;
      break;
    }
    case Instruction::Invoke:
      return true;
    case Instruction::GetElementPtr: {
      // If the GEP offset is out-of-bounds, or is non-constant and so has to be
      // assumed to be potentially out-of-bounds, then any memory access that
      // would use it could also be out-of-bounds meaning stack protection is
      // required.
      const GetElementPtrInst *GEP = cast<GetElementPtrInst>(I);
      unsigned IndexSize = DL.getIndexTypeSizeInBits(I->getType());
      APInt Offset(IndexSize, 0);
      if (!GEP->accumulateConstantOffset(DL, Offset))
        return true;
      TypeSize OffsetSize = TypeSize::getFixed(Offset.getLimitedValue());
      if (!TypeSize::isKnownGT(AllocSize, OffsetSize))
        return true;
      // Adjust AllocSize to be the space remaining after this offset.
      // We can't subtract a fixed size from a scalable one, so in that case
      // assume the scalable value is of minimum size.
      TypeSize NewAllocSize =
          TypeSize::getFixed(AllocSize.getKnownMinValue()) - OffsetSize;
      if (hasAddressTaken(I, NewAllocSize, M, VisitedPHIs))
        return true;
      break;
    }
    case Instruction::BitCast:
    case Instruction::Select:
    case Instruction::AddrSpaceCast:
      if (hasAddressTaken(I, AllocSize, M, VisitedPHIs))
        return true;
      break;
    case Instruction::PHI: {
      // Keep track of what PHI nodes we have already visited to ensure
      // they are only visited once.
      const auto *PN = cast<PHINode>(I);
      auto [It, Inserted] = VisitedPHIs.try_emplace(PN, AllocSize);
      if (!Inserted) {
        if (TypeSize::isKnownGE(AllocSize, It->second.AllocSize))
          break;

        // Check again with smaller size.
        if (It->second.NumDecreased == PhiInfo::MaxNumDecreased)
          return true;

        It->second.AllocSize = AllocSize;
        ++It->second.NumDecreased;
      }
      if (hasAddressTaken(PN, AllocSize, M, VisitedPHIs))
        return true;
      break;
    }
    case Instruction::Load:
    case Instruction::Ret:
      // These instructions take an address operand, but have load-like or
      // other innocuous behavior that should not trigger a stack protector.
      break;
    default:
      // Conservatively return true for any instruction that takes an address
      // operand, but is not handled above.
      return true;
    }
  }
  return false;
}

/// Check whether or not this function needs a stack protector based
/// upon the stack protector level.
///
/// We use two heuristics: a standard (ssp) and strong (sspstrong).
/// The standard heuristic which will add a guard variable to functions that
/// call alloca with a either a variable size or a size >= VarGuardBufferSize,
/// functions with character buffers larger than VarGuardBufferSize, and
/// functions with aggregates containing character buffers larger than
/// VarGuardBufferSize. The strong heuristic will add a guard variables to
/// functions that call alloca regardless of size, functions with any buffer
/// regardless of type and size, functions with aggregates that contain any
/// buffer regardless of type and size, and functions that contain stack-based
/// variables that have had their address taken.
bool VarGuardLayoutAnalysis::requiresVarGuard(Function *F,
                                              VarGuardLayoutMap *Layout) {
  Module *M = F->getParent();
  bool Strong = true;
  bool NeedsProtector = true;

  // The set of PHI nodes visited when determining if a variable's reference has
  // been taken.  This set is maintained to ensure we don't visit the same PHI
  // node multiple times.
  PhiMap VisitedPHIs;

  unsigned VarGuardBufferSize = F->getFnAttributeAsParsedInteger(
      "var-guard-buffer-size", VarGuardLayoutInfo::DefaultVGBufferSize);

  if (F->hasFnAttribute(Attribute::SafeStack))
    return false;

  // We are constructing the OptimizationRemarkEmitter on the fly rather than
  // using the analysis pass to avoid building DominatorTree and LoopInfo which
  // are not available this late in the IR pipeline.
  // creates an object for emitting remarking...
  OptimizationRemarkEmitter ORE(F);

  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {

        if (MDNode *MD = AI->getMetadata("var-guard")) {
          auto *CI = mdconst::dyn_extract<ConstantInt>(MD->getOperand(0));
          if (CI->isZero())
            continue;
        }

        if (AI->isArrayAllocation()) {

          if (auto *CI = dyn_cast<ConstantInt>(AI->getArraySize())) {

            if (CI->getLimitedValue(VarGuardBufferSize) >= VarGuardBufferSize) {
              // A call to alloca with size >= VarGuardBufferSize requires
              // stack protectors.
              // if caller only wants yes or no, return immmediately
              if (!Layout)
                return true;
              Layout->insert(
                  std::make_pair(AI, MachineFrameInfo::SSPLK_LargeArray));
              LLVMContext &Ctx = F->getContext();
              MDNode *N = MDNode::get(Ctx, MDString::get(Ctx, ""));
              AI->setMetadata("varguard.protect", N);
              NeedsProtector = true;

            } else if (Strong) {
              // Require protectors for all alloca calls in strong mode.
              if (!Layout)
                return true;
              Layout->insert(
                  std::make_pair(AI, MachineFrameInfo::SSPLK_SmallArray));
              LLVMContext &Ctx = F->getContext();
              MDNode *N = MDNode::get(Ctx, MDString::get(Ctx, ""));
              AI->setMetadata("varguard.protect", N);
              NeedsProtector = true;
            }
          } else {
            // A call to alloca with a variable size requires protectors.
            if (!Layout)
              return true;
            Layout->insert(
                std::make_pair(AI, MachineFrameInfo::SSPLK_LargeArray));
            LLVMContext &Ctx = F->getContext();
            MDNode *N = MDNode::get(Ctx, MDString::get(Ctx, ""));
            AI->setMetadata("varguard.protect", N);
            NeedsProtector = true;
          }
          continue;
        }

        bool IsLarge = false;
        if (containsGuardableArray(AI->getAllocatedType(), M,
                                   VarGuardBufferSize, IsLarge, Strong,
                                   false)) {
          if (!Layout)
            return true;
          Layout->insert(
              std::make_pair(AI, IsLarge ? MachineFrameInfo::SSPLK_LargeArray
                                         : MachineFrameInfo::SSPLK_SmallArray));
          LLVMContext &Ctx = F->getContext();
          MDNode *N = MDNode::get(Ctx, MDString::get(Ctx, ""));
          AI->setMetadata("varguard.protect", N);
          NeedsProtector = true;
          continue;
        }

        if (Strong) {
          // can you compute the size of this alloc?
          std::optional<TypeSize> AllocSize =
              AI->getAllocationSize(M->getDataLayout());
          if (!AllocSize || hasAddressTaken(AI, *AllocSize, M, VisitedPHIs)) {
            ++NumAddrTaken;
            if (!Layout)
              return true;
            Layout->insert(std::make_pair(AI, MachineFrameInfo::SSPLK_AddrOf));

            LLVMContext &Ctx = F->getContext();
            MDNode *N = MDNode::get(Ctx, MDString::get(Ctx, ""));
            AI->setMetadata("varguard.protect", N);
            NeedsProtector = true;
          }
        }
        // Clear any PHIs that we visited, to make sure we examine all uses of
        // any subsequent allocas that we look at.
        VisitedPHIs.clear();
      }
    }
  }

  return NeedsProtector;
}

static Value *getVarGuardValue(const TargetLoweringBase &TLI,
                               const LibcallLoweringInfo &Libcalls, Module *M,
                               IRBuilder<> &B) {

  Value *Guard = TLI.getIRStackGuard(B, Libcalls);
  StringRef GuardMode = M->getStackProtectorGuard();
  if ((GuardMode == "tls" || GuardMode.empty()) && Guard) {
    return B.CreateLoad(B.getPtrTy(), Guard, true, "VarGuard");
  }

  return nullptr;
}

/// Insert code into the entry block that stores the stack guard
/// variable onto the stack:
///
///   entry:
///     StackGuardSlot = alloca i8*
///     StackGuard = <stack guard>
///     call void @llvm.stackprotector(StackGuard, StackGuardSlot)
///
/// Returns true if the platform/triple supports the stackprotectorcreate pseudo
/// node.
static bool CreateVarGuardPrologue(Function *F, Module *M, AllocaInst *guardA,
                           const TargetLoweringBase *TLI,
                           const LibcallLoweringInfo &Libcalls,
                           AllocaInst *&AI) {
	// start by assuming selectionDag stack protector support is not needed -> what is selectionDag?
  	bool SupportsSelectionDAGSP = false;
	// create an IRBuilder positioned at the first instruction in the entry block.
	// So new instuctions are inserted at the beginning of the function
	IRBuilder<> B(guardA);
	B.SetInsertPoint(B.GetInsertPoint());

	Value *Guard = TLI->getIRStackGuard(B, Libcalls);
	StringRef GuardMode = M->getStackProtectorGuard();
	if ((GuardMode == "tls" || GuardMode.empty()) && Guard) {
		// Get generic pointer type
		PointerType *PtrTy = PointerType::getUnqual(guardA->getContext());
		AI = B.CreateAlloca(PtrTy, nullptr, "VarGuardSlot");
		Value *GuardSlot = B.CreateLoad(B.getPtrTy(), Guard, true, "VarGuard");
		//B.CreateIntrinsic(Intrinsic::stackprotector, {GuardSlot, AI});
		B.CreateStore(GuardSlot, AI, true);
	}

	if (SupportsSelectionDAGSP) 
		return false;

	return true;
static bool WrapAllocaWithVarGuardStruct(const TargetLowering &TLI, 
							const LibcallLoweringInfo &Libcalls, Function *F,
							AllocaInst *OldAI, Value *&BufPtr, Value *&CanaryPtr) {
	LLVMContext &Ctx = F->getContext();
	IRBuilder<> B(OldAI);

	Type *BufTy = OldAI->getAllocatedType();

	PointerType *CanaryTy = PointerType::getUnqual(Ctx);

	Value *GuardPtr = TLI.getIRStackGuard(B, Libcalls);
	if (!GuardPtr)
		return false;
	StructType *VGStructTy = StructType::create(Ctx, {BufTy, CanaryTy}, "varguard.struct");

	AllocaInst *StructAI = B.CreateAlloca(VGStructTy, nullptr, "varguard.obj");
	
	CanaryPtr = B.CreateStructGEP(VGStructTy, StructAI, 1, "vg.canary.ptr");
	BufPtr = B.CreateStructGEP(VGStructTy, StructAI, 0, "vg.buf.ptr");
	
	Value *Guardslot = B.CreateLoad(B.getPtrTy(), GuardPtr, true, "VarGuard");
	B.CreateStore(Guardslot, CanaryPtr, true);
	
	OldAI->replaceUsesWithIf(BufPtr, [](Use &U) -> bool {
        auto const *const user = U.getUser();
        if (auto const *const CI = dyn_cast<CallInst>(user)) {
            return !CI->isLifetimeStartOrEnd();
        }
        return true;
    });
	OldAI->replaceAllUsesWith(StructAI);

  AllocaInst *StructAI = B.CreateAlloca(VGStructTy, nullptr, "varguard.obj");

  CanaryPtr = B.CreateStructGEP(VGStructTy, StructAI, 1, "vg.canary.ptr");
  BufPtr = B.CreateStructGEP(VGStructTy, StructAI, 0, "vg.buf.ptr");

  Value *Guardslot = B.CreateLoad(B.getPtrTy(), GuardPtr, true, "VarGuard");
  B.CreateStore(Guardslot, CanaryPtr, true);

  OldAI->replaceUsesWithIf(BufPtr, [](Use &U) -> bool {
    auto const *const User = U.getUser();
    if (auto const *const CI = dyn_cast<CallInst>(User)) {
      return !CI->isLifetimeStartOrEnd();
    }
    return true;
  });
  OldAI->replaceAllUsesWith(StructAI);

  return true;
}

/*
 * Find places where the function can exit.
 * Create the stack guard prologue if needed.
 * Decided whether backend/SelectionDag will emit the check.
 * Otherwise insert IR checks before returns/noreturn calls.
 * Create failure block that calls stack_chk_fail
 *
 */

static VarGuardSlotMap InsertVarGuards(const TargetLowering &TLI,
                           const LibcallLoweringInfo &Libcalls, Function *F,
                           DomTreeUpdater *DTU, bool &HasPrologue,
                           bool &HasIRCheck) {
	VarGuardSlotMap SlotMap;
  	AllocaInst *pAI = nullptr; // Place on stack that stores the stack guard.
	//VarGuardLayoutInfo::VarGuardObjectInfo STInfo;
	// VarGuardLayoutInfo::ProtectedObject Pvg;
	std::vector<AllocaInst*> toRemove;

	// it walks through every basic block in the function
	// make_early_inc_range is important because this pass may modify the function while looping
	// safely iterate evn if blocks are/inserted during the loop
	 	for (BasicBlock &BB : llvm::make_early_inc_range(*F)) {
	//
	// 	// errs() << "in the basic loop\n";
		// loop over instructions
		for (auto &Inst : BB) {
			// errs() << "in the inst loop\n";
			if (auto *AllocaI = dyn_cast<AllocaInst>(&Inst)) {
				if (AllocaI->getMetadata("varguard.protect")) {
					// errs() << "alloca varguard proted\n";
					AllocaInst *AI = nullptr;
					if (CreateVarGuardPrologue(F, M, AllocaI, &TLI, Libcalls, AI)) {
						auto *I = cast<Instruction>(AllocaI);
					 	SlotMap[I] = AI;
						// errs() << "Protected alloca: " << *AllocaI << "\n";
						// errs() << "Guard slot: " << *AI << "\n";
					}
					Value *BufPtr = nullptr;
					Value *CanaryPtr = nullptr;
					if (!WrapAllocaWithVarGuardStruct(TLI, Libcalls, F, AllocaI, BufPtr, CanaryPtr))
						continue;
					toRemove.push_back(AllocaI);
					SlotMap[BufPtr] = CanaryPtr;
				}
			}
		}
	}

	// SmallVector<AllocaInst *, 8> ProtectedAllocas;
	//
	// for (BasicBlock &BB : *F) {
	// 	for (Instruction &Inst : BB) {
	// 		if (auto *AI = dyn_cast<AllocaInst>(&Inst)) {
	// 			if (AI->getMetadata("varguard.protect"))
	// 				ProtectedAllocas.push_back(AI);
	// 			}
	// 	  	}
	// 	}
	//
	// 	for (size_t Idx = 0; Idx < ProtectedAllocas.size(); ++Idx) {
	// 	  	// if (Idx == 0 || Idx + 1 == ProtectedAllocas.size())
	// 		// if (Idx == 0)
	// 		// 	continue;
	//
	// 	  	AllocaInst *AllocaI = ProtectedAllocas[Idx];
	// 		// if (Idx + 1 == ProtectedAllocas.size()) {
	// 		// 	SlotMap[AllocaI] = pAI;
	// 		// }
	// 		else if (CreateVarGuardPrologue(F, M, AllocaI, &TLI, Libcalls, AI)) {
	// 			if (Idx == 0) {
	// 				pAI = AI;
	// 				continue;
	// 			}
	// 			SlotMap[AllocaI] = pAI;
	// 			pAI = AI;
	// 	  	}
	// 	}

  	return SlotMap;
}

static bool VisitUsesAndInsertChecks(const TargetLowering &TLI,
		const LibcallLoweringInfo &Libcalls, Function *F,
		DomTreeUpdater *DTU, Instruction *Ins, AllocaInst *VGslot, BasicBlock *&fb) {

	auto *M = F->getParent();
		
	bool changed = false;
	for (User *U : llvm::make_early_inc_range(Ins->users())) {

		auto *I = dyn_cast<Instruction>(U);
		if (!I) continue;

		if (!fb)
			fb = CreateVarGuardFailBB(F, Libcalls);

		errs() << "done creating FailBB\n";
		switch (I->getOpcode()) {
			case Instruction::Store:
				{
				auto *SI = cast<StoreInst>(I);

  				Instruction *CheckLoc = SI->getNextNode();
  				if (!CheckLoc)
					break;
				//AllocaInst *AI = cast<AllocaInst>(I);
				//if (AI == cast<StoreInst>(I)->getPointerOperand()) {
				// insert a check after the write 
				// IRBuilder<> B(guardA);
				// B.SetInsertPoint(guardA->getParent(), ++B.GetInsertPoint());
				IRBuilder<> B(CheckLoc);
				//B.SetInsertPoint(CheckLoc->getParent(), ++B.GetInsertPoint());

				Value *Guard = getVarGuardValue(TLI, Libcalls, M, B);
				errs() << "return from getVG check\n";
				if (Guard) {
					errs() << "adding check\n";
					LoadInst *L12 = B.CreateLoad(B.getPtrTy(), VGslot, true);
					auto *Cmp = cast<ICmpInst>(B.CreateICmpNE(Guard, L12));
					auto SuccessProb = 
						BranchProbabilityInfo::getBranchProbStackProtector(true);
					auto FailureProb = 
						BranchProbabilityInfo::getBranchProbStackProtector(false);
					MDNode *Weights = MDBuilder(F->getContext())
						.createBranchWeights(FailureProb.getNumerator(), 
								SuccessProb.getNumerator());
					SplitBlockAndInsertIfThen(Cmp, CheckLoc, false, Weights, DTU, nullptr,
							fb);
					auto *BI = cast<CondBrInst>(Cmp->getParent()->getTerminator());
					BasicBlock *NewBB = BI->getSuccessor(1);
					NewBB->setName("VG_return");
					auto *BB = I->getParent();
					NewBB->moveAfter(BB);

					Cmp->setPredicate(Cmp->getInversePredicate());
					BI->swapSuccessors();
					changed |= true;
				} else {
					changed |= false;
				}
				break;
			}
			case Instruction::GetElementPtr: 

				changed |= VisitUsesAndInsertChecks(TLI, Libcalls, F, DTU, I, VGslot, fb);
				break;
			default:
				changed |= false;
				break;

		}
	}

	return changed;
}

// Write
static bool insertChecks(const TargetLowering &TLI,
                         const LibcallLoweringInfo &Libcalls, Function *F,
                         DomTreeUpdater *DTU, VarGuardSlotMap SlotMap) {

  bool Changed = false;
  BasicBlock *FailBB = nullptr;

	errs() << "looping through slotMap\n";	
	for (auto &Entry : slotMap) {
		Instruction *I = Entry.first;
		AllocaInst *VGslot = Entry.second;

		changed = VisitUsesAndInsertChecks(TLI, Libcalls, F, DTU, I, VGslot, FailBB);
		
	}

  return Changed;
}

// This creates the failure block
BasicBlock *createVarGuardFailBb(Function *F,
                                 const LibcallLoweringInfo &Libcalls) {
  auto *M = F->getParent();
  LLVMContext &Context = F->getContext();
  BasicBlock *FailBB = BasicBlock::Create(Context, "CallStackCheckFailBlk", F);
  IRBuilder<> B(FailBB);
  if (F->getSubprogram())
    B.SetCurrentDebugLocation(
        DILocation::get(Context, 0, 0, F->getSubprogram()));
  FunctionCallee StackChkFail;
  SmallVector<Value *, 1> Args;

  if (RTLIB::LibcallImpl ChkFailImpl =
          Libcalls.getLibcallImpl(RTLIB::STACKPROTECTOR_CHECK_FAIL)) {
    StackChkFail = M->getOrInsertFunction(
        RTLIB::RuntimeLibcallsInfo::getLibcallImplName(ChkFailImpl),
        Type::getVoidTy(Context));
  } else if (RTLIB::LibcallImpl SSHImpl =
                 Libcalls.getLibcallImpl(RTLIB::STACK_SMASH_HANDLER)) {
    StackChkFail = M->getOrInsertFunction(
        RTLIB::RuntimeLibcallsInfo::getLibcallImplName(SSHImpl),
        Type::getVoidTy(Context), PointerType::getUnqual(Context));
    Args.push_back(B.CreateGlobalString(F->getName(), "SSH"));
  } else {
    Context.emitError("no libcall available for var-guard");
  }

  if (StackChkFail) {
    CallInst *Call = B.CreateCall(StackChkFail, Args);
    Call->addFnAttr(Attribute::NoReturn);
  }

  B.CreateUnreachable();
  return FailBB;
}
