/* **********************************************************
 * Copyright (c) 2008-2009 VMware, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/***************************************************************************
 * readwrite.h: Dr. Memory read/write instrumentation
 */

#ifndef _READWRITE_H_
#define _READWRITE_H_ 1

#include "fastpath.h"
#include "callstack.h" /* for app_loc_t */

/* there is no REG_EFLAGS so we use the REG_INVALID sentinel */
#define REG_EFLAGS REG_INVALID

/* we only need a little over 2 pages for whole_bb_spills_enabled(): could get
 * onto 2 pages by not emitting SPILL_REG_NONE.
 * -no_single_arg_slowpath needs only 10 pages.
 */
#define SHARED_SLOWPATH_SIZE (whole_bb_spills_enabled() ? PAGE_SIZE*11 : PAGE_SIZE*7)

void
instrument_init(void);

void
instrument_exit(void);

byte *
generate_shared_slowpath(void *drcontext, instrlist_t *ilist, byte *pc);

void
update_stack_swap_threshold(void *drcontext, int new_threshold);

/* flags passed in to check_mem_opnd() and handle_mem_ref() */
enum {
    MEMREF_WRITE              = 0x001,
    MEMREF_PUSHPOP            = 0x002, /* the stack slot mem ref of push/pop */
    MEMREF_CHECK_DEFINEDNESS  = 0x004,
    MEMREF_USE_VALUES         = 0x008, /* for read, OUT; for write, IN */
    MEMREF_SINGLE_BYTE        = 0x010, /* keep using 1st byte in array */
    MEMREF_SINGLE_WORD        = 0x020, /* keep using 1st 2 bytes in array */
    MEMREF_SINGLE_DWORD       = 0x040, /* keep using 1st 4 bytes in array */
    MEMREF_MOVS               = 0x080, /* if a write, 1st entry in array holds base
                                        * of source shadow addr, which has already
                                        * been checked for addressability */
    MEMREF_CHECK_ADDRESSABLE  = 0x100, /* for pre-write */
};

#ifdef STATISTICS
/* per-opcode counts */
extern uint64 slowpath_count[OP_LAST+1];
extern uint64 slowpath_sz1;
extern uint64 slowpath_sz2;
extern uint64 slowpath_sz4;
extern uint64 slowpath_sz8;
extern uint64 slowpath_szOther;
/* FIXME: make generalized stats infrastructure */
extern uint slowpath_executions;
extern uint read_slowpath;
extern uint write_slowpath;
extern uint push_slowpath;
extern uint pop_slowpath;
extern uint read_fastpath;
extern uint write_fastpath;
extern uint push_fastpath;
extern uint pop_fastpath;
extern uint read4_fastpath;
extern uint write4_fastpath;
extern uint push4_fastpath;
extern uint pop4_fastpath;
extern uint slow_instead_of_fast;
extern uint heap_header_exception;
extern uint tls_exception;
extern uint alloca_exception;
extern uint strlen_exception;
extern uint strcpy_exception;
extern uint rawmemchr_exception;
extern uint strmem_unaddr_exception;
extern uint strrchr_exception;
extern uint andor_exception;
extern uint loader_DRlib_exception;
extern uint reg_dead;
extern uint reg_xchg;
extern uint reg_spill;
extern uint reg_spill_slow;
extern uint reg_spill_own;
extern uint reg_spill_used_in_bb;
extern uint reg_spill_unused_in_bb;
extern uint addressable_checks_elided;
extern uint aflags_saved_at_top;
extern uint num_faults;
extern uint xl8_shared;
extern uint xl8_not_shared_reg_conflict;
extern uint xl8_not_shared_disp_too_big;
extern uint xl8_not_shared_mem2mem;
extern uint xl8_not_shared_offs;
extern uint xl8_not_shared_slowpaths;
extern uint slowpath_unaligned;
extern uint alloc_stack_count;
extern uint delayed_free_bytes;
extern uint app_instrs_fastpath;
extern uint app_instrs_no_dup;
extern uint xl8_app_for_slowpath;
#endif

extern hashtable_t bb_table;

/* PR 493257: share shadow translation across multiple instrs */
extern hashtable_t xl8_sharing_table;

dr_emit_flags_t
instrument_bb(void *drcontext, void *tag, instrlist_t *bb,
              bool for_trace, bool translating);

bool
check_mem_opnd(uint opc, uint flags, app_loc_t *loc, opnd_t opnd, uint sz,
               dr_mcontext_t *mc, uint *shadow_vals);

bool
handle_mem_ref(uint flags, app_loc_t *loc, app_pc addr, size_t sz, dr_mcontext_t *mc,
               uint *shadow_vals);

bool
check_register_defined(void *drcontext, reg_id_t reg, app_loc_t *loc, size_t sz,
                       dr_mcontext_t *mc);

bool
is_in_gencode(byte *pc);

bool
event_restore_state(void *drcontext, bool restore_memory, dr_restore_state_info_t *info);

void
event_fragment_delete(void *drcontext, void *tag);

bool
instr_can_use_shared_slowpath(instr_t *inst);

void
instrument_slowpath(void *drcontext, instrlist_t *bb, instr_t *inst, fastpath_info_t *mi);

/***************************************************************************
 * ISA UTILITY ROUTINES
 */

#define MAX_INSTR_SIZE 17

/* Avoid selfmod mangling for our "meta-instructions that can fault" (xref PR 472190).
 * Things would work without this (just lower performance, but on selfmod only)
 * except our short ctis don't reach w/ all the selfmod mangling: and we don't
 * have jmp_smart (i#56/PR 209710)!
 */
#define PREXL8M instrlist_meta_fault_preinsert

/* eflags eax and up-front save use this slot, and whole-bb spilling stores
 * eflags itself (lahf+seto) here
 */
#define SPILL_SLOT_EFLAGS_EAX SPILL_SLOT_3

int
spill_reg3_slot(bool eflags_dead, bool eax_dead, bool r1_dead, bool r2_dead);

void
spill_reg(void *drcontext, instrlist_t *ilist, instr_t *where, reg_id_t reg,
          dr_spill_slot_t slot);

void
restore_reg(void *drcontext, instrlist_t *ilist, instr_t *where, reg_id_t reg,
            dr_spill_slot_t slot);

opnd_t
spill_slot_opnd(void *drcontext, dr_spill_slot_t slot);

bool
is_spill_slot_opnd(void *drcontext, opnd_t op);

bool
reg_is_gpr(reg_id_t reg);

bool
reg_is_8bit(reg_id_t reg);

bool
reg_is_8bit_high(reg_id_t reg);

bool
reg_is_16bit(reg_id_t reg);

bool
reg_offs_in_dword(reg_id_t reg);

reg_id_t
reg_32_to_8h(reg_id_t reg);

bool
opc_is_push(uint opc);

bool
opc_is_pop(uint opc);

bool
opc_is_stringop(uint opc);

bool
opc_is_stringop_loop(uint opc);

bool
opc_is_gpr_shift(uint opc);

bool
opc_is_jcc(uint opc);

bool
opc_is_cmovcc(uint opc);

bool
opc_is_fcmovcc(uint opc);

/* can 2nd dst be treated as simply an extension of the 1st */
bool
opc_2nd_dst_is_extension(uint opc);

uint
adjust_memop_push_offs(instr_t *inst);

opnd_t
adjust_memop(instr_t *inst, opnd_t opnd, bool write, uint *opsz, bool *pushpop_stackop);

bool
result_is_always_defined(instr_t *inst);

bool
always_check_definedness(instr_t *inst, int opnum);

bool
instr_check_definedness(instr_t *inst);

bool
instr_needs_all_srcs_and_vals(instr_t *inst);

int
num_true_srcs(instr_t *inst, dr_mcontext_t *mc);

int
num_true_dsts(instr_t *inst, dr_mcontext_t *mc);

#endif /* _READWRITE_H_ */
