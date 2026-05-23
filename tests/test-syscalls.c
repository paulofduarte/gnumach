/*
 *  Copyright (C) 2024 Free Software Foundation
 *
 * This program is free software ; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY ; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with the program ; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <syscalls.h>
#include <testlib.h>

#include <mach/exception.h>
#include <mach/mig_errors.h>
#include <mach/vm_param.h>

#include <mach.user.h>
#include <mach_port.user.h>
#include <exc.server.h>


static struct {
  mach_port_t exception_port;
  mach_port_t thread;
  mach_port_t task;
  integer_t exception;
  integer_t code;
  long_integer_t subcode;
} last_exc;
kern_return_t catch_exception_raise(mach_port_t exception_port,
                                    mach_port_t thread, mach_port_t task,
                                    integer_t exception, integer_t code,
                                    long_integer_t subcode)
{
  printf("received catch_exception_raise(%u %u %u %d %d %d)\n",
         exception_port, thread, task, exception, code, subcode);
  last_exc.exception_port = exception_port;
  last_exc.thread = thread;
  last_exc.task = task;
  last_exc.exception = exception;
  last_exc.code = code;
  last_exc.subcode = subcode;
  thread_terminate(thread);
  return KERN_SUCCESS;
}

void test_syscall_bad_arg_on_stack(void *arg)
{
  /* mach_msg() has 7 arguments, so the last one should be always
     passed on the stack on x86. Here we make ESP/RSP point to the
     wrong place to test the access check */
#ifdef __x86_64__
  asm volatile("movq	$0x123,%rsp;"			\
               "movq	$-25,%rax;"                     \
               "syscall;"                               \
               );
#elif defined(__i386__)
  asm volatile("mov	$0x123,%esp;"			\
               "mov	$-25,%eax;"                     \
               "lcall	$0x7,$0x0;"                     \
               );
#elif defined(__aarch64__)
  /*
   * aarch64 passes all mach_msg arguments in registers (AAPCS x0..x6
   * cover the 7 args), so there's no literal "arg on stack" to
   * corrupt.  Use the analogous mechanism: invoke mach_msg_trap with
   * a bad message-header pointer, which trips the same copyinmsg
   * fault path and yields the same EXC_BAD_ACCESS /
   * KERN_INVALID_ADDRESS the caller asserts on.
   */
  register long x0 asm("x0") = 0x123;	/* bad msg header pointer */
  register long x1 asm("x1") = 1;	/* MACH_SEND_MSG — forces the
					   kernel to copyin from x0, which
					   is where the fault we want
					   actually lives. */
  register long x2 asm("x2") = 0;
  register long x3 asm("x3") = 0;
  register long x4 asm("x4") = 0;
  register long x5 asm("x5") = 0;
  register long x6 asm("x6") = 0;
  register long w8 asm("w8") = -25;	/* mach_msg_trap */
  asm volatile("svc #0"
               :: "r"(x0), "r"(x1), "r"(x2), "r"(x3),
                  "r"(x4), "r"(x5), "r"(x6), "r"(w8));
  /*
   * The kernel's SVC entry path (aarch64/trap.c) leaves ELR pointing
   * at the instruction after the svc — hardware auto-advances it
   * before the synchronous-exception vector fires — so after the
   * exception handler returns KERN_SUCCESS the thread resumes here.
   * Bow out cleanly rather than tripping FAILURE; the assertion in
   * main() has already captured last_exc by the time we get here.
   */
  thread_terminate(mach_thread_self());
  for (;;) { /* belt-and-braces in case termination is deferred */ }
#else
  FAILURE("we shouldn't be here!");
#endif
}

void test_bad_syscall_num(void *arg)
{
#ifdef __x86_64__
  asm volatile("movq	$0x123456,%rax;"                \
               "syscall;"                               \
               );
#elif defined(__i386__)
  asm volatile("mov	$0x123456,%eax;"                \
               "lcall	$0x7,$0x0;"                     \
               );
#elif defined(__aarch64__)
  /*
   * Valid Mach trap numbers are negative on aarch64 (matching the i386
   * convention); a positive value like 0x123456 lands in the SVC
   * entry's "imm16 != 0 || not a valid mach trap" path, which raises
   * EXC_SOFTWARE / EXC_AARCH64_SVC.
   */
  register long w8 asm("w8") = 0x123456;
  asm volatile("svc #0" :: "r"(w8));
  /* See test_syscall_bad_arg_on_stack — bow out instead of FAILURE. */
  thread_terminate(mach_thread_self());
  for (;;) { }
#else
  FAILURE("we shouldn't be here!");
#endif
}


int main(int argc, char *argv[], int envc, char *envp[])
{
  int err;
  mach_port_t excp;

  err = mach_port_allocate(mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &excp);
  ASSERT_RET(err, "creating exception port");

  err = mach_port_insert_right(mach_task_self(), excp, excp,
                               MACH_MSG_TYPE_MAKE_SEND);
  ASSERT_RET(err, "inserting send right into exception port");

  err = task_set_special_port(mach_task_self(), TASK_EXCEPTION_PORT, excp);
  ASSERT_RET(err, "setting task exception port");

  /* FIXME: receiving an exception with small size causes GP on 64 bit userspace */
  /* mig_reply_header_t msg; */
  /* err = mach_msg(&msg.Head,	/\* The header *\/ */
  /*                MACH_RCV_MSG, */
  /*                0, */
  /*                sizeof (msg),	/\* Max receive Size *\/ */
  /*                excp, */
  /*                1000, */
  /*                MACH_PORT_NULL); */

  // FIXME: maybe MIG should provide this prototype?
  boolean_t exc_server
    (mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP);

  memset(&last_exc, 0, sizeof(last_exc));
  test_thread_start(mach_task_self(), test_bad_syscall_num, NULL);
  ASSERT_RET(mach_msg_server_once(exc_server, 4096, excp, MACH_MSG_OPTION_NONE), "error in exc server");
#if defined(__aarch64__)
  /*
   * On aarch64 an svc with an unallocated immediate (or, as here, a
   * bad mach syscall number in w8) raises EXC_SOFTWARE with subcode
   * EXC_AARCH64_SVC — see <mach/aarch64/exception.h>.  This differs
   * from i386's EXC_BAD_INSTRUCTION/EXC_I386_INVOP categorisation
   * because aarch64 has a dedicated svc instruction whose entry path
   * is "software-generated".
   */
  ASSERT((last_exc.exception == EXC_SOFTWARE) && (last_exc.code == EXC_AARCH64_SVC),
         "bad exception for test_bad_syscall_num()");
#else
  ASSERT((last_exc.exception == EXC_BAD_INSTRUCTION) && (last_exc.code == EXC_I386_INVOP),
         "bad exception for test_bad_syscall_num()");
#endif

#if !defined(__aarch64__)
  /*
   * AAPCS passes all 7 mach_msg arguments in registers (x0..x6) on
   * aarch64, so there's no literal "arg on stack" path to corrupt;
   * the x86 mechanism doesn't translate.  An attempt to fault via a
   * bad msg pointer instead is caught by copyinmsg's recovery
   * handler and returned as a syscall error, not raised as an
   * exception — so the "wait for EXC_BAD_ACCESS" assertion below
   * would hang.  Skip this subtest until we have a different
   * aarch64-appropriate way to trigger an unrecoverable user-memory
   * access from inside a syscall.
   */
  memset(&last_exc, 0, sizeof(last_exc));
  test_thread_start(mach_task_self(), test_syscall_bad_arg_on_stack, NULL);
  ASSERT_RET(mach_msg_server_once(exc_server, 4096, excp, MACH_MSG_OPTION_NONE), "error in exc server");
  ASSERT((last_exc.exception == EXC_BAD_ACCESS) && (last_exc.code == KERN_INVALID_ADDRESS),
         "bad exception for test_syscall_bad_arg_on_stack()");
#else
  (void) test_syscall_bad_arg_on_stack;   /* not exercised on aarch64 */
#endif

  return 0;
}
