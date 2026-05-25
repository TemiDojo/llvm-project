#include "llvm/CodeGen/StackProtector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/EHPersonalities.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/User.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
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
STATISTIC(NumAddrTaken, "Number of local variables that ahve their address taken");
STATISTIC(NumVarProtected, "Number of variables protected");


struct VarGuardObjectInfo {
  //Value *StructPtr;
  Value *BufPtr;
  Value *CanaryPtr;
};
using VarGuardSlotMap = DenseMap<Value *, Value *>;


// static: visible only in this .cpp file
// cl::opt<boo> llvm command-line option of type bool
// "enable-selectiondag-sp" the command-line flag name
// cl::Hidden -> do not show in normal help output
// So it creates a hidden llvm option:
static cl::opt<bool> EnableVarGuardSelectionDAGSP("enable-varguard-selectiondag-sp",
											cl::init(true), cl::Hidden);
// creates another hidden options controlling whether stack checks are inserted
// before certain noreturn calls - what are noreturn calls?
static cl::opt<bool> DisableVarGuardCheckNoReturn("disable-varguard-check-noreturn-call",
											cl::init(false), cl::Hidden);

// forward declarations of helper functions
// These inform C++ that these functins exist later in this file.
// You can call them before their full body appears
// static here means: This helper is local to this .cpp file.
// Other files cannot call it directly.
static VarGuardSlotMap InsertVarGuards(const TargetLowering &TLI,
                           const LibcallLoweringInfo &Libcalls, Function *F,
                           DomTreeUpdater *DTU, bool &HasPrologue,
                           bool &HasIRCheck);
// static VarGuardSlotMap InsertVarGuards(const TargetLowering &TLI,
// 									const LibcallLoweringInfo &Libcalls,
// 									Function *F, DomTreeUpdater *DTU,
// 									bool &HasPrologue, bool &HasIRCheck, VarGuardLayoutInfo::ProtectedObject);
static BasicBlock *CreateVarGuardFailBB(Function *F, const LibcallLoweringInfo &Libcalls);
static bool InsertChecks(const TargetLowering &TLI, 
		const LibcallLoweringInfo &Libcalls, Function *F,
		DomTreeUpdater *DTU, VarGuardSlotMap slotMap);
static bool VisitUsesAndInsertChecks(const TargetLowering &TLI,
		const LibcallLoweringInfo &Libcalls, Function *F,
		DomTreeUpdater *DTU, Instruction *Ins, Value *VGslot, BasicBlock *&fb);
static bool WrapAllocaWithVarGuardStruct(const TargetLowering &TLI, 
							const LibcallLoweringInfo &Libcalls, Function *F,
							AllocaInst *OldAI, Value *&BufPtr, Value *&CanaryPtr);

// should the backend / selectionDag emit a stack protector check for this block?
// it returns true only if all thre conditions are true
// if a stack protector prolgue was created, no ir-level check was already inserted and
// this basic block ends with a ret instruction.
// VarGuardLayoutInfo::shouldEmitVGCheck - thsi defines a method that was declared inside class VarGuardLayoutInfo
// :: mean this functin belongs to VarGuardLayoutInfo
// const at the end means thsi method will not modify the VarGuardLayoutInfo object
// There are two possible places to emit the stack protector check:
// 		IR pass emits it now or backend/selectiondag emits it later
bool VarGuardLayoutInfo::shouldEmitVGCheck(const BasicBlock &BB) const {
	return HasPrologue && !HasIRCheck && isa<ReturnInst>(BB.getTerminator());
}
/*
 * copies the stack-protector layout decisions from IR-level analyis into backend stack-frame objects. IR alloca -> backend frame index
 * for each backend stack object, it asks: did this stack object come from an aloca that VarGuardLayoutAnalysis marked?
 * if yes, it stores the stack-protector category into MachineFrameInfo
 */

void VarGuardLayoutInfo::copyToVGMachineFrameInfo(MachineFrameInfo &MFI) const {
	// if no allocas were marked, there is nothing to copy
	if (Layout.empty()) return;
	// loop over backend frame object indices, I is a frame index
	for (int I = 0, E = MFI.getObjectIndexEnd(); I != E; ++I) {
		// skip tack objects that no longer exists or are unused
		// what are stack objects?
		if (MFI.isDeadObjectIndex(I)) continue;
		// which llvm IR alloca created this stack object
		const AllocaInst *AI = MFI.getObjectAllocation(I);
		// if no alloca is associated:
		// skip it
		if (!AI) continue;

		// Look up that alloca in the SSP layout map
		VarGuardLayoutMap::const_iterator LI = Layout.find(AI);
		// if the alloca was not marked by stack-protector analysis, skip it
		if (LI == Layout.end()) continue;
		// this tells the backend frame info:
		// 		Frame object I has SSP layout kind LI->second
		// 	example of layout kinds are things like: large array, small array, address-taken object
		MFI.setObjectSSPLayout(I, LI->second);
	}
}

// This is the analysis pass. it computes: does this function need stack protection?
// which allocas matter? what buffer size threshold is being used?
// This defines the run() method for the VarGuardLayoutAnalysis class. so every class in llvm need a run function?
// This analysis returns an VarGuardLayoutInfo object.
VarGuardLayoutInfo VarGuardLayoutAnalysis::run(Function &F, FunctionAnalysisManager &FAM) {
	// create an empty result object
	VarGuardLayoutInfo Info;
	//returns t/f: does F need stack protection?
	//fills Info.Layout with dangerous allocas
	Info.RequireVarGuard = VarGuardLayoutAnalysis::requiresVarGuard(&F, &Info.Layout);
	// Reads teh function attribute, if missing use default 8.
	Info.VarGuardBufferSize = F.getFnAttributeAsParsedInteger("stack-protector-buffer-size", VarGuardLayoutInfo::DefaultVGBufferSize);
	// returns the completed analysis
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
	//Info.ProtectedObject;
	VarGuardSlotMap slotMap = InsertVarGuards(*TLI, Libcalls, &F, DT ? &DTU : nullptr, Info.HasPrologue, Info.HasIRCheck);

	// write check 
	//
	//
	//
	// read check
	//InsertChecks(slotMap);
	errs() << "calling insert checks\n";
	bool changed = InsertChecks(*TLI, Libcalls, &F, DT ? &DTU : nullptr, slotMap);

	
	

	PreservedAnalyses PA;
	//PA.preserve<VarGuardLayoutAnalysis>();
	PA.preserve<DominatorTreeAnalysis>();
	return PA;
}

// this defines the static ID declared in the header
char VarGuard::ID = 0;
// This is the constructor: Define the constructor for class StackProtector
// : is the initializer list
// : FunctionPass(ID) meaning call the parent class constructor FunctionPass with ID
// so this says create a legacy FunctionPass named StackProtector.
VarGuard::VarGuard() : FunctionPass(ID) {}

// These are LLVM macros for registering the legacy pass
// its states: This passs is named stack-protector.
// 		It depends on: LibcallLowringInofoWrapper
// 		TargetPassConfig
// 		DominatorTreeWrapperPass
INITIALIZE_PASS_BEGIN(VarGuard, DEBUG_TYPE, "Insert stack protectors", false, true)
INITIALIZE_PASS_DEPENDENCY(LibcallLoweringInfoWrapper);
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig);
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass);
INITIALIZE_PASS_END(VarGuard, DEBUG_TYPE, "Insert stack protectors", false, true)

// This is a factory function, create and return a new StackProtector legacy pass object
// return type is FunctionPass * means poitner to a legacy function pass.
FunctionPass *llvm::createVarGuardPass() { return new VarGuard(); }

// This just tells llvm (old pass manager):
// what analyses do I need? what analyses do I preserve
void VarGuard::getAnalysisUsage(AnalysisUsage &AU) const {
	// Ined libcall lowering info before I run
	AU.addRequired<LibcallLoweringInfoWrapper>();
	// I need access to TargetMachine/backend config
	AU.addRequired<TargetPassConfig>();
	// I primise I will keep the dominator tree valid
	AU.addPreserved<DominatorTreeWrapperPass>();
}

// legacy equivalent of PreservedAnalyses... run
bool VarGuard::runOnFunction(Function &Fn) {

	F = &Fn; // F -> current function
	M = F->getParent(); // M -> module containing the function
	// auto *DTWP: type is infered automatically
	// getAnalysisIfAvailable<T>(): if dominatortree exists, give it to me. if not return null.
	// DTU.empalce...so create DTU only if DominatorTree exists
	if (auto *DTWP = getAnalysisIfAvailable<DominatorTreeWrapperPass>())
		DTU.emplace(DTWP->getDomTree(), DomTreeUpdater::UpdateStrategy::Lazy);
	// get targetmachine from targetpassconfig - this is how the legacy pass accesses backend info.
	TM = &getAnalysis<TargetPassConfig>().getTM<TargetMachine>();
	// clear previous state
	LayoutInfo.HasPrologue = false;
	LayoutInfo.HasIRCheck = false;

	// read function attribute or use default
  	LayoutInfo.VarGuardBufferSize = Fn.getFnAttributeAsParsedInteger(
      "stack-protector-buffer-size", VarGuardLayoutInfo::DefaultVGBufferSize);
	// if function does not need protection -> do nothing
	if (!requiresVarGuard(F, &LayoutInfo.Layout))
		return false;

	// TODO(etienneb): Functions with funclets are not correctly supported now.
	// Do nothing if this is funclet-based personality.
	if (Fn.hasPersonalityFn()) {
    	EHPersonality Personality = classifyEHPersonality(Fn.getPersonalityFn());
    	if (isFuncletEHPersonality(Personality))
      		return false;
  	}
	// per fuction target info
  	const TargetSubtargetInfo *Subtarget = TM->getSubtargetImpl(Fn);
	// get runtime call lowering rules
  	const LibcallLoweringInfo &Libcalls =
    	getAnalysis<LibcallLoweringInfoWrapper>().getLibcallLowering(*M,
                                                                   *Subtarget);
	// get lowering rules
  	const TargetLowering *TLI = Subtarget->getTargetLowering();
	// increment statistic
  	++NumFunProtected;
	// same as new pass: call the core function that inserts stack protector logic
	// DUT ? &*DTU : nullptr -> if DTU exists -> pass pointer to it else -> pass null
  	VarGuardSlotMap slotMap = InsertVarGuards(*TLI, Libcalls, F, DTU ? &*DTU : nullptr,
  	                          LayoutInfo.HasPrologue, LayoutInfo.HasIRCheck);
	bool Changed = !slotMap.empty();

	// debug verification: verify if dominator tree if enabled
#ifdef EXPENSIVE_CHECKS
  	assert((!DTU ||
    	DTU->getDomTree().verify(DominatorTree::VerificationLevel::Full)) &&
        "Failed to maintain validity of domtree!");
#endif
	// destroy the optional DomTreeUpdater
	DTU.reset();
	return Changed;
}

// does this LLVM Type contain an array that should trigger protection?
// Type *Ty:the type being inspected, the modules, used for target/data-layout info
// threshold size usually 8 bytes, outputs parameter. The & means reference, so changes
// inside the functin affects the caller's variable, whether strong stack protection mode is enabled,
// whether we are currently checking a type nested insdie a struct.
/* Type
 ├─ array?
 │   ├─ large enough?
 │   └─ strong mode?
 └─ struct?
     └─ check each field
	 */
static bool ContainsGuardableArray(Type *Ty, Module *M, unsigned VarGuardBufferSize,
                                     bool &IsLarge, bool Strong,
                                     bool InStruct) {
	// if ther is not type, it cannot contain a protectable array
	if (!Ty)
		return false;
	// if Ty is actually an ArrayType, treat it as ArrayType and enter this block
	// dyn_cat<T>(x) is LLVM's safe runtime cast.
  	if (ArrayType *AT = dyn_cast<ArrayType>(Ty)) {
		// if this not an i8 array, in llvm, char is usually i8.
    	if (!AT->getElementType()->isIntegerTy(8)) {
      // If we're on a non-Darwin platform or we're inside of a structure, don't
      // add stack protectors unless the array is a character array.
      // However, in strong mode any array, regardless of type and size,
      // triggers a protector.
	  	// In normal mode: non-char arrays usually do not trigger protection
		// especially inside structs or non-Darwin targets, but in sting mode, more arrays count
      		if (!Strong && (InStruct || !M->getTargetTriple().isOSDarwin()))
	        	return false;
    	}

    	// If an array has more than VarGuardBufferSize bytes of allocated space, then we
    	// emit stack protectors.
		// This asks, is this array at least VarGuardBufferSize bytes in memory?
		// if yest this a large protectable array, so it sets IsLarge to true
    	if (VarGuardBufferSize <= M->getDataLayout().getTypeAllocSize(AT)) {
      		IsLarge = true;
      		return true;
    	}

		// In strong mode, even small arrays can trigger protection.
    	if (Strong)
      	// Require a protector for all arrays in strong mode
      	return true;
  	}
	// If it is not a struct, and not a protectable array, return false.
  	const StructType *ST = dyn_cast<StructType>(Ty);
  	if (!ST)
		return false;
	// Loop over each filed type inside the struct.
  	bool NeedsProtector = false;
  	for (Type *ET : ST->elements())
		// check whether this struct field contains a protectable array.
		// notice the last argument, now we're inside a struct
    	if (ContainsGuardableArray(ET, M, VarGuardBufferSize, IsLarge, Strong, true)) {
      // If the element is a protectable array and is large (>= VarGuardBufferSize)
      // then we are done.  If the protectable array is not large, then
      // keep looking in case a subsequent element is a large array.
	  // if large array is found
      if (IsLarge)
        return true;
      NeedsProtector = true;
    }

	return NeedsProtector;
}

// This supports the next helper, HasAddressTaken
// Did the address of this stack variable escape or get used dangerously?
struct PhiInfo { // defines a small helper object
  	TypeSize AllocSize; // stores the remaining known allocation size
  	unsigned NumDecreased = 0; // tracks how many times llvm has reduces teh known safe size while following pointer uses
	// A compile time constant: only alow the tracked size to shrink a few times before giving up
	// why? to avaoid infinite or very expensive recursion thorugh complicated PHI cycles.
  	static constexpr unsigned MaxNumDecreased = 3;
	// contructor, after : is an initializer list
	// meaning initialzie the member AllocSize using the constructor parameter AllocSize.
  	PhiInfo(TypeSize AllocSize) : AllocSize(AllocSize) {}
};
// This create a type alias
using PhiMap = SmallDenseMap<const PHINode *, PhiInfo, 16>;
// A PHINode is an SSA instruction that selects a value depending on which predecessor block control
// came from.
// This section exists becaues HasAddressTaken recursively follows users of an alloca

/// Check whether a stack allocation has its address taken.
// does this stack allocation's address get used in a way that could escape or be unsafe?
static bool HasAddressTaken(const Instruction *AI, TypeSize AllocSize,
                            Module *M,
                            PhiMap &VisitedPHIs) {
	// gets size/layout rules for the target
	// how many bytes does this type occupy?
	// what offset does this GEP produce? what is GEP?
  	const DataLayout &DL = M->getDataLayout();
	// for every instruction/user that uses this allocation pointer
  	for (const User *U : AI->users()) {
		// Treat the user as an instruction
		// LLVM checked/assumed this is definitely type T.
		// if not, assert/crash in debug builds.
    	const auto *I = cast<Instruction>(U);
    	// If this instruction accesses memory make sure it doesn't access beyond
    	// the bounds of the allocated object.
		/// If this instruction access memory, and LLVM knows the size, and the 
		/// access may exceed the allocation size, then treat it as dangerous.
		/// So access bigger than object -> address considered risky.
    	std::optional<MemoryLocation> MemLoc = MemoryLocation::getOrNone(I);
    	if (MemLoc && MemLoc->Size.hasValue() &&
        	!TypeSize::isKnownGE(AllocSize, MemLoc->Size.getValue()))
      		return true;
		// this checks what kinds of instruction is using the pointer.
		// the function classifies uses into safe, unsafe, follow recursively.
    	switch (I->getOpcode()) {
			// if the alloca pointer itself is being stored somewhere, its address escaped
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
			// If passed to a real call, usually unsafe.
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
			// A GEP computes a derived pointer. LLVM tries to prove the derived pointer stays
			// in bounds. If not, unsafe. If yes, recursively follow the GEP's users.
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
				if (HasAddressTaken(I, NewAllocSize, M, VisitedPHIs))
					return true;
				break;
    		}
			// These created derived pointer values. Recursively follow them
    		case Instruction::BitCast:
    		case Instruction::Select:
    		case Instruction::AddrSpaceCast:
      			if (HasAddressTaken(I, AllocSize, M, VisitedPHIs))
        			return true;
      			break;
			// Track it in VisitedPhis, then recursively follow it
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
      			if (HasAddressTaken(PN, AllocSize, M, VisitedPHIs))
        			return true;
      			break;
    		}
			// These are treated as not triggering stack protector here.
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


/// Search for the first call to the llvm.stackprotector intrinsic and return it
/// if present.
// It searched a function for this intrinsic: llvm.stackprotector
// that intrinsici represents LLVM stack protector prologue marker/checker setup
// if it finds one, it returns the call instruction
/*
 * This exists because InsertVarGuards may need to recover the stack guard slot that was created 
 * earlier. So it searches for the existing llvm.stackprotector call and extracts its argument.
 * This basically answers- has this function already been given a stack protection intrinsic
 * what is an intrinsic?
 */
static const CallInst *findVarGuardIntrinsic(Function &F) {
	// loop over basic block in the function.
  	for (const BasicBlock &BB : F)
		// loop over every instruction in that basic block
    	for (const Instruction &I : BB)
			// try to cast the instruction to an IntrinsicInst.
			// An intrinsic is a special LLVM built-in function-like operation
      		if (const auto *II = dyn_cast<IntrinsicInst>(&I))
				// Check whether this intrinsic is specifically the stack protector intrinsic.
        		if (II->getIntrinsicID() == Intrinsic::stackprotector)
					// return it immediately
          			return II;
	// if nothing matched, return null
	return nullptr;
}


/// Check whether or not this function needs a stack protector based
/// upon the stack protector level.
///
/// We use two heuristics: a standard (ssp) and strong (sspstrong).
/// The standard heuristic which will add a guard variable to functions that
/// call alloca with a either a variable size or a size >= VarGuardBufferSize,
/// functions with character buffers larger than VarGuardBufferSize, and functions
/// with aggregates containing character buffers larger than VarGuardBufferSize. The
/// strong heuristic will add a guard variables to functions that call alloca
/// regardless of size, functions with any buffer regardless of type and size,
/// functions with aggregates that contain any buffer regardless of type and
/// size, and functions that contain stack-based variables that have had their
/// address taken.
/*
 * This is the decision function. It asks does this fucntion need stack protection?
 * and optionally fills which allocas causes protection. retursn true or false
 */
bool VarGuardLayoutAnalysis::requiresVarGuard(Function *F,
                                               VarGuardLayoutMap *Layout) {
  	Module *M = F->getParent(); // module containing the function
  	bool Strong = true; // are we in strong stack-protector mode
  	bool NeedsProtector = true; // final answer being built

  // The set of PHI nodes visited when determining if a variable's reference has
  // been taken.  This set is maintained to ensure we don't visit the same PHI
  // node multiple times.
  	PhiMap VisitedPHIs; // used by HasAddressTaken
	// This reads the function attribute, if absent, use default size 8
	// so this controls how large must a buffer be before normal stack protection triggers?
  	unsigned VarGuardBufferSize = F->getFnAttributeAsParsedInteger(
      "var-guard-buffer-size", VarGuardLayoutInfo::DefaultVGBufferSize);
	// if function uses safestack, this stack protector pass not protect it here
	// what is safestack?
  	if (F->hasFnAttribute(Attribute::SafeStack))
    	return false;

  	// We are constructing the OptimizationRemarkEmitter on the fly rather than
  	// using the analysis pass to avoid building DominatorTree and LoopInfo which
  	// are not available this late in the IR pipeline.
	// creates an object for emitting remarking...
  	OptimizationRemarkEmitter ORE(F);
	// if stack protection is explicityly required
	//  	if (F->hasFnAttribute(Attribute::StackProtectReq)) {
	// 	// if caller only wants yes/no, return immediately.
	//    	if (!Layout)
	//      		return true;
	// 	// otherwise emit a remark
	//    	ORE.emit([&]() {
	//      	return OptimizationRemark(DEBUG_TYPE, "VarGuardRequested", F)
	//        	<< "Var Guard applied to variable "
	//            << ore::NV("Function", F)
	//            << " due to a function attribute or command-line switch";
	//    	});
	// 	// use strong layout classification
	//    	NeedsProtector = true;
	//    	Strong = true; // Use the same heuristic as strong to determine SSPLayout
	// // if strong protection attribute exists
	//  	} else if (F->hasFnAttribute(Attribute::StackProtectStrong))
	// 	// use strong mode
	//    	Strong = true;
	// // else if normal stack protection is not enabled
	//  	else if (!F->hasFnAttribute(Attribute::StackProtect))
	// 	// no protection
	//    	return false;
	// F is pointer, so *F dereferences it to get the actual Function
	// loop iterate over the function's basic block
  	for (BasicBlock &BB : *F) {
		// loop over instructions inside that basic block
    	for (Instruction &I : BB) {
			// Try to cast instruction I into an AllocaInst
			// If it is an alloca, AI is non-null and the block runs.
			// if not skip
      		if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
				// this checks for metadata attached to the alloca
				// m? if this alloca has stack-protector metadata set to 0, skip it.
        		if (MDNode *MD = AI->getMetadata("stack-protector")) {
          			auto *CI = mdconst::dyn_extract<ConstantInt>(MD->getOperand(0));
          			if (CI->isZero())
            			continue;
					// llvm allows specific allocs to opt out of stack protector consideration. why?
        		}
				// this checks dynamic or repeated allocation size.
				// runtime dependent allocation
        		if (AI->isArrayAllocation()) {
					// this creates a labda taht builds the optimization remark
					// [7] means capture surrounding variables by reference.
					// It oes not emit immediately. It prepares a message taht can beemitted later with
					// ORE.emit...
          			auto RemarkBuilder = [&]() {
            			return OptimizationRemark(DEBUG_TYPE, "VarGuardAllocaOrArray",
                                      &I)
                   			<< "Var Guard applied to variable "
                   			<< ore::NV("Function", F)
                   			<< " due to a call to alloca or use of a variable length "
                      			"array";
          			};
					// is the array size known at compile time?, if yes, CI is the constant integer.
          			if (auto *CI = dyn_cast<ConstantInt>(AI->getArraySize())) {
						// if allocation count is at least the threshold, protect it.
            			if (CI->getLimitedValue(VarGuardBufferSize) >= VarGuardBufferSize) {
              				// A call to alloca with size >= VarGuardBufferSize requires
              				// stack protectors.
							// if caller only wants yes or no, return immmediately
              				if (!Layout)
                				return true;
							// mark this alloca as a large array. Emit remark.
							// Remember that this function needs protection.
              				// Layout->insert(
              				// 				std::make_pair(AI, MachineFrameInfo::SSPLK_LargeArray));
              				// 		ORE.emit(RemarkBuilder);
							// attach a metada to the IR here?
							LLVMContext &Ctx = F->getContext();
							MDNode* N = MDNode::get(Ctx, MDString::get(Ctx, ""));
							AI->setMetadata("varguard.protect", N);
              				NeedsProtector = true;
						// if size is below threshold but strong mode is enabled.
            			} else if (Strong) {
              				// Require protectors for all alloca calls in strong mode.
              				if (!Layout)
                				return true;
							// mark as small array
              				// Layout->insert(
              				// 				std::make_pair(AI, MachineFrameInfo::SSPLK_SmallArray));
              				// 	ORE.emit(RemarkBuilder);
							// attach a metada to the IR here?
							LLVMContext &Ctx = F->getContext();
							MDNode* N = MDNode::get(Ctx, MDString::get(Ctx, ""));
							AI->setMetadata("varguard.protect", N);
              				NeedsProtector = true;
            			}
          			} else {
            			// A call to alloca with a variable size requires protectors.
						// this is the size not known at compile time case
						// if getArraySize() is not a constantInt, LLVM treatse it as dangerous			
            			if (!Layout)
              				return true;
            			// Layout->insert(
            			//  			std::make_pair(AI, MachineFrameInfo::SSPLK_LargeArray));
            			// 		ORE.emit(RemarkBuilder);
						// attach metadata to the IR
						LLVMContext &Ctx = F->getContext();
						MDNode* N = MDNode::get(Ctx, MDString::get(Ctx, ""));
						AI->setMetadata("varguard.protect", N);
            			NeedsProtector = true;
          			}
          		continue;
        	}
		// output flag
		bool IsLarge = false;
		// recursively checks if this type contain a buffertaht should trigger stack protection?
		if (ContainsGuardableArray(AI->getAllocatedType(), M, VarGuardBufferSize,
                                     IsLarge, Strong, false)) {
        	if (!Layout)
        		return true;
			// if buffer is large, mark alloc as large array
			// else mark alloca as small array
			// condition ? value_if_true : value_if_false
          	// Layout->insert(std::make_pair(
          	//  	AI, IsLarge ? MachineFrameInfo::SSPLK_LargeArray
          	//                : MachineFrameInfo::SSPLK_SmallArray));
          	ORE.emit([&]() {
            	return OptimizationRemark(DEBUG_TYPE, "VarGuardBuffer", &I)
                	<< "Var Guard applied to  variable"
                   	<< ore::NV("Function", F)
                   	<< " due to a stack allocated buffer or struct containing a "
                      	"buffer";
          	});
			// attach metadata to the IR
			LLVMContext &Ctx = F->getContext();
			MDNode* N = MDNode::get(Ctx, MDString::get(Ctx, ""));
			AI->setMetadata("varguard.protect", N);
          	NeedsProtector = true;
			// move to the next instruction
          	continue;
		}
		// this runs only in strong
		if (Strong) {
			// can you compute the size of this alloc?
          	std::optional<TypeSize> AllocSize =
            	AI->getAllocationSize(M->getDataLayout());
			// !AllocSize = maybe there is a size, maybe not. llvm could not determine the size
			// HasAddressTaken...= Does thsi alloca's addressescape or get used dagerously.
			// *AllocSize unwraps the optional value
			// of size is unkown OR address is taken, treat it as needing protection
          	if (!AllocSize || HasAddressTaken(AI, *AllocSize, M, VisitedPHIs)) {
				// increment statistic
            	++NumAddrTaken;
            	if (!Layout)
              		return true;
            		// Layout->insert(std::make_pair(AI, MachineFrameInfo::SSPLK_AddrOf));
            			ORE.emit([&]() {
              				return OptimizationRemark(DEBUG_TYPE,
                                        "StackProtectorAddressTaken", &I)
                     			<< "Stack protection applied to function "
                     			<< ore::NV("Function", F)
                     			<< " due to the address of a local variable being taken";
            			});

					// attach metadata to the IR
					LLVMContext &Ctx = F->getContext();
					MDNode* N = MDNode::get(Ctx, MDString::get(Ctx, ""));
					AI->setMetadata("varguard.protect", N);
            		NeedsProtector = true;
          	}
    	}
        // Clear any PHIs that we visited, to make sure we examine all uses of
        // any subsequent allocas that we look at.
		// This clears the PHI tracking set before checking next alloca.
		// Why? The PHI recursion tracking is only for one alloca's use graph. You do not
		// want visited PHI's from one variable affecting the next variabl
        	VisitedPHIs.clear();
      		}
		}
	}

	return NeedsProtector;
}


/// Create a stack guard loading and populate whether SelectionDAG SSP is
/// supported.
// It gets the actual stack guard /canary value
// IRBuilder<> &B -> helper object used to create LLVM IR instruction.
static Value *getVarGuardValue(const TargetLoweringBase &TLI,
                            const LibcallLoweringInfo &Libcalls, Module *M,
                            IRBuilder<> &B,
                            bool *SupportsSelectionDAGSP = nullptr) {
	// can you give me an IR-level way to access the stack guard
	Value *Guard = TLI.getIRStackGuard(B, Libcalls);
	// reads module-level stack-protector guard mode
  	StringRef GuardMode = M->getStackProtectorGuard();
	// if guard mode is TLS/default and target gave us a guard pointer, load the
	// canary value from it.
  	if ((GuardMode == "tls" || GuardMode.empty()) && Guard) {
		// B.CreateLoad... creates a new LLVM IR load instruction.
		//AI = B.Create
		errs() << "returning guard value \n";
    	return B.CreateLoad(B.getPtrTy(), Guard, true, "VarGuard");
	}

	if (SupportsSelectionDAGSP)
    	*SupportsSelectionDAGSP = true;
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
}

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
	Value *GuardValue = B.CreateStore(Guardslot, CanaryPtr, true);
	
	OldAI->replaceAllUsesWith(BufPtr);

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
  	auto *M = F->getParent(); // get the module containing the function

	VarGuardSlotMap SlotMap;
	//VarGuardLayoutInfo::VarGuardObjectInfo STInfo;
	// VarGuardLayoutInfo::ProtectedObject Pvg;
  	AllocaInst *pAI = nullptr; // Place on stack that stores the stack guard.
	std::vector<AllocaInst*> toRemove;

	// it walks through every basic block in the function
	// make_early_inc_range is important because this pass may modify the function while looping
	// safely iterate evn if blocks are/inserted during the loop
	for (BasicBlock &BB : llvm::make_early_inc_range(*F)) {

 	// errs() << "in the basic loop\n";
	// loop over instructions
		for (auto &Inst : BB) {
			// errs() << "in the inst loop\n";
			if (auto *AllocaI = dyn_cast<AllocaInst>(&Inst)) {
			// auto *AllocaI = cast<AllocaInst>(&Inst);
				if (AllocaI->getMetadata("varguard.protect")) {
					// errs() << "alloca varguard proted\n";
					AllocaInst *AI = nullptr;
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

	for (auto *AllocaI : toRemove) {
		AllocaI->eraseFromParent();
	}

	

  	return SlotMap;
}

static bool VisitUsesAndInsertChecks(const TargetLowering &TLI,
		const LibcallLoweringInfo &Libcalls, Function *F,
		DomTreeUpdater *DTU, Instruction *Ins, Value *VGslot, BasicBlock *&fb) {

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
static bool InsertChecks(const TargetLowering &TLI, 
		const LibcallLoweringInfo &Libcalls, Function *F,
		DomTreeUpdater *DTU, VarGuardSlotMap slotMap) {

	bool changed = false;
	BasicBlock *FailBB = nullptr;

	errs() << "looping through slotMap\n";	
	for (auto &Entry : slotMap) {

		auto *I = dyn_cast<Instruction>(Entry.first);
		if (!I) continue;

		Value *guardStore = Entry.second;
		changed = VisitUsesAndInsertChecks(TLI, Libcalls, F, DTU, I, guardStore, FailBB);
		
	}

	return changed;
}

// This creates the failure block
BasicBlock *CreateVarGuardFailBB(Function *F, const LibcallLoweringInfo &Libcalls) {
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
    	Context.emitError("no libcall available for stack protector");
  	}

  	if (StackChkFail) {
    	CallInst *Call = B.CreateCall(StackChkFail, Args);
    	Call->addFnAttr(Attribute::NoReturn);
  	}

  	B.CreateUnreachable();
  	return FailBB;
}


