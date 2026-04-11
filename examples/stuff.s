	.file	"stuff.ll"
	.text
	.globl	init                            # -- Begin function init
	.p2align	4
	.type	init,@function
init:                                   # @init
	.cfi_startproc
# %bb.0:                                # %entry
	movl	$42, -4(%rsp)
	retq
.Lfunc_end0:
	.size	init, .Lfunc_end0-init
	.cfi_endproc
                                        # -- End function
	.globl	move                            # -- Begin function move
	.p2align	4
	.type	move,@function
move:                                   # @move
	.cfi_startproc
# %bb.0:                                # %entry
	retq
.Lfunc_end1:
	.size	move, .Lfunc_end1-move
	.cfi_endproc
                                        # -- End function
	.globl	double                          # -- Begin function double
	.p2align	4
	.type	double,@function
double:                                 # @double
	.cfi_startproc
# %bb.0:                                # %entry
	xorps	%xmm0, %xmm0
	retq
.Lfunc_end2:
	.size	double, .Lfunc_end2-double
	.cfi_endproc
                                        # -- End function
	.section	".note.GNU-stack","",@progbits
