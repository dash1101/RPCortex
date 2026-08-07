/* Fold a multi-file package into one relocatable object with FOUR sections.
 *
 * Used with `ld -r --force-group-allocation`. Without it, a C++ package grows
 * ELF sections faster than it grows code, and the loader accepts 128.
 *
 * Two things produce them, and neither is under the package author's control:
 *
 *   COMDAT groups. An inline or implicitly-instantiated function is emitted as
 *   a weak definition in its own section plus a .group section describing it —
 *   TWO sections per weak symbol, so that a later link can pick one copy and
 *   discard the rest. A package is never linked again, so the whole mechanism
 *   is dead weight. --force-group-allocation takes the members out of their
 *   groups; the wildcards below then collect them.
 *
 *   Per-symbol sections. Vtables, string literal merge sections and anything
 *   else the compiler decides to keep separable all arrive as .text.<mangled>,
 *   .rodata.str1.1 and so on.
 *
 * With one class the difference is nothing. With fifty screen classes it is the
 * difference between a package that loads and one that does not, and the failure
 * shows up as "too many sections" naming nothing that appears in the source.
 *
 * .rpc_app_header is deliberately NOT mentioned. It is an orphan and ld keeps it
 * as its own section, which is what the loader looks for by name. Folding it
 * into .rodata would leave the header unfindable and the package refused.
 */
SECTIONS
{
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.* COMMON) }
}
