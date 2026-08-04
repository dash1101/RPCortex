// Just enough ELF32 to load a relocatable object. No general-purpose parser.
#ifndef RPC_ELF_H
#define RPC_ELF_H

#include <stdint.h>

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info, sh_addralign, sh_entsize;
} Elf32_Shdr;

typedef struct {
    uint32_t st_name, st_value, st_size;
    unsigned char st_info, st_other;
    uint16_t st_shndx;
} Elf32_Sym;

typedef struct { uint32_t r_offset, r_info; } Elf32_Rel;

#define ET_REL          1
#define EM_ARM          40

#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_NOBITS      8
#define SHT_REL         9

// SHF_WRITE is what sorts an app's sections into the half it may write to and
// the half it may not — .text and .rodata lack it, .data and .bss carry it.
#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4

#define SHN_UNDEF       0
#define SHN_ABS         0xfff1
#define SHN_COMMON      0xfff2

#define ELF32_R_SYM(i)  ((i) >> 8)
#define ELF32_R_TYPE(i) ((unsigned char)(i))
#define ELF32_ST_TYPE(i) ((i) & 0xf)
#define STT_FUNC        2

// The relocation types that matter. Measured, not guessed: GCC 14.2 targeting
// Cortex-M33 with -Os emits exactly ABS32, THM_CALL and THM_JUMP24 for real C++
// (it materialises addresses through literal pools rather than movw/movt, which
// is why the MOVW/MOVT pair does not appear). The rest are handled because they
// are cheap to support and turn up with other flags or with unwind tables.
#define R_ARM_NONE            0
#define R_ARM_ABS32           2
#define R_ARM_REL32           3
#define R_ARM_THM_CALL       10
#define R_ARM_ABS16          16
#define R_ARM_ABS8           17
#define R_ARM_THM_JUMP24     30
#define R_ARM_TARGET1        38
#define R_ARM_PREL31         42
#define R_ARM_THM_MOVW_ABS_NC 47
#define R_ARM_THM_MOVT_ABS   48
#define R_ARM_THM_JUMP11     102

const char *elf_reloc_name(uint32_t type);

#endif  // RPC_ELF_H
