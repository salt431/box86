#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>
#include <sys/mman.h>

#include "box86version.h"
#include "elfloader.h"
#include "debug.h"
#include "elfload_dump.h"
#include "elfloader_private.h"

#ifndef PN_XNUM 
#define PN_XNUM (0xffff)
#endif

int LoadSH(FILE *f, Elf32_Shdr *s, void** SH, const char* name, uint32_t type)
{
    if(type && (s->sh_type != type)) {
        printf_debug(DEBUG_INFO, "Section Header \"%s\" (off=%d, size=%d) has incorect type (%d != %d)\n", name, s->sh_offset, s->sh_size, s->sh_type, type);
        return -1;
    }
    if (type==SHT_SYMTAB && s->sh_size%sizeof(Elf32_Sym)) {
        printf_debug(DEBUG_INFO, "Section Header \"%s\" (off=%d, size=%d) has size (not multiple of %d)\n", name, s->sh_offset, s->sh_size, sizeof(Elf32_Sym));
    }
    *SH = calloc(1, s->sh_size);
    fseek(f, s->sh_offset ,SEEK_SET);
    if(fread(*SH, s->sh_size, 1, f)!=1) {
            printf_debug(DEBUG_INFO, "Cannot read Section Header \"%s\" (off=%d, size=%d)\n", name, s->sh_offset, s->sh_size);
            return -1;
    }

    return 0;
}

int FindSection(Elf32_Shdr *s, int n, char* SHStrTab, const char* name)
{
    for (int i=0; i<n; ++i) {
        if(s[i].sh_type!=SHT_NULL)
            if(!strcmp(SHStrTab+s[i].sh_name, name))
                return i;
    }
    return 0;
}

void LoadNamedSection(FILE *f, Elf32_Shdr *s, int size, char* SHStrTab, const char* name, const char* clearname, uint32_t type, void** what, int* num)
{
    int n = FindSection(s, size, SHStrTab, name);
    printf_debug(DEBUG_DEBUG, "Loading %s (idx = %d)\n", clearname, n);
    if(n)
        LoadSH(f, s+n, what, name, type);
    if(type==SHT_SYMTAB || type==SHT_DYNSYM) {
        if(*what && num)
            *num = s[n].sh_size / sizeof(Elf32_Sym);
    } else if(type==SHT_DYNAMIC) {
        if(*what && num)
            *num = s[n].sh_size / sizeof(Elf32_Dyn);
    }
}

void* LoadAndCheckElfHeader(FILE* f, const char* name, int exec)
{
    Elf32_Ehdr header;
    if(fread(&header, sizeof(Elf32_Ehdr), 1, f)!=1) {
        printf_debug(DEBUG_INFO, "Cannot read ELF Header\n");
        return NULL;
    }
    if(memcmp(header.e_ident, ELFMAG, SELFMAG)!=0) {
        printf_debug(DEBUG_INFO, "Not an ELF file (sign=%c%c%c%c)\n", header.e_ident[0], header.e_ident[1], header.e_ident[2], header.e_ident[3]);
        return NULL;
    }
    if(header.e_ident[EI_CLASS]!=ELFCLASS32) {
        printf_debug(DEBUG_INFO, "Not an 32bits ELF (%d)\n", header.e_ident[EI_CLASS]);
        return NULL;
    }
    if(header.e_ident[EI_DATA]!=ELFDATA2LSB) {
        printf_debug(DEBUG_INFO, "Not an LittleEndian ELF (%d)\n", header.e_ident[EI_DATA]);
        return NULL;
    }
    if(header.e_ident[EI_VERSION]!=EV_CURRENT) {
        printf_debug(DEBUG_INFO, "Incorrect ELF version (%d)\n", header.e_ident[EI_VERSION]);
        return NULL;
    }
    if(header.e_ident[EI_OSABI]!=ELFOSABI_LINUX && header.e_ident[EI_OSABI]!=ELFOSABI_NONE && header.e_ident[EI_OSABI]!=ELFOSABI_SYSV) {
        printf_debug(DEBUG_INFO, "Not a Linux ELF (%d)\n",header.e_ident[EI_OSABI]);
        return NULL;
    }

    if(exec) {
        if(header.e_type != ET_EXEC) {
            printf_debug(DEBUG_INFO, "Not an Executable (%d)\n", header.e_type);
            return NULL;
        }
    } else {
        if(header.e_type != ET_DYN) {
            printf_debug(DEBUG_INFO, "Not an Library (%d)\n", header.e_type);
            return NULL;
        }
    }

    if(header.e_machine != EM_386) {
        printf_debug(DEBUG_INFO, "Not an i386 ELF (%d)\n", header.e_machine);
        return NULL;
    }

    if(header.e_entry == 0) {
        printf_debug(DEBUG_INFO, "No entry point in ELF\n");
        return NULL;
    }
    if(header.e_phentsize != sizeof(Elf32_Phdr)) {
        printf_debug(DEBUG_INFO, "Program Header Entry size incorrect (%d != %d)\n", header.e_phentsize, sizeof(Elf32_Phdr));
        return NULL;
    }
    if(header.e_shentsize != sizeof(Elf32_Shdr)) {
        printf_debug(DEBUG_INFO, "Section Header Entry size incorrect (%d != %d)\n", header.e_shentsize, sizeof(Elf32_Shdr));
        return NULL;
    }

    elfheader_t *h = calloc(1, sizeof(elfheader_t));
    h->name = strdup(name);
    h->numPHEntries = header.e_phnum;
    h->numSHEntries = header.e_shnum;
    h->SHIdx = header.e_shstrndx;
    // special cases for nums
    if(h->numSHEntries == 0) {
        printf_debug(DEBUG_DEBUG, "Read number of Sections in 1st Section\n");
        // read 1st section header and grab actual number from here
        fseek(f, header.e_shoff, SEEK_SET);
        Elf32_Shdr section;
        if(fread(&section, sizeof(Elf32_Shdr), 1, f)!=1) {
            free(h);
            printf_debug(DEBUG_INFO, "Cannot read Initial Section Header\n");
            return NULL;
        }
        h->numSHEntries = section.sh_size;
    }
    // now read all section headers
    printf_debug(DEBUG_DEBUG, "Read %d Section header\n", h->numSHEntries);
    h->SHEntries = (Elf32_Shdr*)calloc(h->numSHEntries, sizeof(Elf32_Shdr));
    fseek(f, header.e_shoff ,SEEK_SET);
    if(fread(h->SHEntries, sizeof(Elf32_Shdr), h->numSHEntries, f)!=h->numSHEntries) {
            FreeElfHeader(&h);
            printf_debug(DEBUG_INFO, "Cannot read all Section Header\n");
            return NULL;
    }

    if(h->numPHEntries == PN_XNUM) {
        printf_debug(DEBUG_DEBUG, "Read number of Program Header in 1st Section\n");
        // read 1st section header and grab actual number from here
        h->numPHEntries = h->SHEntries[0].sh_info;
    }

    printf_debug(DEBUG_DEBUG, "Read %d Program header\n", h->numPHEntries);
    h->PHEntries = (Elf32_Phdr*)calloc(h->numPHEntries, sizeof(Elf32_Phdr));
    fseek(f, header.e_phoff ,SEEK_SET);
    if(fread(h->PHEntries, sizeof(Elf32_Phdr), h->numPHEntries, f)!=h->numPHEntries) {
            FreeElfHeader(&h);
            printf_debug(DEBUG_INFO, "Cannot read all Program Header\n");
            return NULL;
    }

    if(h->SHIdx == SHN_XINDEX) {
        printf_debug(DEBUG_DEBUG, "Read number of String Table in 1st Section\n");
        h->SHIdx = h->SHEntries[0].sh_link;
    }
    if(h->SHIdx > h->numSHEntries) {
        printf_debug(DEBUG_INFO, "Incoherent Section String Table Index : %d / %d\n", h->SHIdx, h->numSHEntries);
        FreeElfHeader(&h);
        return NULL;
    }
    // load Section table
    printf_debug(DEBUG_DEBUG, "Loading Sections Table String (idx = %d)\n", h->SHIdx);
    if(LoadSH(f, h->SHEntries+h->SHIdx, (void*)&h->SHStrTab, ".shstrtab", SHT_STRTAB)) {
        FreeElfHeader(&h);
        return NULL;
    }
    if(box86_debug>=DEBUG_DUMP) DumpMainHeader(&header, h);

    LoadNamedSection(f, h->SHEntries, h->numSHEntries, h->SHStrTab, ".strtab", "SymTab Strings", SHT_STRTAB, (void**)&h->StrTab, NULL);
    LoadNamedSection(f, h->SHEntries, h->numSHEntries, h->SHStrTab, ".symtab", "SymTab", SHT_SYMTAB, (void**)&h->SymTab, &h->numSymTab);
    if(box86_debug>=DEBUG_DUMP && h->SymTab) DumpSymTab(h);

    LoadNamedSection(f, h->SHEntries, h->numSHEntries, h->SHStrTab, ".dynamic", "Dynamic", SHT_DYNAMIC, (void**)&h->Dynamic, &h->numDynamic);
    if(box86_debug>=DEBUG_DUMP && h->Dynamic) DumpDynamicSections(h);

    LoadNamedSection(f, h->SHEntries, h->numSHEntries, h->SHStrTab, ".dynstr", "DynSym Strings", SHT_STRTAB, (void**)&h->DynStr, NULL);
    LoadNamedSection(f, h->SHEntries, h->numSHEntries, h->SHStrTab, ".dynsym", "DynSym", SHT_DYNSYM, (void**)&h->DynSym, &h->numDynSym);
    if(box86_debug>=DEBUG_DUMP && h->DynSym) DumpDynSym(h);

    return h;
}

void FreeElfHeader(elfheader_t** head)
{
    if(!head || !*head)
        return;
    elfheader_t *h = *head;
    free(h->name);
    free(h->PHEntries);
    free(h->SHEntries);
    free(h->SHStrTab);
    free(h->StrTab);
    free(h->DynStr);
    free(h->SymTab);
    free(h->DynSym);
    free(h->memory);
    free(h);

    *head = NULL;
}

int CalcLoadAddr(elfheader_t* head)
{
    head->memsz = 0;
    head->paddr = head->vaddr = (uintptr_t)~0;
    head->align = 1;
    for (int i=0; i<head->numPHEntries; ++i)
        if(head->PHEntries[i].p_type == PT_LOAD) {
            if(head->paddr > head->PHEntries[i].p_paddr)
                head->paddr = head->PHEntries[i].p_paddr;
            if(head->vaddr > head->PHEntries[i].p_vaddr)
                head->vaddr = head->PHEntries[i].p_vaddr;
        }
    
    if(head->vaddr==~0 || head->paddr==~0) {
        printf_debug(DEBUG_NONE, "Error: v/p Addr for Elf Load not set\n");
        return 1;
    }

    head->stacksz = 1024*1024;          //1M stack size default?
    head->stackalign = 4;   // default align for stack
    for (int i=0; i<head->numPHEntries; ++i) {
        if(head->PHEntries[i].p_type == PT_LOAD) {
            uintptr_t phend = head->PHEntries[i].p_vaddr - head->vaddr + head->PHEntries[i].p_memsz;
            if(phend > head->memsz)
                head->memsz = phend;
            if(head->PHEntries[i].p_align > head->align)
                head->align = head->PHEntries[i].p_align;
        }
        if(head->PHEntries[i].p_type == PT_GNU_STACK) {
            if(head->stacksz < head->PHEntries[i].p_memsz)
                head->stacksz = head->PHEntries[i].p_memsz;
            if(head->stackalign < head->PHEntries[i].p_align)
                head->stackalign = head->PHEntries[i].p_align;
        }
    }
    printf_debug(DEBUG_DEBUG, "Elf Addr(v/p)=%p/%p Memsize=%u (align=%u)\n", head->vaddr, head->paddr, head->memsz, head->align);
    printf_debug(DEBUG_DEBUG, "Elf Stack Memsize=%u (align=%u)\n", head->stacksz, head->stackalign);

    return 0;
}

int AllocElfMemory(elfheader_t* head)
{
    printf_debug(DEBUG_DEBUG, "Allocating memory for Elf \"%s\"\n", head->name);
    if (posix_memalign((void**)&head->memory, head->align, head->memsz)) {
        printf_debug(DEBUG_NONE, "Cannot allocate aligned memory (%u/%d) for elf \"%s\"\n", head->memsz, head->align, head->name);
        return 1;
    }
    printf_debug(DEBUG_DEBUG, "Address is %p\n", head->memory);
    printf_debug(DEBUG_DEBUG, "And setting memory access to PROT_READ | PROT_WRITE | PROT_EXEC\n");
    if (mprotect(head->memory, head->memsz, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        printf_debug(DEBUG_NONE, "Cannot protect memory for elf \"%s\"\n", head->name);
        // memory protect error not fatal for now....
    }

    return 0;
}