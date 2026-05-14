#ifndef LLVM_TRANSFORMS_UTILS_VARGUARDHELLO_H
#define LLVM_TRANSFORMS_UTILS_VARGUARDHELLO_H

#include "llvm/IR/PassManager.h"

namespace llvm {
	class VarGuardHelloPass : public PassInfoMixin<VarGuardHelloPass> {
		public:
			PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
	};
}	// namespace llvm
#endif
