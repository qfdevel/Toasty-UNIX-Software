/* TUS port: upstream uses arch_prctl(ARCH_SET_FS) via the Linux
 * `syscall` instruction; TUS exposes the same operation as syscall
 * number 14 through `int $0x80`. */
.text
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
	mov %rdi,%rsi           /* shift for syscall */
	movl $0x1002,%edi       /* SET_FS register */
	movl $14,%eax           /* TUS arch_prctl */
	int $0x80
	ret
