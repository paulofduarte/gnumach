/*
 * Copyright (c) 2024 Free Software Foundation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <syscalls.h>
#include <testlib.h>
#include <mach/exception.h>
#include <mach.user.h>
#include <mach_port.user.h>

#if defined(__aarch64__)

/*
 *	aarch64 NEON / VFP equivalent of test_fp_state_getset /
 *	test_xfp_state_getset below.  We use one combined test because
 *	aarch64 doesn't have the i386/i387 vs SSE/XSAVE split — the
 *	whole NEON register file (V0..V31 plus FPCR/FPSR) lives in
 *	struct aarch64_float_state and is fetched in one
 *	thread_get_state(AARCH64_FLOAT_STATE) call.
 *
 *	The shape mirrors the x86 test: the test thread loads known
 *	values into V3 and FPCR, spawns a helper thread, suspends
 *	itself; the helper reads the test thread's saved FP state,
 *	verifies V3 / FPCR are preserved, writes new values into V7
 *	and FPCR, resumes the test thread; the test thread then reads
 *	V7 and FPCR back via inline asm and asserts they reflect the
 *	helper's writes.
 */

#define V3_PATTERN_LO	0x3333333333333333ULL
#define V3_PATTERN_HI	0x3333333333333333ULL
#define V7_PATTERN_LO	0x7777777777777777ULL
#define V7_PATTERN_HI	0x7777777777777777ULL
/* FPCR.RM = 01 (round toward +inf) -- bits 23-22 = 0b01. */
#define FPCR_INIT	0x00400000ULL
/* FPCR.RM = 10 (round toward -inf). */
#define FPCR_MODIFIED	0x00800000ULL

static void thread_fp_getset_aarch64(void *arg)
{
  int err;
  thread_t th = *(thread_t*)arg;

  wait_thread_suspended(th);

  /*
   * thread_setstatus() in aarch64/aarch64/pcb.c checks the address of
   * the supplied state struct is aligned to alignof(struct
   * aarch64_float_state) and rejects with KERN_INVALID_ARGUMENT
   * otherwise.  __int128 inside v[32] gives the struct a 16-byte
   * alignment requirement.  Using a file-scope static (BSS placement,
   * fully aligned) sidesteps the stack-frame layout concerns we'd hit
   * with a function-local — aarch64-unknown-none-elf-gcc doesn't always
   * honour __attribute__((aligned(16))) on locals containing __int128
   * across nested calls.
   */
  static struct aarch64_float_state state;
  mach_msg_type_number_t state_count = AARCH64_FLOAT_STATE_COUNT;

  memset(&state, 0, sizeof(state));
  err = thread_get_state(th, AARCH64_FLOAT_STATE,
                         (thread_state_t) &state, &state_count);
  ASSERT_RET(err, "thread_get_state get failed");
  ASSERT(state_count == AARCH64_FLOAT_STATE_COUNT, "bad state_count");

  /* V3 should match what the test thread loaded. */
  uint64_t v3_lo = (uint64_t) state.v[3];
  uint64_t v3_hi = (uint64_t) (state.v[3] >> 64);
  printf("V3 lo=%016llx hi=%016llx (expected lo=%016llx hi=%016llx)\n",
         (unsigned long long) v3_lo, (unsigned long long) v3_hi,
         (unsigned long long) V3_PATTERN_LO,
         (unsigned long long) V3_PATTERN_HI);
  ASSERT(v3_lo == V3_PATTERN_LO && v3_hi == V3_PATTERN_HI,
         "V3 not preserved across context switch");
  printf("FPCR get=%016llx (expected %016llx)\n",
         (unsigned long long) state.fpcr,
         (unsigned long long) FPCR_INIT);
  ASSERT(state.fpcr == FPCR_INIT, "FPCR not preserved");

  /* Modify V7 and FPCR. */
  state.v[7] = ((__int128) V7_PATTERN_HI << 64) | V7_PATTERN_LO;
  state.fpcr = FPCR_MODIFIED;

  printf("set: state addr=%p count=%u fpsr=%llx fpcr=%llx fpmr=%llx\n",
         &state, state_count,
         (unsigned long long) state.fpsr,
         (unsigned long long) state.fpcr,
         (unsigned long long) state.fpmr);
  err = thread_set_state(th, AARCH64_FLOAT_STATE,
                         (thread_state_t) &state, state_count);
  ASSERT_RET(err, "thread_set_state set failed");

  err = thread_resume(th);
  ASSERT_RET(err, "error in thread_resume");
  thread_terminate(mach_thread_self());
  FAILURE("thread_terminate");
}

static void test_fp_state_getset_aarch64(void)
{
  int err;
  thread_t th = mach_thread_self();

  /* Load known value into V3 and FPCR. */
  uint64_t v3_bytes[2] = { V3_PATTERN_LO, V3_PATTERN_HI };
  asm volatile ("ldr q3, [%0]" :: "r"(v3_bytes) : "v3");
  uint64_t fpcr_init = FPCR_INIT;
  asm volatile ("msr fpcr, %0" :: "r"(fpcr_init));

  /* Spawn helper, then suspend self so helper sees a stable FP state. */
  test_thread_start(mach_task_self(), thread_fp_getset_aarch64, &th);
  err = thread_suspend(th);
  ASSERT_RET(err, "error in thread_suspend");

  /* Check V7 and FPCR have the values the helper set. */
  uint64_t v7_after[2] = { 0, 0 };
  asm volatile ("str q7, [%0]" :: "r"(v7_after) : "memory");
  uint64_t fpcr_after = 0;
  asm volatile ("mrs %0, fpcr" : "=r"(fpcr_after));
  printf("V7 lo=%016llx hi=%016llx\n",
         (unsigned long long) v7_after[0], (unsigned long long) v7_after[1]);
  printf("FPCR after=%016llx (expected %016llx)\n",
         (unsigned long long) fpcr_after,
         (unsigned long long) FPCR_MODIFIED);
  ASSERT(v7_after[0] == V7_PATTERN_LO && v7_after[1] == V7_PATTERN_HI,
         "V7 wasn't correctly set by the helper thread");
  ASSERT(fpcr_after == FPCR_MODIFIED, "FPCR wasn't updated by the helper thread");
}

#endif /* __aarch64__ */

#if defined(__i386__) || defined(__x86_64__)
#include <mach_i386.user.h>

static void printx(struct i386_xfloat_state *state, int size)
{
  printf("xfloat init %d fp %d exc %d\n",
         state->initialized, state->fpkind, state->exc_status);
  struct i386_xfp_save *xfp = (struct i386_xfp_save *) &state->hw_state[0];
  printf("xfp %x %x %x %x %x %x %x %x %x\n",
         xfp->fp_control, xfp->fp_status, xfp->fp_tag, xfp->fp_eip,
         xfp->fp_cs, xfp->fp_opcode, xfp->fp_dp, xfp->fp_ds, xfp->fp_mxcsr);
  for (int i=0; i<8; i++)
    {
      printf("fp%d", i);
      for (int j=0; j<16; j++)
        printf(" %02x", xfp->fp_reg_word[i][j]);
      printf("\n");
    }
  for (int i=0; i<16; i++)
    {
      printf("xmm%02d", i);
      for (int j=0; j<16; j++)
        printf(" %02x", xfp->fp_xreg_word[i][j]);
      printf("\n");
    }
  printf("header xfp_features %llx bv %llx\n",
         xfp->header.xfp_features, xfp->header.xcomp_bv);
  int rem = size - sizeof(*state) - sizeof(struct i386_xfp_save);
  if (rem > 0)
    {
      int iter = 0;
      while (rem > 0)
        {
          const int len = 16;
          int n;
          if (rem > len)
            n = len;
          else
            n = rem;
          printf("ext");
          for (int j=0; j<n; j++)
            printf(" %02x", xfp->extended[j + iter*len]);
          printf("\n");
          rem -= n;
          iter++;
        }
    }
}

static void thread_fp_getset(void *arg)
{
  int err;
  thread_t th = *(thread_t*)arg;

  wait_thread_suspended(th);

  mach_msg_type_number_t state_count = i386_FLOAT_STATE_COUNT;
  struct i386_float_state state;

  memset(&state, 0, sizeof(state));
  err = thread_get_state(th, i386_FLOAT_STATE,
                        (thread_state_t) &state, &state_count);
  ASSERT_RET(err, "thread_get_state get failed");
  ASSERT(state_count == i386_FLOAT_STATE_COUNT, "bad state_count");

  struct i386_fp_regs *fpr =
      (struct i386_fp_regs *) (&state.hw_state[0] + sizeof(struct i386_fp_save));

  printf("fp regs get:\n");
  for (int i=0; i<8; i++)
  {
      printf("fp%d", i);
      for (int j=0; j<5; j++)
          printf(" %04x", fpr->fp_reg_word[i][j]);
      printf("\n");
  }

  char tmp[10];
  memcpy(tmp, &fpr->fp_reg_word[1][0], sizeof(tmp));
  memcpy(&fpr->fp_reg_word[1][0], &fpr->fp_reg_word[0][0], sizeof(tmp));
  memcpy(&fpr->fp_reg_word[0][0], tmp, sizeof(tmp));

  printf("fp regs set:\n");
  for (int i=0; i<8; i++)
  {
      printf("fp%d", i);
      for (int j=0; j<5; j++)
          printf(" %04x", fpr->fp_reg_word[i][j]);
      printf("\n");
  }

  err = thread_set_state(th, i386_FLOAT_STATE,
                        (thread_state_t) &state, state_count);
  ASSERT_RET(err, "thread_set_state set failed");

  err = thread_resume(th);
  ASSERT_RET(err, "error in thread_resume");
  thread_terminate(mach_thread_self());
  FAILURE("thread_terminate");
}

static void test_fp_state_getset()
{
  int err;
  thread_t th = mach_thread_self();

  /* load some known value in FP registers */
  int n1[1] = {1111111111};
  float d2[1] = {123.456};
  asm volatile ("fildl  %0\n"
                "fldl   %1\n"
                :: "m" (n1), "m" (d2) :);

  /* then switch to the get/set test thread, and wait to be resumed */
  test_thread_start(mach_task_self(), thread_fp_getset, &th);
  err = thread_suspend(th);
  ASSERT_RET(err, "error in thread_suspend");

  /* and check that now we have the values swapped in FP registers */
  int m1[1] = {0};
  float f2[1] = {0.0};
  asm volatile ("fistpl %0\n"
                "fstpl %1\n"
                :: "m" (m1), "m" (f2):);
  int fint, fdec;
  fint = (int)f2[0];
  fdec = (int)((f2[0] - fint) * 1000);
  printf("fp %d %d.%03d\n", m1[0], fint, fdec);
  ASSERT(n1[0] == m1[0], "error in moving int value in fp regs");
  ASSERT(f2[0] == d2[0], "error in moving fp value in fp regs");
}


static void thread_xfp_getset(void *arg)
{
  int err;
  thread_t th = *(thread_t*)arg;

  wait_thread_suspended(th);

  vm_size_t xfp_size;
  err = i386_get_xstate_size(host_priv(), &xfp_size);
  ASSERT_RET(err, "i386_get_xstate_size");

  mach_msg_type_number_t state_count = xfp_size / sizeof(integer_t);
  struct i386_xfloat_state *state = __builtin_alloca(xfp_size);
  printf("xfp size %u min %u\n", xfp_size, sizeof(struct i386_xfloat_state));

  memset(state, 0, xfp_size);
  err = thread_get_state(th, i386_XFLOAT_STATE,
                        (thread_state_t) state, &state_count);
  ASSERT_RET(err, "thread_get_state get failed");
  ASSERT(state_count == (xfp_size / sizeof(integer_t)), "bad state_count");
  printx(state, xfp_size);

  struct i386_xfp_save *xfp = (struct i386_xfp_save *) &state->hw_state[0];
  printf("xmm3 (after get)");
  for (int j=0; j<16; j++)
    printf(" %02x", xfp->fp_xreg_word[3][j]);
  printf("\n");
  for (int j=0; j<16; j++)
    ASSERT(xfp->fp_xreg_word[3][j] == 0x33,
           "register xmm3 wasn't correctly retrieved from the getset thread");

  printf("mxcsr (after get) %04x\n", xfp->fp_mxcsr);
  ASSERT(xfp->fp_mxcsr == 0x1f80, "mxcsr wasn't correctly retrieved from the getset thread");

  memset(xfp->fp_xreg_word[7], 0x77, 16);
  xfp->fp_mxcsr = 0x1fa0;

  err = thread_set_state(th, i386_XFLOAT_STATE,
                        (thread_state_t) state, state_count);
  ASSERT_RET(err, "thread_set_state set failed");

  err = thread_resume(th);
  ASSERT_RET(err, "error in thread_resume");
  thread_terminate(mach_thread_self());
  FAILURE("thread_terminate");
}

static void test_xfp_state_getset()
{
  int err;
  thread_t th = mach_thread_self();

  /* load some known value in XMM registers */
  char buf3[16];
  memset(buf3, 0x33, 16);
  asm volatile ("movups (%0),%%xmm3" :: "r" (buf3) :);

  /* And to MXCSR */
  unsigned int mxcsr = 0x1f80;
  asm volatile ("ldmxcsr %0" :: "m" (mxcsr));

  /* then switch to the get/set test thread, and wait to be resumed */
  test_thread_start(mach_task_self(), thread_xfp_getset, &th);
  err = thread_suspend(th);
  ASSERT_RET(err, "error in thread_suspend");

  /* and check that now we have different values in XMM registers */
  char buf7[16];
  memset(buf7, 0, 16);
  asm volatile ("movups %%xmm7,(%0)" :: "r" (buf7) :);

  printf("xmm7 (after set)");
  for (int j=0; j<16; j++)
    printf(" %02x", buf7[j]);
  printf("\n");
  for (int j=0; j<16; j++)
    ASSERT(buf7[j] == 0x77,
           "register xmm7 wasn't correctly set by the getset thread");

  asm volatile ("stmxcsr %0" :: "m" (mxcsr));
  ASSERT(mxcsr == 0x1fa0, "mxcsr wasn't updated");
}
#endif

int main(int argc, char *argv[], int envc, char *envp[])
{
#if defined(__i386__) || defined(__x86_64__)
  test_fp_state_getset();
  test_xfp_state_getset();
#elif defined(__aarch64__)
  test_fp_state_getset_aarch64();
#else
  FAILURE("FP/XSTATE test missing on this arch!");
#endif
  return 0;
}
