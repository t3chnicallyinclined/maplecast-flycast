/*
	Highly inefficient and boring interpreter. Nothing special here
*/

#include "types.h"

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "../sh4_sched.h"
#include "../sh4_cache.h"
#include "debug/gdb_server.h"
#include "../sh4_cycles.h"

Sh4ICache icache;
Sh4OCache ocache;
Sh4Interpreter *Sh4Interpreter::Instance;

void Sh4Interpreter::ExecuteOpcode(u16 op)
{
	if (ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);
	OpPtr[op](ctx, op);
	sh4cycles.executeCycles(op);
}

u16 Sh4Interpreter::ReadNexOp()
{
	u32 addr = ctx->pc;
	if (!mmu_enabled() && (addr & 1))
		// address error
		throw SH4ThrownException(addr, Sh4Ex_AddressErrorRead);

	ctx->pc = addr + 2;

	return IReadMem16(addr);
}

// SH4Recomp: static block dispatch
#ifdef SH4RECOMP_BLOCKS
typedef u32 (*RecompBlockFunc)(Sh4Context* ctx);
extern RecompBlockFunc sh4recomp_find_block(u32 pc);
extern u64 sh4recomp_static_hits();
extern u64 sh4recomp_fallback_hits();
#endif

void Sh4Interpreter::Run()
{
	Instance = this;
	ctx->restoreHostRoundingMode();

#ifdef SH4RECOMP_BLOCKS
	static bool _logged_recomp = false;
	if (!_logged_recomp) {
		printf("[sh4recomp] Static block dispatch ACTIVE in interpreter loop\n");
		fflush(stdout);
		_logged_recomp = true;
	}
#endif

	try {
		do
		{
			try {
				do
				{
#ifdef SH4RECOMP_BLOCKS
					// Try static block first
					RecompBlockFunc block = sh4recomp_find_block(ctx->pc);
					if (block) {
						u32 next_pc = block(ctx);
						ctx->pc = next_pc;
					} else {
						u32 op = ReadNexOp();
						ExecuteOpcode(op);
					}
					// Periodic stats every ~5 seconds
					{
						static u64 _last_report = 0;
						static u64 _pc_sample_count = 0;
						u64 total = sh4recomp_static_hits() + sh4recomp_fallback_hits();
						if (total - _last_report > 5000000) {
							_last_report = total;
							printf("[sh4recomp] %lu static / %lu fallback (%.1f%% recompiled) pc=0x%08x\n",
								sh4recomp_static_hits(), sh4recomp_fallback_hits(),
								100.0 * sh4recomp_static_hits() / (total ? total : 1),
								ctx->pc);
							fflush(stdout);
						}
						// Log first few PCs to see what addresses are being hit
						if (_pc_sample_count < 20) {
							_pc_sample_count++;
							printf("[sh4recomp] PC sample #%lu: 0x%08x\n", _pc_sample_count, ctx->pc);
							fflush(stdout);
						}
					}
#else
					u32 op = ReadNexOp();

					ExecuteOpcode(op);
#endif
				} while (ctx->cycle_counter > 0);
				ctx->cycle_counter += SH4_TIMESLICE;
				UpdateSystem_INTC();
			} catch (const SH4ThrownException& ex) {
				Do_Exception(ex.epc, ex.expEvn);
				// an exception requires the instruction pipeline to drain, so approx 5 cycles
				sh4cycles.addCycles(5 * CPU_RATIO);
			}
		} while (ctx->CpuRunning);
	} catch (const debugger::Stop&) {
	}

#ifdef SH4RECOMP_BLOCKS
	printf("[sh4recomp] Session stats: %lu static hits, %lu fallbacks\n",
		sh4recomp_static_hits(), sh4recomp_fallback_hits());
	fflush(stdout);
#endif

	ctx->CpuRunning = false;
	Instance = nullptr;
}

void Sh4Interpreter::Start()
{
	ctx->CpuRunning = true;
}

void Sh4Interpreter::Stop()
{
	ctx->CpuRunning = false;
}

void Sh4Interpreter::Step()
{
	Instance = this;

	ctx->restoreHostRoundingMode();
	try {
		u32 op = ReadNexOp();
		ExecuteOpcode(op);
	} catch (const SH4ThrownException& ex) {
		Do_Exception(ex.epc, ex.expEvn);
		// an exception requires the instruction pipeline to drain, so approx 5 cycles
		sh4cycles.addCycles(5 * CPU_RATIO);
	} catch (const debugger::Stop&) {
	}
	Instance = nullptr;
}

void Sh4Interpreter::Reset(bool hard)
{
	verify(!ctx->CpuRunning);

	if (hard)
	{
		int schedNext = ctx->sh4_sched_next;
		memset(ctx, 0, sizeof(*ctx));
		ctx->sh4_sched_next = schedNext;
	}
	ctx->pc = 0xA0000000;

	memset(ctx->r, 0, sizeof(ctx->r));
	memset(ctx->r_bank, 0, sizeof(ctx->r_bank));

	ctx->gbr = ctx->ssr = ctx->spc = ctx->sgr = ctx->dbr = ctx->vbr = 0;
	ctx->mac.full = ctx->pr = ctx->fpul = 0;

	ctx->sr.setFull(0x700000F0);
	ctx->old_sr.status = ctx->sr.status;
	UpdateSR();

	ctx->fpscr.full = 0x00040001;
	ctx->old_fpscr = ctx->fpscr;

	icache.Reset(hard);
	ocache.Reset(hard);
	sh4cycles.reset();
	ctx->cycle_counter = SH4_TIMESLICE;

	INFO_LOG(INTERPRETER, "Sh4 Reset");
}

bool Sh4Interpreter::IsCpuRunning()
{
	return ctx->CpuRunning;
}

//TODO : Check for valid delayslot instruction
void Sh4Interpreter::ExecuteDelayslot()
{
	try {
		u32 op = ReadNexOp();

		ExecuteOpcode(op);
	} catch (SH4ThrownException& ex) {
		AdjustDelaySlotException(ex);
		throw ex;
	} catch (const debugger::Stop& e) {
		ctx->pc -= 2;	// break on previous instruction
		throw e;
	}
}

void Sh4Interpreter::ExecuteDelayslot_RTE()
{
	try {
		// In an RTE delay slot, status register (SR) bits are referenced as follows.
		// In instruction access, the MD bit is used before modification, and in data access,
		// the MD bit is accessed after modification.
		// The other bits—S, T, M, Q, FD, BL, and RB—after modification are used for delay slot
		// instruction execution. The STC and STC.L SR instructions access all SR bits after modification.
		u32 op = ReadNexOp();
		// Now restore all SR bits
		ctx->sr.setFull(ctx->ssr);
		// And execute
		ExecuteOpcode(op);
	} catch (const SH4ThrownException&) {
		throw FlycastException("Fatal: SH4 exception in RTE delay slot");
	} catch (const debugger::Stop& e) {
		ctx->pc -= 2;	// break on previous instruction
		throw e;
	}
}

// every SH4_TIMESLICE cycles
int UpdateSystem_INTC()
{
	Sh4cntx.sh4_sched_next -= SH4_TIMESLICE;
	if (Sh4cntx.sh4_sched_next < 0)
		sh4_sched_tick(SH4_TIMESLICE);
	if (Sh4cntx.interrupt_pend)
		return UpdateINTC();
	else
		return 0;
}

void Sh4Interpreter::Init()
{
	ctx = &p_sh4rcb->cntx;
	memset(ctx, 0, sizeof(*ctx));
	sh4cycles.init(ctx);
	icache.init(ctx);
	ocache.init(ctx);
}

void Sh4Interpreter::Term()
{
	Stop();
	INFO_LOG(INTERPRETER, "Sh4 Term");
}

Sh4Executor *Get_Sh4Interpreter()
{
	return new Sh4Interpreter();
}
