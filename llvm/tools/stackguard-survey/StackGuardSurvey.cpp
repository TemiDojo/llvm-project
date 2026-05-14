#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

int main(int argc, char **argv) {
	InitializeAllTargetInfos();
	InitializeAllTargets();
	InitializeAllTargetMCs();
	InitializeAllAsmPrinters();
	InitializeAllAsmParsers();
	// A target triple describes: <architecture-vendor-os>
	// LLVM uses this to decides
	// 		ABI
	// 		calling convention
	// 		instruction set
	// 		object format, etc.
	Triple TT("x86_64-pc-linux-gnu");

	std::string Error;
	// Target lookup
	// Target is LLVM's description of a backend family
	// eg., X86, AArch64, RISCV, ARM
	// Think Target = backend class/factory
	const Target *T = TargetRegistry::lookupTarget(TT, Error);

	if (!T) {
		errs() << "Target lookup failed: " << Error << "\n";
		return 1;
	}

	outs() << "Target found: " << T->getName() << "\n";

	TargetOptions Options;
	// Target machine creation
	// This is the fully configured compilation target
	// it includes the target triple, CPU, features, ABI, codegen optsion
	// subtargets, frame lowering rules
	// std::unique_ptr means this objects own the TargetMachine and 
	// automatically frees it later. Smart poiters
	std::unique_ptr<TargetMachine> TM(
			T->createTargetMachine(
				TT, // target triple
				"generic", // cpu model
				"", // feature sting
				Options, // target options with codegen settings
				std::nullopt));
	if (!TM) {
		errs() << "Failed to create TargetMachine\n";
		return 1;
	}

	outs() << "TargetMachine created successfully\n";

	return 0;

}
