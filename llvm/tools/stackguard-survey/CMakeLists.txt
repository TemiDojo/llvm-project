set(LLVM_LINK_COMPONENTS
	Support
	Core
	Target
	CodeGen
	X86
)

add_llvm_tool(stackguard-survey
	StackGuardSurvey.cpp
)

target_link_libraries(stackguard-survey
	PRIVATE
	LLVMCore
	LLVMSupport
	LLVMTarget
	LLVMCodeGen
)


