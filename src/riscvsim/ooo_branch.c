/**
 * Out of order core Back-end: Branch Handling
 *
 * MARSS-RISCV : Micro-Architectural System Simulator for RISC-V
 *
 * Copyright (c) 2017-2019 Gaurav Kothari {gkothar1@binghamton.edu}
 * State University of New York at Binghamton
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "ooo.h"
#include "../riscv_cpu_priv.h"
#include "circular_queue.h"
#include "riscv_ins_execute_lib.h"
#include "riscv_sim_cpu.h"

static void
restore_cpu_frontend(OOCore *core, IMapEntry *e)
{
    RISCVCPUState *s;

    s = core->simcpu->emu_cpu_state;

    /* Flush front-end stages */
    speculative_cpu_stage_flush(&core->fetch, s->simcpu->imap);
    speculative_cpu_stage_flush(&core->decode, s->simcpu->imap);
    speculative_cpu_stage_flush(&core->dispatch, s->simcpu->imap);

    /* Flush the memory transactions added by fetch stage */
    mem_controller_flush_stage_mem_access_queue(
        &s->simcpu->mmu->mem_controller->frontend_mem_access_queue);

    /* Set the new target address into fetch and enable fetch unit to start
     * fetching from the target */
    s->code_ptr = NULL;
    s->code_end = NULL;
    s->code_to_pc_addend = e->branch_target;
    core->fetch.has_data = TRUE;
}

static int
rob_entry_committed_after_rollback(ROB *rob, int src_idx)
{
    if (rob->cq.rear >= rob->cq.front)
    {
        if ((src_idx >= rob->cq.front) && (src_idx <= rob->cq.rear))
        {
            return FALSE;
        }
    }
    else
    {
        if (((src_idx >= rob->cq.front) && (src_idx < rob->cq.max_size))
            || ((src_idx >= 0) && (src_idx <= rob->cq.rear)))
        {
            return FALSE;
        }
    }
    return TRUE;
}

static void
restore_rob_entry(OOCore *core, ROBEntry *rbe, uint64_t tag)
{
    IMapEntry *e;

    e = rbe->e;
    if (e->ins_dispatch_id > tag)
    {
        if (e->ins.has_dest && e->ins.rd)
        {
            core->int_rat[e->ins.rd].rob_idx = e->ins.old_pdest;
            core->int_rat[e->ins.rd].read_from_rob = TRUE;
            if (e->ins.old_pdest == -1)
            {
                core->int_rat[e->ins.rd].read_from_rob = FALSE;
            }
            assert(core->int_rat[e->ins.rd].rob_idx != e->rob_idx);
        }
        else if (e->ins.has_fp_dest)
        {
            core->fp_rat[e->ins.rd].rob_idx = e->ins.old_pdest;
            core->fp_rat[e->ins.rd].read_from_rob = TRUE;
            if (e->ins.old_pdest == -1)
            {
                core->fp_rat[e->ins.rd].read_from_rob = FALSE;
            }
            assert(core->fp_rat[e->ins.rd].rob_idx != e->rob_idx);
        }
        /* Free up IMAP entry */
        e->status = IMAP_ENTRY_STATUS_FREE;
    }
}

/* Can be optimized */
static void
restore_rob(OOCore *core, IMapEntry *e, uint64_t tag)
{
    int i;

    if (core->rob.cq.rear >= core->rob.cq.front)
    {
        for (i = core->rob.cq.rear; i >= core->rob.cq.front; i--)
        {
            restore_rob_entry(core, &core->rob.entries[i], tag);
        }
    }
    else
    {
        /* ROB is wrapped around */
        for (i = core->rob.cq.rear; i >= 0; i--)
        {
            restore_rob_entry(core, &core->rob.entries[i], tag);
        }

        for (i = core->rob.cq.max_size - 1; i >= core->rob.cq.front; i--)
        {
            restore_rob_entry(core, &core->rob.entries[i], tag);
        }
    }

    /* Flush all the ROB entries till this branch */
    cq_set_rear(&core->rob.cq, e->rob_idx);
    assert(e == core->rob.entries[core->rob.cq.rear].e);
}

static void
restore_iq(IssueQueueEntry *iq, int size, uint64_t tag)
{
    int i;

    for (i = 0; i < size; ++i)
    {
        if (iq[i].valid && (iq[i].e->ins_dispatch_id > tag))
        {
            iq[i].valid = FALSE;
        }
    }
}

static void
restore_fu(CPUStage *fu, int stages, uint64_t tag, IMapEntry *imap)
{
    int i;
    IMapEntry *e;

    for (i = 0; i < stages; ++i)
    {
        if (fu[i].has_data)
        {
            e = &imap[fu[i].imap_index];
            if (e->ins_dispatch_id > tag)
            {
                speculative_cpu_stage_flush(&fu[i], imap);
            }
        }
    }
}

static void
restore_lsu(OOCore *core, uint64_t tag)
{
    IMapEntry *e;

    if (core->lsu.has_data)
    {
        e = &core->simcpu->imap[core->lsu.imap_index];
        if (e->ins_dispatch_id > tag)
        {
            speculative_cpu_stage_flush(&core->lsu, core->simcpu->imap);

            /* Flush memory controller queues on flush */
            mem_controller_reset(core->simcpu->mmu->mem_controller);
        }
    }
}

static int
is_lsq_entry_speculated(LSQEntry *lsqe, uint64_t tag)
{
    if (lsqe->e->ins_dispatch_id > tag)
    {
        return TRUE;
    }
    return FALSE;
}

static void
restore_lsq(OOCore *core, uint64_t tag)
{
    int i;
    LSQEntry *lsqe;

    if (!cq_empty(&core->lsq.cq))
    {
        lsqe = &core->lsq.entries[cq_front(&core->lsq.cq)];

        /* Memory instruction on LSQ front is on miss-predicted path */
        if (lsqe->e->ins_dispatch_id > tag)
        {
            /* Flush entire LSQ since all the LSQ entries are along the
             * speculated path */
            cq_reset(&core->lsq.cq);
        }
        else
        {
            if (core->lsq.cq.rear >= core->lsq.cq.front)
            {
                for (i = core->lsq.cq.front; i <= core->lsq.cq.rear; i++)
                {
                    if (is_lsq_entry_speculated(&core->lsq.entries[i], tag))
                    {
                        cq_set_rear(&core->lsq.cq, i - 1);
                        return;
                    }
                }
            }
            else
            {
                /* LSQ is wrapped around */
                for (i = core->lsq.cq.front; i < core->lsq.cq.max_size; i++)
                {
                    if (is_lsq_entry_speculated(&core->lsq.entries[i], tag))
                    {
                        cq_set_rear(&core->lsq.cq, i - 1);
                        return;
                    }
                }

                for (i = 0; i <= core->lsq.cq.rear; i++)
                {
                    if (is_lsq_entry_speculated(&core->lsq.entries[i], tag))
                    {
                        if (i == 0)
                        {
                            cq_set_rear(&core->lsq.cq,
                                        core->lsq.cq.max_size - 1);
                        }
                        else
                        {
                            cq_set_rear(&core->lsq.cq, i - 1);
                        }
                        return;
                    }
                }
            }
        }
    }
}

/** After rollback of mis-speculated instructions, fix the rename table entries
 * pointing to restored physical registers which are already committed  */
static void
fix_rename_tables(OOCore *core, RenameTableEntry *rat, int num_regs)
{
    int i;

    for (i = 0; i < num_regs; ++i)
    {
        if (rat[i].read_from_rob)
        {
            assert(rat[i].rob_idx != -1);
            if (rob_entry_committed_after_rollback(&core->rob, rat[i].rob_idx))
            {
                rat[i].rob_idx = -1;
                rat[i].read_from_rob = FALSE;
            }
        }
    }
}

static void
reallocate_active_imap_entries(OOCore *core)
{
    int i;

    /* Set the imap entry status for active instructions as allocated */
    if (core->rob.cq.rear >= core->rob.cq.front)
    {
        for (i = core->rob.cq.front; i <= core->rob.cq.rear; ++i)
        {
            core->rob.entries[i].e->status = IMAP_ENTRY_STATUS_ALLOCATED;
        }
    }
    else
    {
        for (i = core->rob.cq.front; i < core->rob.cq.max_size; ++i)
        {
            core->rob.entries[i].e->status = IMAP_ENTRY_STATUS_ALLOCATED;
        }
        for (i = 0; i <= core->rob.cq.rear; ++i)
        {
            core->rob.entries[i].e->status = IMAP_ENTRY_STATUS_ALLOCATED;
        }
    }
}

static void
rollback_speculated_cpu_state(OOCore *core, IMapEntry *e)
{
    restore_cpu_frontend(core, e);
    restore_rob(core, e, e->ins_dispatch_id);
    restore_iq(core->iq, core->simcpu->params->iq_size, e->ins_dispatch_id);
    restore_fu(core->ialu, core->simcpu->params->num_alu_stages,
               e->ins_dispatch_id, core->simcpu->imap);
    restore_fu(core->imul, core->simcpu->params->num_mul_stages,
               e->ins_dispatch_id, core->simcpu->imap);
    restore_fu(core->idiv, core->simcpu->params->num_div_stages,
               e->ins_dispatch_id, core->simcpu->imap);
    restore_fu(&core->fpu_alu, 1, e->ins_dispatch_id, core->simcpu->imap);
    restore_fu(core->fpu_fma, core->simcpu->params->num_fpu_fma_stages,
               e->ins_dispatch_id, core->simcpu->imap);
    restore_lsq(core, e->ins_dispatch_id);
    restore_lsu(core, e->ins_dispatch_id);
    fix_rename_tables(core, core->int_rat, NUM_INT_REG);
    fix_rename_tables(core, core->fp_rat, NUM_FP_REG);
    reset_imap(core->simcpu->imap);
    reallocate_active_imap_entries(core);
}

void
oo_process_branch(OOCore *core, IMapEntry *e)
{
    e->mispredict
        = core->simcpu->pfn_branch_handler(core->simcpu->emu_cpu_state, e);

    if (e->mispredict)
    {
        rollback_speculated_cpu_state(core, e);
    }

    switch (e->ins.branch_type)
    {
        case BRANCH_COND:
        {
            core->rob.entries[e->rob_idx].ready = TRUE;
            break;
        }
        case BRANCH_UNCOND:
        {
            if (!e->ins.has_dest)
            {
                core->rob.entries[e->rob_idx].ready = TRUE;
            }
            break;
        }
    }

    e->branch_processed = TRUE;
}
