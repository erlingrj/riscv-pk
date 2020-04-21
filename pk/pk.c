// See LICENSE for license details.

#include "pk.h"
#include "mmap.h"
#include "boot.h"
#include "elf.h"
#include "mtrap.h"
#include "frontend.h"
#include <stdbool.h>

elf_info current;
long disabled_hart_mask;

static void help()
{
  printk("Proxy kernel\n\n");
  printk("usage: pk [pk options] <user program> [program options]\n");
  printk("Options:\n");
  printk("  -h, --help            Print this help message\n");
  printk("  -p                    Disable on-demand program paging\n");
  printk("  -s                    Print cycles upon termination\n");

  shutdown(0);
}

static void suggest_help()
{
  printk("Try 'pk --help' for more information.\n");
  shutdown(1);
}

static void handle_option(const char* arg)
{
  if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
    help();
    return;
  }

  if (strcmp(arg, "-s") == 0) {  // print cycle count upon termination
    current.cycle0 = 1;
    return;
  }

  if (strcmp(arg, "-p") == 0) { // disable demand paging
    demand_paging = 0;
    return;
  }

  panic("unrecognized option: `%s'", arg);
  suggest_help();
}

#define MAX_ARGS 256
typedef union {
  uint64_t buf[MAX_ARGS];
  char* argv[MAX_ARGS];
} arg_buf;

static size_t parse_args(arg_buf* args)
{
  long r = frontend_syscall(SYS_getmainvars, va2pa(args), sizeof(*args), 0, 0, 0, 0, 0);
  if (r != 0)
    panic("args must not exceed %d bytes", (int)sizeof(arg_buf));

  kassert(r == 0);
  uint64_t* pk_argv = &args->buf[1];
  // pk_argv[0] is the proxy kernel itself.  skip it and any flags.
  size_t pk_argc = args->buf[0], arg = 1;
  for ( ; arg < pk_argc && *(char*)(uintptr_t)pk_argv[arg] == '-'; arg++)
    handle_option((const char*)(uintptr_t)pk_argv[arg]);

  for (size_t i = 0; arg + i < pk_argc; i++)
    args->argv[i] = (char*)(uintptr_t)pk_argv[arg + i];
  return pk_argc - arg;
}

static void init_tf(trapframe_t* tf, long pc, long sp)
{
  memset(tf, 0, sizeof(*tf));
  tf->status = (read_csr(sstatus) &~ SSTATUS_SPP &~ SSTATUS_SIE) | SSTATUS_SPIE;
  tf->gpr[2] = sp;
  tf->epc = pc;
}
static void rest_of_boot_loader(uintptr_t kstack_top);

static void init_csrs(){
  //needs to be in machine mode??

  // Init the hpmcounters
  //  Bits [0:7] = Event set
  //  Bits[?:8] = Mask out what events you want to map to that register.
  //  In our case we only map one event to each reg. therefore mphmevent3 => 0b001. mphevent4 => 0b010 etc. All are powers of 2.

  write_csr(mcounteren, -1);
  write_csr(scounteren, -1); // Enable user use of all perf counters
  // general:
  //write_csr(mhpmevent3, 0x2001);// branch misprediction
  //write_csr(mhpmevent4, 0x1C000);// branch resolution for boom - decoded branch/jal/jalr for rocket
  // LSC:
  write_csr(mhpmevent5, 0x103); // Q0 Lane 0
  write_csr(mhpmevent6, 0x203); // Q1 Lane 0
  //write_csr(mhpmevent7, 0x403); // Q2 Lane 0
  //write_csr(mhpmevent8, 0x104); // Q0 Lane 1
  //write_csr(mhpmevent9, 0x204); // Q1 Lane 1
  //write_csr(mhpmevent10, 0x404); // Q2 Lane 1
  // currently configured to 10 performance counters - so up to mhpmevent12

  // continue execution
  enter_supervisor_mode(rest_of_boot_loader, pk_vm_init(), 0);
}
static void read_csrs(){

  // Read the initial value of the CSR regs attached to the counters
  
  current.in_a_q = read_csr(hpmcounter5);
  current.in_b_q = read_csr(hpmcounter6);
  
  //current.branch_misp_0 = read_csr(hpmcounter3);
  //current.branch_res_0 = read_csr(hpmcounter4);
  //current.q0_0_0 = read_csr(hpmcounter5);
  //current.q1_0_0 = read_csr(hpmcounter6);
  //current.q2_0_0 = read_csr(hpmcounter7);
  //current.q0_1_0 = read_csr(hpmcounter8);
  //current.q1_1_0 = read_csr(hpmcounter9);
  //current.q2_1_0 = read_csr(hpmcounter10);
  current.syscall_cnt = 0;
  current.frontend_syscall_cnt = 0;
}

static void run_loaded_program(size_t argc, char** argv, uintptr_t kstack_top)
{
  // copy phdrs to user stack
  size_t stack_top = current.stack_top - current.phdr_size;
  memcpy((void*)stack_top, (void*)current.phdr, current.phdr_size);
  current.phdr = stack_top;

  // copy argv to user stack
  for (size_t i = 0; i < argc; i++) {
    size_t len = strlen((char*)(uintptr_t)argv[i])+1;
    stack_top -= len;
    memcpy((void*)stack_top, (void*)(uintptr_t)argv[i], len);
    argv[i] = (void*)stack_top;
  }

  // copy envp to user stack
  const char* envp[] = {
    // environment goes here
  };
  size_t envc = sizeof(envp) / sizeof(envp[0]);
  for (size_t i = 0; i < envc; i++) {
    size_t len = strlen(envp[i]) + 1;
    stack_top -= len;
    memcpy((void*)stack_top, envp[i], len);
    envp[i] = (void*)stack_top;
  }

  // align stack
  stack_top &= -sizeof(void*);

  struct {
    long key;
    long value;
  } aux[] = {
    {AT_ENTRY, current.entry},
    {AT_PHNUM, current.phnum},
    {AT_PHENT, current.phent},
    {AT_PHDR, current.phdr},
    {AT_PAGESZ, RISCV_PGSIZE},
    {AT_SECURE, 0},
    {AT_RANDOM, stack_top},
    {AT_NULL, 0}
  };

  // place argc, argv, envp, auxp on stack
  #define PUSH_ARG(type, value) do { \
    *((type*)sp) = (type)value; \
    sp += sizeof(type); \
  } while (0)

  #define STACK_INIT(type) do { \
    unsigned naux = sizeof(aux)/sizeof(aux[0]); \
    stack_top -= (1 + argc + 1 + envc + 1 + 2*naux) * sizeof(type); \
    stack_top &= -16; \
    long sp = stack_top; \
    PUSH_ARG(type, argc); \
    for (unsigned i = 0; i < argc; i++) \
      PUSH_ARG(type, argv[i]); \
    PUSH_ARG(type, 0); /* argv[argc] = NULL */ \
    for (unsigned i = 0; i < envc; i++) \
      PUSH_ARG(type, envp[i]); \
    PUSH_ARG(type, 0); /* envp[envc] = NULL */ \
    for (unsigned i = 0; i < naux; i++) { \
      PUSH_ARG(type, aux[i].key); \
      PUSH_ARG(type, aux[i].value); \
    } \
  } while (0)

  STACK_INIT(uintptr_t);

  if (current.cycle0) { // start timer if so requested
    read_csrs();
    current.time0 = rdtime64();
    current.cycle0 = rdcycle64();
    current.instret0 = rdinstret64();
  }

  trapframe_t tf;
  init_tf(&tf, current.entry, stack_top);
  __clear_cache(0, 0);
  write_csr(sscratch, kstack_top);
  start_user(&tf);
}

static void rest_of_boot_loader(uintptr_t kstack_top)
{
  arg_buf args;
  size_t argc = parse_args(&args);
  if (!argc)
    panic("tell me what ELF to load!");

  // load program named by argv[0]
  long phdrs[128];
  current.phdr = (uintptr_t)phdrs;
  current.phdr_size = sizeof(phdrs);
  load_elf(args.argv[0], &current);
  run_loaded_program(argc, args.argv, kstack_top);
}

void boot_loader(uintptr_t dtb)
{
  extern char trap_entry;
  write_csr(stvec, &trap_entry);
  write_csr(sscratch, 0);
  write_csr(sie, 0);
  set_csr(sstatus, SSTATUS_SUM | SSTATUS_FS | SSTATUS_VS);

  file_init();
  enter_machine_mode(init_csrs, 0, 0);
}

void boot_other_hart(uintptr_t dtb)
{
  // stall all harts besides hart 0
  while (1)
    wfi();
}
