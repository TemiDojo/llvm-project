	.file	"t_store_buf.c"
	.text
	.globl	test2                           # -- Begin function test2
	.p2align	4
	.type	test2,@function
test2:                                  # @test2
	.cfi_startproc
# %bb.0:
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	subq	$48, %rsp
	movq	%fs:40, %rax
	movq	%rax, -24(%rbp)
	movq	%fs:40, %rax
	movq	%rax, -40(%rbp)
	movq	%fs:40, %rax
	movq	%rax, -16(%rbp)
	movl	%edi, -28(%rbp)
	movl	$4473666, -8(%rbp)              # imm = 0x444342
	movb	$65, -5(%rbp)
	movq	%fs:40, %rax
	cmpq	-24(%rbp), %rax
	jne	.LBB0_6
# %bb.1:                                # %VG_return7
	movl	$0, -4(%rbp)
	cmpl	$4, -4(%rbp)
	jg	.LBB0_5
	.p2align	4
.LBB0_3:                                # =>This Inner Loop Header: Depth=1
	movslq	-4(%rbp), %rax
	movb	$65, -32(%rbp,%rax)
	movq	%fs:40, %rax
	cmpq	-16(%rbp), %rax
	jne	.LBB0_6
# %bb.4:                                # %VG_return
                                        #   in Loop: Header=BB0_3 Depth=1
	incl	-4(%rbp)
	cmpl	$4, -4(%rbp)
	jle	.LBB0_3
.LBB0_5:
	addq	$48, %rsp
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	retq
.LBB0_6:                                # %CallStackCheckFailBlk
	.cfi_def_cfa %rbp, 16
	callq	__stack_chk_fail@PLT
.Lfunc_end0:
	.size	test2, .Lfunc_end0-test2
	.cfi_endproc
                                        # -- End function
	.globl	test3                           # -- Begin function test3
	.p2align	4
	.type	test3,@function
test3:                                  # @test3
	.cfi_startproc
# %bb.0:
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	subq	$32, %rsp
	movl	%edi, -20(%rbp)
	movl	%edi, %eax
	movq	%rsp, -16(%rbp)
	movq	%fs:40, %rcx
	movq	%rcx, -8(%rbp)
	movq	%rsp, %rcx
	leaq	15(,%rax,4), %rdx
	andq	$-16, %rdx
	movq	%rcx, %rsi
	subq	%rdx, %rsi
	movq	%rsi, %rsp
	movq	%rax, -32(%rbp)
	negq	%rdx
	movl	$20, 20(%rcx,%rdx)
	movq	%fs:40, %rax
	cmpq	-8(%rbp), %rax
	jne	.LBB1_2
# %bb.1:                                # %VG_return
	movq	-16(%rbp), %rsp
	movq	%rbp, %rsp
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	retq
.LBB1_2:                                # %CallStackCheckFailBlk
	.cfi_def_cfa %rbp, 16
	callq	__stack_chk_fail@PLT
.Lfunc_end1:
	.size	test3, .Lfunc_end1-test3
	.cfi_endproc
                                        # -- End function
	.globl	main                            # -- Begin function main
	.p2align	4
	.type	main,@function
main:                                   # @main
	.cfi_startproc
# %bb.0:
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	subq	$16, %rsp
	movl	$0, -4(%rbp)
	movl	$4, %edi
	callq	test2
	movl	$4, %edi
	callq	test3
	xorl	%eax, %eax
	addq	$16, %rsp
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	retq
.Lfunc_end2:
	.size	main, .Lfunc_end2-main
	.cfi_endproc
                                        # -- End function
	.type	.L__const.test2.buf1,@object    # @__const.test2.buf1
	.section	.rodata.str1.1,"aMS",@progbits,1
.L__const.test2.buf1:
	.asciz	"BCD"
	.size	.L__const.test2.buf1, 4

	.ident	"clang version 22.1.3"
	.section	".note.GNU-stack","",@progbits
