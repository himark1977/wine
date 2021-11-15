/*
 * ARM signal handling routines
 *
 * Copyright 2002 Marcus Meissner, SuSE Linux AG
 * Copyright 2010-2013, 2015 André Hentschel
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#ifdef __arm__

#include "config.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef HAVE_SYSCALL_H
# include <syscall.h>
#else
# ifdef HAVE_SYS_SYSCALL_H
#  include <sys/syscall.h>
# endif
#endif
#ifdef HAVE_SYS_SIGNAL_H
# include <sys/signal.h>
#endif
#ifdef HAVE_SYS_UCONTEXT_H
# include <sys/ucontext.h>
#endif
#ifdef HAVE_LIBUNWIND
# define UNW_LOCAL_ONLY
# include <libunwind.h>
#endif

#define NONAMELESSUNION
#define NONAMELESSSTRUCT
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/asm.h"
#include "unix_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(seh);


/***********************************************************************
 * signal context platform-specific definitions
 */
#ifdef linux

#if defined(__ANDROID__) && !defined(HAVE_SYS_UCONTEXT_H)
typedef struct ucontext
{
    unsigned long uc_flags;
    struct ucontext *uc_link;
    stack_t uc_stack;
    struct sigcontext uc_mcontext;
    sigset_t uc_sigmask;
    unsigned long uc_regspace[128] __attribute__((__aligned__(8)));
} ucontext_t;
#endif

/* All Registers access - only for local access */
# define REG_sig(reg_name, context) ((context)->uc_mcontext.reg_name)
# define REGn_sig(reg_num, context) ((context)->uc_mcontext.arm_r##reg_num)

/* Special Registers access  */
# define SP_sig(context)            REG_sig(arm_sp, context)    /* Stack pointer */
# define LR_sig(context)            REG_sig(arm_lr, context)    /* Link register */
# define PC_sig(context)            REG_sig(arm_pc, context)    /* Program counter */
# define CPSR_sig(context)          REG_sig(arm_cpsr, context)  /* Current State Register */
# define IP_sig(context)            REG_sig(arm_ip, context)    /* Intra-Procedure-call scratch register */
# define FP_sig(context)            REG_sig(arm_fp, context)    /* Frame pointer */

/* Exceptions */
# define ERROR_sig(context)         REG_sig(error_code, context)
# define TRAP_sig(context)          REG_sig(trap_no, context)

struct extended_ctx
{
    unsigned long magic;
    unsigned long size;
};

struct vfp_sigframe
{
    struct extended_ctx ctx;
    unsigned long long fpregs[32];
    unsigned long fpscr;
};

static void *get_extended_sigcontext( const ucontext_t *sigcontext, unsigned int magic )
{
    struct extended_ctx *ctx = (struct extended_ctx *)sigcontext->uc_regspace;
    while ((char *)ctx < (char *)(sigcontext + 1) && ctx->magic && ctx->size)
    {
        if (ctx->magic == magic) return ctx;
        ctx = (struct extended_ctx *)((char *)ctx + ctx->size);
    }
    return NULL;
}

static void save_fpu( CONTEXT *context, const ucontext_t *sigcontext )
{
    struct vfp_sigframe *frame = get_extended_sigcontext( sigcontext, 0x56465001 );

    if (!frame) return;
    memcpy( context->u.D, frame->fpregs, sizeof(context->u.D) );
    context->Fpscr = frame->fpscr;
}

static void restore_fpu( const CONTEXT *context, ucontext_t *sigcontext )
{
    struct vfp_sigframe *frame = get_extended_sigcontext( sigcontext, 0x56465001 );

    if (!frame) return;
    memcpy( frame->fpregs, context->u.D, sizeof(context->u.D) );
    frame->fpscr = context->Fpscr;
}

#elif defined(__FreeBSD__)

/* All Registers access - only for local access */
# define REGn_sig(reg_num, context) ((context)->uc_mcontext.__gregs[reg_num])

/* Special Registers access  */
# define SP_sig(context)            REGn_sig(_REG_SP, context)    /* Stack pointer */
# define LR_sig(context)            REGn_sig(_REG_LR, context)    /* Link register */
# define PC_sig(context)            REGn_sig(_REG_PC, context)    /* Program counter */
# define CPSR_sig(context)          REGn_sig(_REG_CPSR, context)  /* Current State Register */
# define IP_sig(context)            REGn_sig(_REG_R12, context)   /* Intra-Procedure-call scratch register */
# define FP_sig(context)            REGn_sig(_REG_FP, context)    /* Frame pointer */

static void save_fpu( CONTEXT *context, const ucontext_t *sigcontext ) { }
static void restore_fpu( const CONTEXT *context, ucontext_t *sigcontext ) { }

#endif /* linux */

enum arm_trap_code
{
    TRAP_ARM_UNKNOWN    = -1,  /* Unknown fault (TRAP_sig not defined) */
    TRAP_ARM_PRIVINFLT  =  6,  /* Invalid opcode exception */
    TRAP_ARM_PAGEFLT    = 14,  /* Page fault */
    TRAP_ARM_ALIGNFLT   = 17,  /* Alignment check exception */
};

struct syscall_frame
{
    DWORD                 r0;             /* 000 */
    DWORD                 r1;             /* 004 */
    DWORD                 r2;             /* 008 */
    DWORD                 r3;             /* 00c */
    DWORD                 r4;             /* 010 */
    DWORD                 r5;             /* 014 */
    DWORD                 r6;             /* 018 */
    DWORD                 r7;             /* 01c */
    DWORD                 r8;             /* 020 */
    DWORD                 r9;             /* 024 */
    DWORD                 r10;            /* 028 */
    DWORD                 r11;            /* 02c */
    DWORD                 r12;            /* 030 */
    DWORD                 pc;             /* 034 */
    DWORD                 sp;             /* 038 */
    DWORD                 lr;             /* 03c */
    DWORD                 cpsr;           /* 040 */
    DWORD                 restore_flags;  /* 044 */
    DWORD                 fpscr;          /* 048 */
    struct syscall_frame *prev_frame;     /* 04c */
    SYSTEM_SERVICE_TABLE *syscall_table;  /* 050 */
    DWORD                 align[3];       /* 054 */
    ULONGLONG             d[32];          /* 060 */
};

C_ASSERT( sizeof( struct syscall_frame ) == 0x160);

struct arm_thread_data
{
    void                 *exit_frame;    /* 1d4 exit frame pointer */
    struct syscall_frame *syscall_frame; /* 1d8 frame pointer on syscall entry */
};

C_ASSERT( sizeof(struct arm_thread_data) <= sizeof(((struct ntdll_thread_data *)0)->cpu_data) );
C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct arm_thread_data, exit_frame ) == 0x1d4 );
C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct arm_thread_data, syscall_frame ) == 0x1d8 );

static inline struct arm_thread_data *arm_thread_data(void)
{
    return (struct arm_thread_data *)ntdll_get_thread_data()->cpu_data;
}

static BOOL is_inside_syscall( ucontext_t *sigcontext )
{
    return ((char *)SP_sig(sigcontext) >= (char *)ntdll_get_thread_data()->kernel_stack &&
            (char *)SP_sig(sigcontext) <= (char *)arm_thread_data()->syscall_frame);
}

extern void raise_func_trampoline( EXCEPTION_RECORD *rec, CONTEXT *context, void *dispatcher );

/***********************************************************************
 *           unwind_builtin_dll
 */
NTSTATUS CDECL unwind_builtin_dll( ULONG type, struct _DISPATCHER_CONTEXT *dispatch, CONTEXT *context )
{
#ifdef HAVE_LIBUNWIND
    DWORD ip = context->Pc - (dispatch->ControlPcIsUnwound ? 2 : 0);
    unw_context_t unw_context;
    unw_cursor_t cursor;
    unw_proc_info_t info;
    int rc, i;

    for (i = 0; i <= 12; i++)
        unw_context.regs[i] = (&context->R0)[i];
    unw_context.regs[13] = context->Sp;
    unw_context.regs[14] = context->Lr;
    unw_context.regs[15] = context->Pc;
    rc = unw_init_local( &cursor, &unw_context );

    if (rc != UNW_ESUCCESS)
    {
        WARN( "setup failed: %d\n", rc );
        return STATUS_INVALID_DISPOSITION;
    }
    rc = unw_get_proc_info( &cursor, &info );
    if (rc != UNW_ESUCCESS && rc != -UNW_ENOINFO)
    {
        WARN( "failed to get info: %d\n", rc );
        return STATUS_INVALID_DISPOSITION;
    }
    if (rc == -UNW_ENOINFO || ip < info.start_ip || ip > info.end_ip)
    {
        NTSTATUS status = context->Pc != context->Lr ?
                          STATUS_SUCCESS : STATUS_INVALID_DISPOSITION;
        TRACE( "no info found for %x ip %x-%x, %s\n",
               ip, info.start_ip, info.end_ip, status == STATUS_SUCCESS ?
               "assuming leaf function" : "error, stuck" );
        dispatch->LanguageHandler = NULL;
        dispatch->EstablisherFrame = context->Sp;
        context->Pc = context->Lr;
        context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
        return status;
    }

    TRACE( "ip %#x function %#lx-%#lx personality %#lx lsda %#lx fde %#lx\n",
           ip, (unsigned long)info.start_ip, (unsigned long)info.end_ip, (unsigned long)info.handler,
           (unsigned long)info.lsda, (unsigned long)info.unwind_info );

    rc = unw_step( &cursor );
    if (rc < 0)
    {
        WARN( "failed to unwind: %d %d\n", rc, UNW_ENOINFO );
        return STATUS_INVALID_DISPOSITION;
    }

    dispatch->LanguageHandler  = (void *)info.handler;
    dispatch->HandlerData      = (void *)info.lsda;
    dispatch->EstablisherFrame = context->Sp;

    for (i = 0; i <= 12; i++)
        unw_get_reg( &cursor, UNW_ARM_R0 + i, (unw_word_t *)&(&context->R0)[i] );
    unw_get_reg( &cursor, UNW_ARM_R13, (unw_word_t *)&context->Sp );
    unw_get_reg( &cursor, UNW_ARM_R14, (unw_word_t *)&context->Lr );
    unw_get_reg( &cursor, UNW_REG_IP,  (unw_word_t *)&context->Pc );
    context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;

    if ((info.start_ip & ~(unw_word_t)1) ==
        ((unw_word_t)raise_func_trampoline & ~(unw_word_t)1)) {
        /* raise_func_trampoline stores the original Lr at the bottom of the
         * stack. The unwinder normally can't restore both Pc and Lr to
         * individual values, thus do that manually here.
         * (The function we unwind to might be a leaf function that hasn't
         * backed up its own original Lr value on the stack.) */
        const DWORD *orig_lr = (const DWORD *) dispatch->EstablisherFrame;
        context->Lr = *orig_lr;
    }

    TRACE( "next function pc=%08x%s\n", context->Pc, rc ? "" : " (last frame)" );
    TRACE("  r0=%08x  r1=%08x  r2=%08x  r3=%08x\n",
          context->R0, context->R1, context->R2, context->R3 );
    TRACE("  r4=%08x  r5=%08x  r6=%08x  r7=%08x\n",
          context->R4, context->R5, context->R6, context->R7 );
    TRACE("  r8=%08x  r9=%08x r10=%08x r11=%08x\n",
          context->R8, context->R9, context->R10, context->R11 );
    TRACE(" r12=%08x  sp=%08x  lr=%08x  pc=%08x\n",
          context->R12, context->Sp, context->Lr, context->Pc );
    return STATUS_SUCCESS;
#else
    ERR("libunwind not available, unable to unwind\n");
    return STATUS_INVALID_DISPOSITION;
#endif
}


/***********************************************************************
 *           get_trap_code
 *
 * Get the trap code for a signal.
 */
static inline enum arm_trap_code get_trap_code( int signal, const ucontext_t *sigcontext )
{
#ifdef TRAP_sig
    enum arm_trap_code trap = TRAP_sig(sigcontext);
    if (trap)
        return trap;
#endif

    switch (signal)
    {
    case SIGILL:
        return TRAP_ARM_PRIVINFLT;
    case SIGSEGV:
        return TRAP_ARM_PAGEFLT;
    case SIGBUS:
        return TRAP_ARM_ALIGNFLT;
    default:
        return TRAP_ARM_UNKNOWN;
    }
}


/***********************************************************************
 *           get_error_code
 *
 * Get the error code for a signal.
 */
static inline WORD get_error_code( const ucontext_t *sigcontext )
{
#ifdef ERROR_sig
    return ERROR_sig(sigcontext);
#else
    return 0;
#endif
}


/***********************************************************************
 *           save_context
 *
 * Set the register values from a sigcontext.
 */
static void save_context( CONTEXT *context, const ucontext_t *sigcontext )
{
#define C(x) context->R##x = REGn_sig(x,sigcontext)
    /* Save normal registers */
    C(0); C(1); C(2); C(3); C(4); C(5); C(6); C(7); C(8); C(9); C(10);
#undef C

    context->ContextFlags = CONTEXT_FULL;
    context->Sp   = SP_sig(sigcontext);   /* Stack pointer */
    context->Lr   = LR_sig(sigcontext);   /* Link register */
    context->Pc   = PC_sig(sigcontext);   /* Program Counter */
    context->Cpsr = CPSR_sig(sigcontext); /* Current State Register */
    context->R11  = FP_sig(sigcontext);   /* Frame pointer */
    context->R12  = IP_sig(sigcontext);   /* Intra-Procedure-call scratch register */
    if (CPSR_sig(sigcontext) & 0x20) context->Pc |= 1;  /* Thumb mode */
    save_fpu( context, sigcontext );
}


/***********************************************************************
 *           restore_context
 *
 * Build a sigcontext from the register values.
 */
static void restore_context( const CONTEXT *context, ucontext_t *sigcontext )
{
#define C(x)  REGn_sig(x,sigcontext) = context->R##x
    /* Restore normal registers */
    C(0); C(1); C(2); C(3); C(4); C(5); C(6); C(7); C(8); C(9); C(10);
#undef C

    SP_sig(sigcontext)   = context->Sp;   /* Stack pointer */
    LR_sig(sigcontext)   = context->Lr;   /* Link register */
    PC_sig(sigcontext)   = context->Pc;   /* Program Counter */
    CPSR_sig(sigcontext) = context->Cpsr; /* Current State Register */
    FP_sig(sigcontext)   = context->R11;  /* Frame pointer */
    IP_sig(sigcontext)   = context->R12;  /* Intra-Procedure-call scratch register */
    if (PC_sig(sigcontext) & 1) CPSR_sig(sigcontext) |= 0x20;
    else CPSR_sig(sigcontext) &= ~0x20;
    restore_fpu( context, sigcontext );
}


/***********************************************************************
 *           signal_set_full_context
 */
NTSTATUS signal_set_full_context( CONTEXT *context )
{
    NTSTATUS status = NtSetContextThread( GetCurrentThread(), context );

    if (!status && (context->ContextFlags & CONTEXT_INTEGER) == CONTEXT_INTEGER)
        arm_thread_data()->syscall_frame->restore_flags |= CONTEXT_INTEGER;
    return status;
}


/***********************************************************************
 *              get_native_context
 */
void *get_native_context( CONTEXT *context )
{
    return context;
}


/***********************************************************************
 *              get_wow_context
 */
void *get_wow_context( CONTEXT *context )
{
    return NULL;
}


/***********************************************************************
 *              NtSetContextThread  (NTDLL.@)
 *              ZwSetContextThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtSetContextThread( HANDLE handle, const CONTEXT *context )
{
    NTSTATUS ret;
    struct syscall_frame *frame = arm_thread_data()->syscall_frame;
    DWORD flags = context->ContextFlags & ~CONTEXT_ARM;
    BOOL self = (handle == GetCurrentThread());

    if (!self)
    {
        ret = set_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_ARMNT );
        if (ret || !self) return ret;
    }
    if (flags & CONTEXT_INTEGER)
    {
        frame->r0  = context->R0;
        frame->r1  = context->R1;
        frame->r2  = context->R2;
        frame->r3  = context->R3;
        frame->r4  = context->R4;
        frame->r5  = context->R5;
        frame->r6  = context->R6;
        frame->r7  = context->R7;
        frame->r8  = context->R8;
        frame->r9  = context->R9;
        frame->r10 = context->R10;
        frame->r11 = context->R11;
        frame->r12 = context->R12;
    }
    if (flags & CONTEXT_CONTROL)
    {
        frame->sp = context->Sp;
        frame->lr = context->Lr;
        frame->pc = context->Pc & ~1;
        frame->cpsr = context->Cpsr;
        if (context->Cpsr & 0x20) frame->pc |= 1; /* thumb */
    }
    if (flags & CONTEXT_FLOATING_POINT)
    {
        frame->fpscr = context->Fpscr;
        memcpy( frame->d, context->u.D, sizeof(context->u.D) );
    }
    frame->restore_flags |= flags & ~CONTEXT_INTEGER;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              NtGetContextThread  (NTDLL.@)
 *              ZwGetContextThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtGetContextThread( HANDLE handle, CONTEXT *context )
{
    struct syscall_frame *frame = arm_thread_data()->syscall_frame;
    DWORD needed_flags = context->ContextFlags & ~CONTEXT_ARM;
    BOOL self = (handle == GetCurrentThread());

    if (!self)
    {
        NTSTATUS ret = get_thread_context( handle, &context, &self, IMAGE_FILE_MACHINE_ARMNT );
        if (ret || !self) return ret;
    }

    if (needed_flags & CONTEXT_INTEGER)
    {
        context->R0  = frame->r0;
        context->R1  = frame->r1;
        context->R2  = frame->r2;
        context->R3  = frame->r3;
        context->R4  = frame->r4;
        context->R5  = frame->r5;
        context->R6  = frame->r6;
        context->R7  = frame->r7;
        context->R8  = frame->r8;
        context->R9  = frame->r9;
        context->R10 = frame->r10;
        context->R11 = frame->r11;
        context->R12 = frame->r12;
        context->ContextFlags |= CONTEXT_INTEGER;
    }
    if (needed_flags & CONTEXT_CONTROL)
    {
        context->Sp   = frame->sp;
        context->Lr   = frame->lr;
        context->Pc   = frame->pc;
        context->Cpsr = frame->cpsr;
        context->ContextFlags |= CONTEXT_CONTROL;
    }
    if (needed_flags & CONTEXT_FLOATING_POINT)
    {
        context->Fpscr = frame->fpscr;
        memcpy( context->u.D, frame->d, sizeof(frame->d) );
        context->ContextFlags |= CONTEXT_FLOATING_POINT;
    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              set_thread_wow64_context
 */
NTSTATUS set_thread_wow64_context( HANDLE handle, const void *ctx, ULONG size )
{
    return STATUS_INVALID_INFO_CLASS;
}


/***********************************************************************
 *              get_thread_wow64_context
 */
NTSTATUS get_thread_wow64_context( HANDLE handle, void *ctx, ULONG size )
{
    return STATUS_INVALID_INFO_CLASS;
}


__ASM_GLOBAL_FUNC( raise_func_trampoline,
                   "push {r12,lr}\n\t" /* (Padding +) Pc in the original frame */
                   "ldr r3, [r1, #0x38]\n\t" /* context->Sp */
                   "push {r3}\n\t" /* Original Sp */
                   __ASM_CFI(".cfi_escape 0x0f,0x03,0x7D,0x04,0x06\n\t") /* CFA, DW_OP_breg13 + 0x04, DW_OP_deref */
                   __ASM_CFI(".cfi_escape 0x10,0x0e,0x02,0x7D,0x0c\n\t") /* LR, DW_OP_breg13 + 0x0c */
                   /* We can't express restoring both Pc and Lr with CFI
                    * directives, but we manually load Lr from the stack
                    * in unwind_builtin_dll above. */
                   "ldr r3, [r1, #0x3c]\n\t" /* context->Lr */
                   "push {r3}\n\t" /* Original Lr */
                   "blx r2\n\t"
                   "udf #0")

/***********************************************************************
 *           setup_exception
 *
 * Modify the signal context to call the exception raise function.
 */
static void setup_exception( ucontext_t *sigcontext, EXCEPTION_RECORD *rec )
{
    struct
    {
        CONTEXT          context;
        EXCEPTION_RECORD rec;
    } *stack;

    void *stack_ptr = (void *)(SP_sig(sigcontext) & ~3);
    CONTEXT context;
    NTSTATUS status;

    rec->ExceptionAddress = (void *)PC_sig(sigcontext);
    save_context( &context, sigcontext );

    status = send_debug_event( rec, &context, TRUE );
    if (status == DBG_CONTINUE || status == DBG_EXCEPTION_HANDLED)
    {
        restore_context( &context, sigcontext );
        return;
    }

    stack = virtual_setup_exception( stack_ptr, sizeof(*stack), rec );
    stack->rec = *rec;
    stack->context = context;

    /* now modify the sigcontext to return to the raise function */
    SP_sig(sigcontext) = (DWORD)stack;
    LR_sig(sigcontext) = context.Pc;
    PC_sig(sigcontext) = (DWORD)raise_func_trampoline;
    if (PC_sig(sigcontext) & 1) CPSR_sig(sigcontext) |= 0x20;
    else CPSR_sig(sigcontext) &= ~0x20;
    REGn_sig(0, sigcontext) = (DWORD)&stack->rec;  /* first arg for KiUserExceptionDispatcher */
    REGn_sig(1, sigcontext) = (DWORD)&stack->context; /* second arg for KiUserExceptionDispatcher */
    REGn_sig(2, sigcontext) = (DWORD)pKiUserExceptionDispatcher;
}


/***********************************************************************
 *           call_user_apc_dispatcher
 */
NTSTATUS call_user_apc_dispatcher( CONTEXT *context, ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3,
                                   PNTAPCFUNC func, NTSTATUS status )
{
    struct syscall_frame *frame = arm_thread_data()->syscall_frame;
    ULONG sp = context ? context->Sp : frame->sp;
    struct apc_stack_layout
    {
        void   *func;
        void   *align;
        CONTEXT context;
    } *stack;

    sp &= ~15;
    stack = (struct apc_stack_layout *)sp - 1;
    if (context)
    {
        memmove( &stack->context, context, sizeof(stack->context) );
        NtSetContextThread( GetCurrentThread(), &stack->context );
    }
    else
    {
        stack->context.ContextFlags = CONTEXT_FULL;
        NtGetContextThread( GetCurrentThread(), &stack->context );
        stack->context.R0 = status;
    }
    frame->sp = (DWORD)stack;
    frame->pc = (DWORD)pKiUserApcDispatcher;
    frame->r0 = (DWORD)&stack->context;
    frame->r1 = arg1;
    frame->r2 = arg2;
    frame->r3 = arg3;
    stack->func = func;
    frame->restore_flags |= CONTEXT_CONTROL | CONTEXT_INTEGER;
    return status;
}


/***********************************************************************
 *           call_raise_user_exception_dispatcher
 */
void call_raise_user_exception_dispatcher(void)
{
    arm_thread_data()->syscall_frame->pc = (DWORD)pKiRaiseUserExceptionDispatcher;
}


/***********************************************************************
 *           call_user_exception_dispatcher
 */
NTSTATUS call_user_exception_dispatcher( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    struct syscall_frame *frame = arm_thread_data()->syscall_frame;
    DWORD lr = frame->lr;
    DWORD sp = frame->sp;
    NTSTATUS status = NtSetContextThread( GetCurrentThread(), context );

    if (status) return status;
    frame->r0 = (DWORD)rec;
    frame->r1 = (DWORD)context;
    frame->pc = (DWORD)pKiUserExceptionDispatcher;
    frame->lr = lr;
    frame->sp = sp;
    frame->restore_flags |= CONTEXT_INTEGER | CONTEXT_CONTROL;
    return status;
}


struct user_callback_frame
{
    struct syscall_frame frame;
    void               **ret_ptr;
    ULONG               *ret_len;
    __wine_jmp_buf       jmpbuf;
    NTSTATUS             status;
};

/***********************************************************************
 *           KeUserModeCallback
 */
NTSTATUS WINAPI KeUserModeCallback( ULONG id, const void *args, ULONG len, void **ret_ptr, ULONG *ret_len )
{
    struct user_callback_frame callback_frame = { { 0 }, ret_ptr, ret_len };

    if ((char *)ntdll_get_thread_data()->kernel_stack + min_kernel_stack > (char *)&callback_frame)
        return STATUS_STACK_OVERFLOW;

    if (!__wine_setjmpex( &callback_frame.jmpbuf, NULL ))
    {
        struct syscall_frame *frame = arm_thread_data()->syscall_frame;
        void *args_data = (void *)((frame->sp - len) & ~15);

        memcpy( args_data, args, len );

        callback_frame.frame.r0            = id;
        callback_frame.frame.r1            = (ULONG_PTR)args;
        callback_frame.frame.r2            = len;
        callback_frame.frame.sp            = (ULONG_PTR)args_data;
        callback_frame.frame.pc            = (ULONG_PTR)pKiUserCallbackDispatcher;
        callback_frame.frame.restore_flags = CONTEXT_INTEGER;
        callback_frame.frame.syscall_table = frame->syscall_table;
        callback_frame.frame.prev_frame    = frame;
        arm_thread_data()->syscall_frame = &callback_frame.frame;

        __wine_syscall_dispatcher_return( &callback_frame.frame, 0 );
    }
    return callback_frame.status;
}


/***********************************************************************
 *           NtCallbackReturn  (NTDLL.@)
 */
NTSTATUS WINAPI NtCallbackReturn( void *ret_ptr, ULONG ret_len, NTSTATUS status )
{
    struct user_callback_frame *frame = (struct user_callback_frame *)arm_thread_data()->syscall_frame;

    if (!frame->frame.prev_frame) return STATUS_NO_CALLBACK_ACTIVE;

    *frame->ret_ptr = ret_ptr;
    *frame->ret_len = ret_len;
    frame->status = status;
    arm_thread_data()->syscall_frame = frame->frame.prev_frame;
    __wine_longjmp( &frame->jmpbuf, 1 );
}


/***********************************************************************
 *           handle_syscall_fault
 *
 * Handle a page fault happening during a system call.
 */
static BOOL handle_syscall_fault( ucontext_t *context, EXCEPTION_RECORD *rec )
{
    struct syscall_frame *frame = arm_thread_data()->syscall_frame;
    DWORD i;

    if (!is_inside_syscall( context ) && !ntdll_get_thread_data()->jmp_buf) return FALSE;

    TRACE( "code=%x flags=%x addr=%p pc=%08x tid=%04x\n",
           rec->ExceptionCode, rec->ExceptionFlags, rec->ExceptionAddress,
           (DWORD)PC_sig(context), GetCurrentThreadId() );
    for (i = 0; i < rec->NumberParameters; i++)
        TRACE( " info[%d]=%08lx\n", i, rec->ExceptionInformation[i] );

    TRACE( " r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x\n",
           (DWORD)REGn_sig(0, context), (DWORD)REGn_sig(1, context), (DWORD)REGn_sig(2, context),
           (DWORD)REGn_sig(3, context), (DWORD)REGn_sig(4, context), (DWORD)REGn_sig(5, context) );
    TRACE( " r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x\n",
           (DWORD)REGn_sig(6, context), (DWORD)REGn_sig(7, context), (DWORD)REGn_sig(8, context),
           (DWORD)REGn_sig(9, context), (DWORD)REGn_sig(10, context), (DWORD)FP_sig(context) );
    TRACE( " r12=%08x sp=%08x lr=%08x pc=%08x cpsr=%08x\n",
           (DWORD)IP_sig(context), (DWORD)SP_sig(context), (DWORD)LR_sig(context),
           (DWORD)PC_sig(context), (DWORD)CPSR_sig(context) );

    if (ntdll_get_thread_data()->jmp_buf)
    {
        TRACE( "returning to handler\n" );
        REGn_sig(0, context) = (DWORD)ntdll_get_thread_data()->jmp_buf;
        REGn_sig(1, context) = 1;
        PC_sig(context)      = (DWORD)__wine_longjmp;
        ntdll_get_thread_data()->jmp_buf = NULL;
    }
    else
    {
        TRACE( "returning to user mode ip=%08x ret=%08x\n", frame->pc, rec->ExceptionCode );
        REGn_sig(0, context) = (DWORD)frame;
        REGn_sig(1, context) = rec->ExceptionCode;
        PC_sig(context)      = (DWORD)__wine_syscall_dispatcher_return;
    }
    return TRUE;
}


/**********************************************************************
 *		segv_handler
 *
 * Handler for SIGSEGV and related errors.
 */
static void segv_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };
    ucontext_t *context = sigcontext;

    switch (get_trap_code(signal, context))
    {
    case TRAP_ARM_PRIVINFLT:   /* Invalid opcode exception */
        if (*(WORD *)PC_sig(context) == 0xdefe)  /* breakpoint */
        {
            rec.ExceptionCode = EXCEPTION_BREAKPOINT;
            rec.NumberParameters = 1;
            break;
        }
        rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
        break;
    case TRAP_ARM_PAGEFLT:  /* Page fault */
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = (get_error_code(context) & 0x800) != 0;
        rec.ExceptionInformation[1] = (ULONG_PTR)siginfo->si_addr;
        rec.ExceptionCode = virtual_handle_fault( siginfo->si_addr, rec.ExceptionInformation[0],
                                                  (void *)SP_sig(context) );
        if (!rec.ExceptionCode) return;
        break;
    case TRAP_ARM_ALIGNFLT:  /* Alignment check exception */
        rec.ExceptionCode = EXCEPTION_DATATYPE_MISALIGNMENT;
        break;
    case TRAP_ARM_UNKNOWN:   /* Unknown fault code */
        rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = 0;
        rec.ExceptionInformation[1] = 0xffffffff;
        break;
    default:
        ERR("Got unexpected trap %d\n", get_trap_code(signal, context));
        rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
        break;
    }
    if (handle_syscall_fault( context, &rec )) return;
    setup_exception( context, &rec );
}


/**********************************************************************
 *		trap_handler
 *
 * Handler for SIGTRAP.
 */
static void trap_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };

    switch (siginfo->si_code)
    {
    case TRAP_TRACE:
        rec.ExceptionCode = EXCEPTION_SINGLE_STEP;
        break;
    case TRAP_BRKPT:
    default:
        rec.ExceptionCode = EXCEPTION_BREAKPOINT;
        rec.NumberParameters = 1;
        break;
    }
    setup_exception( sigcontext, &rec );
}


/**********************************************************************
 *		fpe_handler
 *
 * Handler for SIGFPE.
 */
static void fpe_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };

    switch (siginfo->si_code & 0xffff )
    {
#ifdef FPE_FLTSUB
    case FPE_FLTSUB:
        rec.ExceptionCode = EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
        break;
#endif
#ifdef FPE_INTDIV
    case FPE_INTDIV:
        rec.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
        break;
#endif
#ifdef FPE_INTOVF
    case FPE_INTOVF:
        rec.ExceptionCode = EXCEPTION_INT_OVERFLOW;
        break;
#endif
#ifdef FPE_FLTDIV
    case FPE_FLTDIV:
        rec.ExceptionCode = EXCEPTION_FLT_DIVIDE_BY_ZERO;
        break;
#endif
#ifdef FPE_FLTOVF
    case FPE_FLTOVF:
        rec.ExceptionCode = EXCEPTION_FLT_OVERFLOW;
        break;
#endif
#ifdef FPE_FLTUND
    case FPE_FLTUND:
        rec.ExceptionCode = EXCEPTION_FLT_UNDERFLOW;
        break;
#endif
#ifdef FPE_FLTRES
    case FPE_FLTRES:
        rec.ExceptionCode = EXCEPTION_FLT_INEXACT_RESULT;
        break;
#endif
#ifdef FPE_FLTINV
    case FPE_FLTINV:
#endif
    default:
        rec.ExceptionCode = EXCEPTION_FLT_INVALID_OPERATION;
        break;
    }
    setup_exception( sigcontext, &rec );
}


/**********************************************************************
 *		int_handler
 *
 * Handler for SIGINT.
 */
static void int_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    HANDLE handle;

    if (!p__wine_ctrl_routine) return;
    if (!NtCreateThreadEx( &handle, THREAD_ALL_ACCESS, NULL, NtCurrentProcess(),
                           p__wine_ctrl_routine, 0 /* CTRL_C_EVENT */, 0, 0, 0, 0, NULL ))
        NtClose( handle );
}


/**********************************************************************
 *		abrt_handler
 *
 * Handler for SIGABRT.
 */
static void abrt_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { EXCEPTION_WINE_ASSERTION, EH_NONCONTINUABLE };

    setup_exception( sigcontext, &rec );
}


/**********************************************************************
 *		quit_handler
 *
 * Handler for SIGQUIT.
 */
static void quit_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    abort_thread(0);
}


/**********************************************************************
 *		usr1_handler
 *
 * Handler for SIGUSR1, used to signal a thread that it got suspended.
 */
static void usr1_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    CONTEXT context;

    if (is_inside_syscall( sigcontext ))
    {
        context.ContextFlags = CONTEXT_FULL;
        NtGetContextThread( GetCurrentThread(), &context );
        wait_suspend( &context );
        NtSetContextThread( GetCurrentThread(), &context );
    }
    else
    {
        save_context( &context, sigcontext );
        wait_suspend( &context );
        restore_context( &context, sigcontext );
    }
}


/**********************************************************************
 *           get_thread_ldt_entry
 */
NTSTATUS get_thread_ldt_entry( HANDLE handle, void *data, ULONG len, ULONG *ret_len )
{
    return STATUS_NOT_IMPLEMENTED;
}


/******************************************************************************
 *           NtSetLdtEntries   (NTDLL.@)
 *           ZwSetLdtEntries   (NTDLL.@)
 */
NTSTATUS WINAPI NtSetLdtEntries( ULONG sel1, LDT_ENTRY entry1, ULONG sel2, LDT_ENTRY entry2 )
{
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *             signal_init_threading
 */
void signal_init_threading(void)
{
}


/**********************************************************************
 *             signal_alloc_thread
 */
NTSTATUS signal_alloc_thread( TEB *teb )
{
    teb->WOW32Reserved = __wine_syscall_dispatcher;
    return STATUS_SUCCESS;
}


/**********************************************************************
 *             signal_free_thread
 */
void signal_free_thread( TEB *teb )
{
}


/**********************************************************************
 *		signal_init_thread
 */
void signal_init_thread( TEB *teb )
{
    __asm__ __volatile__( "mcr p15, 0, %0, c13, c0, 2" : : "r" (teb) );
}


/**********************************************************************
 *		signal_init_process
 */
void signal_init_process(void)
{
    struct sigaction sig_act;
    void *kernel_stack = (char *)ntdll_get_thread_data()->kernel_stack + kernel_stack_size;

    arm_thread_data()->syscall_frame = (struct syscall_frame *)kernel_stack - 1;

    sig_act.sa_mask = server_block_set;
    sig_act.sa_flags = SA_RESTART | SA_SIGINFO | SA_ONSTACK;

    sig_act.sa_sigaction = int_handler;
    if (sigaction( SIGINT, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = fpe_handler;
    if (sigaction( SIGFPE, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = abrt_handler;
    if (sigaction( SIGABRT, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = quit_handler;
    if (sigaction( SIGQUIT, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = usr1_handler;
    if (sigaction( SIGUSR1, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = trap_handler;
    if (sigaction( SIGTRAP, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = segv_handler;
    if (sigaction( SIGSEGV, &sig_act, NULL ) == -1) goto error;
    if (sigaction( SIGILL, &sig_act, NULL ) == -1) goto error;
    if (sigaction( SIGBUS, &sig_act, NULL ) == -1) goto error;
    return;

 error:
    perror("sigaction");
    exit(1);
}


/***********************************************************************
 *           call_init_thunk
 */
void DECLSPEC_HIDDEN call_init_thunk( LPTHREAD_START_ROUTINE entry, void *arg, BOOL suspend, TEB *teb )
{
    struct arm_thread_data *thread_data = (struct arm_thread_data *)&teb->GdiTebBatch;
    struct syscall_frame *frame = thread_data->syscall_frame;
    CONTEXT *ctx, context = { CONTEXT_ALL };

    context.R0 = (DWORD)entry;
    context.R1 = (DWORD)arg;
    context.Sp = (DWORD)teb->Tib.StackBase;
    context.Pc = (DWORD)pRtlUserThreadStart;
    if (context.Pc & 1) context.Cpsr |= 0x20; /* thumb mode */
    if ((ctx = get_cpu_area( IMAGE_FILE_MACHINE_ARMNT ))) *ctx = context;

    if (suspend) wait_suspend( &context );

    ctx = (CONTEXT *)((ULONG_PTR)context.Sp & ~15) - 1;
    *ctx = context;
    ctx->ContextFlags = CONTEXT_FULL;
    NtSetContextThread( GetCurrentThread(), ctx );

    frame->sp = (DWORD)ctx;
    frame->pc = (DWORD)pLdrInitializeThunk;
    frame->r0 = (DWORD)ctx;
    frame->prev_frame = NULL;
    frame->restore_flags |= CONTEXT_INTEGER;
    frame->syscall_table = KeServiceDescriptorTable;

    pthread_sigmask( SIG_UNBLOCK, &server_block_set, NULL );
    __wine_syscall_dispatcher_return( frame, 0 );
}


/***********************************************************************
 *           signal_start_thread
 */
__ASM_GLOBAL_FUNC( signal_start_thread,
                   "push {r4-r12,lr}\n\t"
                   /* store exit frame */
                   "str sp, [r3, #0x1d4]\n\t" /* arm_thread_data()->exit_frame */
                   /* set syscall frame */
                   "ldr r6, [r3, #0x1d8]\n\t" /* arm_thread_data()->syscall_frame */
                   "cbnz r6, 1f\n\t"
                   "sub r6, sp, #0x160\n\t"   /* sizeof(struct syscall_frame) */
                   "str r6, [r3, #0x1d8]\n\t" /* arm_thread_data()->syscall_frame */
                   "1:\tmov sp, r6\n\t"
                   "bl " __ASM_NAME("call_init_thunk") )


/***********************************************************************
 *           signal_exit_thread
 */
__ASM_GLOBAL_FUNC( signal_exit_thread,
                   "ldr r3, [r2, #0x1d4]\n\t"  /* arm_thread_data()->exit_frame */
                   "mov ip, #0\n\t"
                   "str ip, [r2, #0x1d4]\n\t"
                   "cmp r3, ip\n\t"
                   "it ne\n\t"
                   "movne sp, r3\n\t"
                   "blx r1" )


/***********************************************************************
 *           __wine_syscall_dispatcher
 */
__ASM_GLOBAL_FUNC( __wine_syscall_dispatcher,
                   "mrc p15, 0, r1, c13, c0, 2\n\t" /* NtCurrentTeb() */
                   "ldr r1, [r1, #0x1d8]\n\t"       /* arm_thread_data()->syscall_frame */
                   "add r0, r1, #0x10\n\t"
                   "stm r0, {r4-r12,lr}\n\t"
                   "add r2, sp, #0x10\n\t"
                   "str r2, [r1, #0x38]\n\t"
                   "str r3, [r1, #0x3c]\n\t"
                   "mrs r0, CPSR\n\t"
                   "bfi r0, lr, #5, #1\n\t"         /* set thumb bit */
                   "str r0, [r1, #0x40]\n\t"
                   "mov r0, #0\n\t"
                   "str r0, [r1, #0x44]\n\t"        /* frame->restore_flags */
#ifndef __SOFTFP__
                   "vmrs r0, fpscr\n\t"
                   "str r0, [r1, #0x48]\n\t"
                   "add r0, r1, #0x60\n\t"
                   "vstm r0, {d0-d15}\n\t"
#endif
                   "mov r6, sp\n\t"
                   "mov sp, r1\n\t"
                   "mov r8, r1\n\t"
                   "ldr r5, [r1, #0x50]\n\t"        /* frame->syscall_table */
                   "ubfx r4, ip, #12, #2\n\t"       /* syscall table number */
                   "bfc ip, #12, #20\n\t"           /* syscall number */
                   "add r4, r5, r4, lsl #4\n\t"
                   "ldr r5, [r4, #8]\n\t"           /* table->ServiceLimit */
                   "cmp ip, r5\n\t"
                   "bcs 5f\n\t"
                   "ldr r5, [r4, #12]\n\t"          /* table->ArgumentTable */
                   "ldrb r5, [r5, ip]\n\t"
                   "cmp r5, #16\n\t"
                   "it le\n\t"
                   "movle r5, #16\n\t"
                   "sub r0, sp, r5\n\t"
                   "and r0, #~7\n\t"
                   "mov sp, r0\n"
                   "2:\tsubs r5, r5, #4\n\t"
                   "ldr r0, [r6, r5]\n\t"
                   "str r0, [sp, r5]\n\t"
                   "bgt 2b\n\t"
                   "pop {r0-r3}\n\t"                /* first 4 args are in registers */
                   "ldr r5, [r4]\n\t"               /* table->ServiceTable */
                   "ldr ip, [r5, ip, lsl #2]\n\t"
                   "blx ip\n"
                   "4:\tldr ip, [r8, #0x44]\n\t"    /* frame->restore_flags */
#ifndef __SOFTFP__
                   "tst ip, #4\n\t"                 /* CONTEXT_FLOATING_POINT */
                   "beq 3f\n\t"
                   "ldr r4, [r8, #0x48]\n\t"
                   "vmsr fpscr, r4\n\t"
                   "add r4, r8, #0x60\n\t"
                   "vldm r4, {d0-d15}\n"
                   "3:\n\t"
#endif
                   "tst ip, #2\n\t"                 /* CONTEXT_INTEGER */
                   "it ne\n\t"
                   "ldmne r8, {r0-r3}\n\t"
                   "ldr lr, [r8, #0x3c]\n\t"
                   "ldr sp, [r8, #0x38]\n\t"
                   "add r8, r8, #0x10\n\t"
                   "ldm r8, {r4-r12,pc}\n"
                   "5:\tmovw r0, #0x000d\n\t" /* STATUS_INVALID_PARAMETER */
                   "movt r0, #0xc000\n\t"
                   "add sp, sp, #0x10\n\t"
                   "b 4b\n"
                   __ASM_NAME("__wine_syscall_dispatcher_return") ":\n\t"
                   "mov r8, r0\n\t"
                   "mov r0, r1\n\t"
                   "b 4b" )


/***********************************************************************
 *           __wine_setjmpex
 */
__ASM_GLOBAL_FUNC( __wine_setjmpex,
                   "stm r0, {r1,r4-r11}\n"         /* jmp_buf->Frame,R4..R11 */
                   "str sp, [r0, #0x24]\n\t"       /* jmp_buf->Sp */
                   "str lr, [r0, #0x28]\n\t"       /* jmp_buf->Pc */
#ifndef __SOFTFP__
                   "vmrs r2, fpscr\n\t"
                   "str r2, [r0, #0x2c]\n\t"       /* jmp_buf->Fpscr */
                   "add r0, r0, #0x30\n\t"
                   "vstm r0, {d8-d15}\n\t"         /* jmp_buf->D[0..7] */
#endif
                   "mov r0, #0\n\t"
                   "bx lr" )


/***********************************************************************
 *           __wine_longjmp
 */
__ASM_GLOBAL_FUNC( __wine_longjmp,
                   "ldm r0, {r3-r11}\n\t"          /* jmp_buf->Frame,R4..R11 */
                   "ldr sp, [r0, #0x24]\n\t"       /* jmp_buf->Sp */
                   "ldr r2, [r0, #0x28]\n\t"       /* jmp_buf->Pc */
#ifndef __SOFTFP__
                   "ldr r3, [r0, #0x2c]\n\t"       /* jmp_buf->Fpscr */
                   "vmsr fpscr, r3\n\t"
                   "add r0, r0, #0x30\n\t"
                   "vldm r0, {d8-d15}\n\t"         /* jmp_buf->D[0..7] */
#endif
                   "mov r0, r1\n\t"                /* retval */
                   "bx r2" )

#endif  /* __arm__ */
