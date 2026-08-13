/* TUS port: thread cancellation is not implemented yet, so
 * __syscall_cp_asm forwards to tus_syscall() after checking the
 * cancellation flag exactly like upstream. The Linux `syscall`
 * instruction is replaced by the TUS trap path. */
.text
.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.hidden __cancel
.global __syscall_cp_asm
.hidden __syscall_cp_asm
.type   __syscall_cp_asm,@function
__syscall_cp_asm:

__cp_begin:
	mov (%rdi),%eax
	test %eax,%eax
	jnz __cp_cancel
	/* Convert (cancel, n, a1, a2, a3, a4, a5, a6) to the SysV
	   argument order of tus_syscall(n, a1, a2, a3, a4, a5, a6):
	   incoming rdi=cancel rsi=n rdx=a1 rcx=a2 r8=a3 r9=a4
	              8(%rsp)=a5 16(%rsp)=a6
	   outgoing      rdi=n   rsi=a1 rdx=a2 rcx=a3 r8=a4 r9=a5
	              8(%rsp)=a6 */
	mov %rsi,%rdi
	mov %rdx,%rsi
	mov %rcx,%rdx
	mov %r8,%rcx
	mov %r9,%r8
	mov 8(%rsp),%r9
	mov 16(%rsp),%rax
	push %rax
	call tus_syscall
	add $8,%rsp
__cp_end:
	ret
__cp_cancel:
	jmp __cancel
.size   __syscall_cp_asm, .-__syscall_cp_asm
