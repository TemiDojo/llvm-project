#ifndef LLVM_CODEGEN_VARGUARD_H
#define LLVM_CODEGEN_VARGUARD_H
// header guard, this prevents the header from nbeing included twice 
// in  the same compiled file
// 'if LLVM_CODE... is not defined,
// define it and include this file's contents

#include "llvm/CodeGen/StackProtector.h"
#include "llvm/Analysis/DomTreeUpdater.h" // updates the dominator tree after CFG changes
#include "llvm/CodeGen/MachineFrameInfo.h"	// backend stack-frame info
#include "llvm/IR/Instructions.h"	// from instruction.h
#include "llvm/IR/PassManager.h"	// new pass manager
#include "llvm/Pass.h"	// legacy pass manager
#include "llvm/TargetParser/Triple.h"	// target/platform info


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

//Ora
//
// This class sotres stack-protector analysis results
class SSPLayoutInfo { // everything below is private the friend classes mean, that these classes are allowed to access private fields in the SSPLayoutInfo
	friend class StackProtectorPass;
	friend class SSPLayoutAnalysis;
	friend class StackProtector;
	//static -> belongs to the class,not one object
	//constexpr -> compile-time constant
	// default stack-protector buffer threshold is 8 bytes
	static constexpr unsigned DefaultSSPBufferSize = 8;

	/// A mapping of AllocaInsts to their required SSP layout.
	// this just creates a shorter name(type alias) meaning:
	// SSPLayoutMap is a hash map:
	// 		key = conts AllocaInst*
	// 		value = MachineFrameIfo::SSPLayoutKind
	// 	so it maps: IR stack allocation -> stack-protector layout category
	// 	e.g %buf alloca [64 x i8] -> SSPLK_LargeArray
	using SSPLayoutMap = DenseMap<const AllocaInst *, MachineFrameInfo::SSPLayoutKind>;

	/// Layout - Mapping of allocations to the required SSPLaoutKind.
	/// StackProtector analysis will update thi map hwen determining if an 
	/// AllocaInst triggers a st ack protector.
	// This stores the map... The .cpp analysis fills it and later
	// copyToMachineFrameInfo(...) copies it into backend MachineFrameInfo. The 
	// implementation loops over frame objects, get each original AllocaInst, looks
	// it up in Layout, then calls MFI.setObjectSSPLayout(...)
	SSPLayoutMap Layout;

	/// The minimum size of buffers taht will recieve stack smashing
	/// protection when -fstack-protection is used.
	// state fields
	unsigned SSPBufferSize = DefaultSSPBufferSize;

	bool RequireStackProtector = false;

	// A prologue is generated.
	bool HasPrologue = false;


	// IR checking code is generated.
	bool HasIRCheck = false;

public:
	// Return true if StackProtector is suppoesd to be handled by SelectionDAG.
	// what is selectionDAG?
	// should selctionDAG/backend emit the stack protector for this block?
	bool shouldEmitSDCheck(const BasicBlock &BB) const;
	// copy the IR-level layout decisions into backend stack-frame info.
	void copyToMachineFrameInfo(MachineFrameInfo &MFI) const;
};


// This is an LLVM analysis pass differentn from transform pass
// Analysis pass -> computes info
// Transform pass -> changes code
// This inherits from AnalysisInfoMixin, similar CRTP style to PassInfoMixin
class SSPLayoutAnalysis : public AnalysisInfoMixin<SSPLayoutAnalysis> {
	// this lets the AnalysisInfoMixin access private analysis identity info
	friend AnalysisInfoMixin<SSPLayoutAnalysis>; 
	// inside this class, reuse the private map tuype. this works because
	// SSPLayoutAnalysis is a friend of SSPLayoutInfo
	using SSPLayoutMap = SSPLayoutInfo::SSPLayoutMap;
	// LLVM's unique ID for this analysis
	// The PreservedAnalyses implementation uses analysis keys/IDs to track which
	// analyses are preserved or invalidated.
	static AnalysisKey Key;

public:
	// when this analysis runs, its result type is SSPLayoutInfo
	using Result = SSPLayoutInfo;
	// This is the analysis entry point
	// it computes the layout for one functon
	Result run(Function &F, FunctionAnalysisManager &FAM);

	/// Check whether or not \p F needs a stack protector based upon the stack
	/// protector level.
	// this helper checks if a function need stack protection. 
	static bool requiresStackProtector(Function *F, SSPLayoutMap *Layout = nullptr);
	// syntax
	// SSPLayoutMap *Layout = nullptr
	// means the second argument is optional so you can just call
	// requiresStackProtector(F) or requireStackProtector(F, &Layout)
	// if Lahyout is null, it can just answer yes/no. If Layout is provided, it also fills
	// the map.

};

// This is the new pass manager version
class StackProtectorPass : public PassInfoMixin<StackProtectorPass> {
	// It sotres TM -> target machine info
	const TargetMachine *TM;
	// constructors
public:
	// syntax: 
	// 		explicit -> prevents accidental implicit construction
	// 		const TargetMchine &TM -> constructor recieves a reference
	// 		: TM(&TM) -> initializer list; stores the address in the member pointer
	explicit StackProtectorPass(const TargetMachine &TM) : TM(&TM) {}
	// This the entry point to the pass
	// The LLVM new pass manager docs say new passes use PassInfoMixin and implement run()
	// returning PreservedAnalyses.
	PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

// This is the legacy pass manager version
// LLVM keeps both:
// 		StackProtectorPass -> new PM
// 		StackProtector		-> legacy PM
class StackProtector : public FunctionPass {
	//private aliases/members
private:
	/// A mapping of AllocaInsts to their required SSP layout.
	using SSPLayoutMap = SSPLayoutInfo::SSPLayoutMap;

	const TargetMachine *TM = nullptr; // target info
	
	Function *F = nullptr; // current function
	Module *M = nullptr; // current module

	// There may or may not be a DomTreeUpdater object
	std::optional<DomTreeUpdater> DTU; // maybe exists, maybe not

	SSPLayoutInfo LayoutInfo; // analysis/ layout state

public:
	// old pm uses this as pass identity
	static char ID; // Pass identification, repalcement for typeid.
	
	// constructor
	StackProtector();

	// returns the stored layout info by reference
	SSPLayoutInfo &getLayoutInfo() { return LayoutInfo; }

	// legacy pass manager method for declaring required/preserved analyses.
	void getAnalysisUsage(AnalysisUsage &AU) const override;

	// Return true if StackProtector is supposed to be handled by SelectionDAG.
	// wrapper method: asks LayoutInfo
	bool shouldEmitSDCheck(const BasicBlock &BB) const {
		return LayoutInfo.shouldEmitSDCheck(BB);
	}

	// legacy version of run()
	bool runOnFunction(Function &Fn) override;

	// copies layout info to backend frame info
	void copyToMachineFrameInfo(MachineFrameInfo &MFI) const {
		LayoutInfo.copyToMachineFrameInfo(MFI);
	}


	/// Check whether or not \p F needs a stack protector based upon the stack 
	/// protector level.
	// 	legacy wrapper that reuses the new analysis helper
	static bool requiresStackProtector(Function *F, SSPLayoutMap *Layout = nullptr) {
		return SSPLayoutAnalysis::requiresStackProtector(F, Layout);
	}
};

} // end namespace LLVM



#endif // LLVM_CODEGEN_VARGUARD_H
