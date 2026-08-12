/*
 * linker.ld - TUS kernel linker script (x86_64, higher half)
 *
 * The kernel is linked into the top of the canonical 64-bit address
 * space ("higher half"). Limine maps our ELF program headers exactly
 * where the linker placed them, so the kernel runs from virtual
 * addresses starting at 0xffffffff80000000 with no relocation needed.
 */

OUTPUT_FORMAT(elf64-x86-64)

/* The bootloader jumps to this symbol; see kernel/main.c. */
ENTRY(_start)

PHDRS
{
    text    PT_LOAD;
    rodata  PT_LOAD;
    data    PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(4096);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    /*
     * Limine boot protocol requests. Limine locates this section by
     * name and fills in every request struct found inside, so it must
     * be kept as its own, dedicated section.
     */
    .requests : {
        KEEP(*(.requests))
    } :rodata

    . = ALIGN(4096);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /* Drop sections that serve no purpose in a freestanding kernel. */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
    }
}
