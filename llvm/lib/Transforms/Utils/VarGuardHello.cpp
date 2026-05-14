#include "llvm/Transforms/Utils/VarGuardHello.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"


using namespace llvm;


PreservedAnalyses VarGuardHelloPass::run(Function &F, FunctionAnalysisManager &) {
	errs() << "VarGuardHello running on: " << F.getName() << "\n";
	return PreservedAnalyses::all();
}
