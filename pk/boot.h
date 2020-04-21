// See LICENSE for license details.

#ifndef _BOOT_H
#define _BOOT_H

#ifndef __ASSEMBLER__

#include <stddef.h>

typedef struct {
  int phent;
  int phnum;
  int is_supervisor;
  size_t phdr;
  size_t phdr_size;
  size_t bias;
  size_t entry;
  size_t brk_min;
  size_t brk;
  size_t brk_max;
  size_t mmap_max;
  size_t stack_top;
  uint64_t time0;
  uint64_t cycle0;
  uint64_t instret0;
  uint64_t branch_misp_0;
  uint64_t branch_res_0;
  uint64_t in_a_q;
  uint64_t in_b_q;
  //uint64_t q0_0_0;
  //uint64_t q1_0_0;
  //uint64_t q2_0_0;
  //uint64_t q0_1_0;
  //uint64_t q1_1_0;
  //uint64_t q2_1_0;
  uint64_t syscall_cnt;
  uint64_t frontend_syscall_cnt;
  uint64_t frontend_syscall_cycles;
  uint64_t frontend_syscall_instret;
} elf_info;

extern elf_info current;

void load_elf(const char* fn, elf_info* info);

#endif // !__ASSEMBLER__

#endif
