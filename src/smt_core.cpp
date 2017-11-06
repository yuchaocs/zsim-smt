/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the * terms of the GNU General Public License as published by the Free Software * Foundation, version 2.  *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

// A composite core that simulates SMT.
// Controls access to two virtual OOO Cores in the background.

#include "smt_core.h"
#include <stdio.h>
#include <algorithm>
#include <queue>
#include <string>
#include <inttypes.h>
#include "bithacks.h"
#include "g_std/g_multimap.h"
#include "memory_hierarchy.h"
#include "ooo_core_recorder.h"
#include "filter_cache.h"
#include "scheduler.h"
#include "stats.h"
#include "pad.h"
#include "zsim.h"

extern GlobSimInfo* zinfo;

#define FETCH_STAGE 1
#define DECODE_STAGE 4
#define ISSUE_STAGE 7
#define DISPATCH_STAGE 13

#define L1D_LAT 4
#define FETCH_BYTES_PER_CYCLE 16
#define ISSUES_PER_CYCLE 4
#define RF_READS_PER_CYCLE 3


SMTCore::SMTCore(FilterCache* _l1i, FilterCache* _l1d, g_string& _name)
	: Core(_name), l1i(_l1i), l1d(_l1d), cRec(0, _name) {

    //info("OOOE: Creating a SMT Core");
	futex_init(&windowLock);
	prevContext = new(gm_memalign<BblContext>(CACHE_LINE_BYTES, 1)) BblContext();
	smtWindow = new(gm_memalign< SmtWindow >(CACHE_LINE_BYTES, 1)) SmtWindow();
	smtWindow->vcore = 0;
	curCycle = 0;
	prevContext->bbl = nullptr;
}

void SMTCore::initStats(AggregateStat* parentStat) {
	AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");

    auto x = [this]() { return cRec.getUnhaltedCycles(curCycle); };
    LambdaStat<decltype(x)>* cyclesStat = new LambdaStat<decltype(x)>(x);
    cyclesStat->init("cycles", "Simulated unhalted cycles");

    auto y = [this]() { return cRec.getContentionCycles(); };
    LambdaStat<decltype(y)>* cCyclesStat = new LambdaStat<decltype(y)>(y);
    cCyclesStat->init("cCycles", "Cycles due to contention stalls");

    ProxyStat* instrsStat = new ProxyStat();
    instrsStat->init("instrs", "Simulated instructions", &instrs);
    ProxyStat* uopsStat = new ProxyStat();
    uopsStat->init("uops", "Retired micro-ops", &uops);
    ProxyStat* bblsStat = new ProxyStat();
    bblsStat->init("bbls", "Basic blocks", &bbls);
    ProxyStat* approxInstrsStat = new ProxyStat();
    approxInstrsStat->init("approxInstrs", "Instrs with approx uop decoding", &approxInstrs);
    ProxyStat* mispredBranchesStat = new ProxyStat();
    mispredBranchesStat->init("mispredBranches", "Mispredicted branches", &mispredBranches);

    coreStat->append(cyclesStat);
    coreStat->append(cCyclesStat);
    coreStat->append(instrsStat);
    coreStat->append(uopsStat);
    coreStat->append(bblsStat);
    coreStat->append(approxInstrsStat);
    coreStat->append(mispredBranchesStat);

#ifdef SMT_STALL_STATS
    profFetchStalls.init("fetchStalls",  "Fetch stalls");  coreStat->append(&profFetchStalls);
    profDecodeStalls.init("decodeStalls", "Decode stalls"); coreStat->append(&profDecodeStalls);
    profIssueStalls.init("issueStalls",  "Issue stalls");  coreStat->append(&profIssueStalls);
#endif

    parentStat->append(coreStat);

}

uint64_t SMTCore::getInstrs() const {
	return instrs;
}

uint64_t SMTCore::getPhaseCycles() const {
	return curCycle % zinfo->phaseLength;
}

uint64_t SMTCore::getCycles() const {
	// old way. TODO: revise for SMT.
	return cRec.getUnhaltedCycles(curCycle);
}

/* OOOE: Function to mark if a thread has completed on the core. Used to playback only when the last thread needs to be completed */
void SMTCore::markDone() {
	if ( !smtWindow->thCompleted ){
		smtWindow->thCompleted = true;
	}
	else{
		this->playback();
	}
}

void SMTCore::contextSwitch(int32_t gid) {
	/* OOOE: Run from scheduler. gid = -1 is passed from scheduler in the
	   deschedule function. */
	if (gid == -1) { 
		smtWindow->vcore = (smtWindow->vcore + 1) % SmtWindow::NUM_VCORES;

		//info("OOOE: CntxtSw VC:%d PID:%d A[0]:%d A[1]:%d", smtWindow->vcore, getpid(), smtWindow->bblQueue[0].count(), smtWindow->bblQueue[1].count());

		/* Playback on context switch if both queues are full, else only playback if there is one thread left */
        if ( !smtWindow->bblQueue[0].empty() && !smtWindow->bblQueue[1].empty() ){
            this->playback();
        }
        else if ( !smtWindow->bblQueue[0].empty() || !smtWindow->bblQueue[1].empty() ){
            if ( smtWindow->thCompleted ){
                this->playback();
            }
        }

        // Do not store previous BBL, as we were context-switched
        if (prevContext->bbl){
            prevContext->bbl = nullptr;
        }

        /* OOOE: AG: We dont want to clear the cache since they are shared */
        // l1i->contextSwitch();
        // l1d->contextSwitch();
    }
}

/**
 * TODO:
 * What is a join? 
 * Is this called by the scheduler?
 * Can we update the virtual core number here?
 */
void SMTCore::join() {
	info("[%s] Joining, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    uint64_t targetCycle = cRec.notifyJoin(curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
    phaseEndCycle = zinfo->globPhaseCycles + zinfo->phaseLength;
    // assert(targetCycle <= phaseEndCycle);
    info("[%s] Joined, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
}

/**
 * TODO:
 * What is a leave? 
 * Is this called by the scheduler?
 * Can we update the virtual core number here?
 */
void SMTCore::leave() {
    info("[%s] Leaving, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    cRec.notifyLeave(curCycle);
}

InstrFuncPtrs SMTCore::GetFuncPtrs() {
	return {
		LoadFunc, StoreFunc, 
		BblFunc, BranchFunc, 
		PredLoadFunc, PredStoreFunc, 
		FPTR_ANALYSIS, {0}
	};
}

// Contention simulation interface
EventRecorder* SMTCore::getEventRecorder() {
	return cRec.getEventRecorder();
}

void SMTCore::cSimStart(){
    uint64_t targetCycle = cRec.cSimStart(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

void SMTCore::cSimEnd() {
    uint64_t targetCycle = cRec.cSimEnd(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

// Predicated loads and stores call this function, gets recorded as a 0-cycle op.
// Predication is rare enough that we don't need to model it perfectly to be accurate 
// (i.e. the uops still execute, retire, etc), but this is needed for correctness.
inline void SMTCore::predFalseMemOp() {
    // I'm going to go out on a limb and assume just loads are predicated 
	// (this will not fail silently if it's a store)
	prevContext->loadAddrs[prevContext->loads++] = -1L;
}

inline void SMTCore::load(Address addr) {
	prevContext->loadAddrs[prevContext->loads++] = addr;
}

inline void SMTCore::store(Address addr) {
	prevContext->storeAddrs[prevContext->stores++] = addr;
}

/* NOTE: Analysis routines cannot touch curCycle directly, must use
 * advance() for long jumps or insWindow.advancePos() for 1-cycle
 * jumps.
 *
 * UPDATE: With decodeCycle, this difference is more serious. ONLY
 * cSimStart and cSimEnd should call advance(). advance() is now meant
 * to advance the cycle counters in the whole core in lockstep.
 */
inline void SMTCore::advance(uint64_t targetCycle) {
	// advance internal cycle counts.
	decodeCycle += targetCycle - curCycle;
	insWindow.longAdvance(curCycle, targetCycle);
	curCycleRFReads = 0;
	curCycleIssuedUops = 0;
}

/** fPtrs Core Analysis functions. */
inline void SMTCore::branch(Address pc, bool taken, Address takenNpc, Address notTakenNpc) {
	prevContext->branchPc = pc;
	prevContext->branchTaken = taken;
	prevContext->branchTakenNpc = takenNpc;
	prevContext->branchNotTakenNpc = notTakenNpc;
}

/* OOOE: Refactored BBL function to store BBL's and not run them */
void SMTCore::bbl(THREADID tid, Address bblAddr, BblInfo* bblInfo) {
	if ( !prevContext->bbl ) {
        // This is the 1st BBL since scheduled, nothing to simulate
		prevContext->bbl = bblInfo;
		prevContext->bblAddress = bblAddr;
        // Kill lingering ops from previous BBL
		prevContext->loads = prevContext->stores = 0;
        return;
    }

	/* OOOE: Check to see that the right queue is filling up with the right PID's */
	uint8_t vcore = smtWindow->vcore;
	if( smtWindow->bblQueue[vcore].empty() ){
        if( smtWindow->bblQueue[1-vcore].empty() ){
            /* Assign and fill */
            smtWindow->bblQueue[vcore].pid = getpid();
        }
        else{
            if ( smtWindow->bblQueue[1-vcore].pid == getpid() ){
                /* Assign and fill but mark old */
                smtWindow->bblQueue[vcore].pid = getpid();
                smtWindow->bblQueue[vcore].older = true;
            }
            else{
                /* Assign and fill */
                smtWindow->bblQueue[vcore].pid = getpid();
            }
        }
	}
    else{
        /* Fill (keep marking same) */
        assert( smtWindow->bblQueue[vcore].pid == getpid() );
    }

    if( smtWindow->bblQueue[vcore].full() ){
        this->playback();
    }

    BblContext * curContext = nullptr;
	if(smtWindow->bblQueue[vcore].push(&curContext)){
		/* Store the bbl within a new context that fits in the queue */
		futex_lock(&windowLock);
		
		// construct and initialize new context from previous.
		curContext->bbl = prevContext->bbl;
		curContext->bblAddress = prevContext->bblAddress;
		curContext->loads = prevContext->loads;
		curContext->stores = prevContext->stores;

		// copy load and store addresses
		memcpy(curContext->loadAddrs, prevContext->loadAddrs, sizeof(curContext->loadAddrs));
		memcpy(curContext->storeAddrs, prevContext->storeAddrs, sizeof(curContext->storeAddrs));

		curContext->branchPc = prevContext->branchPc;
		curContext->branchTaken = prevContext->branchTaken;
		curContext->branchTakenNpc = prevContext->branchTakenNpc;
		curContext->branchNotTakenNpc = prevContext->branchNotTakenNpc;
		
		// set previous context
		prevContext->bbl = bblInfo;
		prevContext->bblAddress = bblAddr;
		prevContext->loads = prevContext->stores = 0;

		// update numContexts count.
		futex_unlock(&windowLock);
	}
	else{
		/* OOOE: Should not happen if playback worked correctly */
		panic("OOOE: failure on updating bbl queue");
	}

	// filled last context, time to sleep.
	if(smtWindow->bblQueue[vcore].full()) {
		//warn("(pid: %d, tid: %d, phase: %lu)", getpid(), tid, zinfo->numPhases + 1);
		// custom yield implementation.
		zinfo->sched->yield(getCid(tid));
	}
}

void SMTCore::LoadFunc(THREADID tid, ADDRINT addr) {
	static_cast<SMTCore*>(cores[tid])->load(addr);
}

void SMTCore::StoreFunc(THREADID tid, ADDRINT addr) {
	static_cast<SMTCore*>(cores[tid])->store(addr);
}

void SMTCore::PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    SMTCore* core = static_cast<SMTCore*>(cores[tid]);
    if (pred) core->load(addr);
    else core->predFalseMemOp();
}

void SMTCore::PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    SMTCore* core = static_cast<SMTCore*>(cores[tid]);
    if (pred) core->store(addr);
    else core->predFalseMemOp();
}

void SMTCore::BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    SMTCore* core = static_cast<SMTCore*>(cores[tid]);
    core->bbl(tid, bblAddr, bblInfo);
	while (core->curCycle > core->phaseEndCycle) {
	      core->phaseEndCycle += zinfo->phaseLength;
	
	      uint32_t cid = getCid(tid);
	      // NOTE: TakeBarrier may take ownership of the core, and so it will be used by some other thread. If TakeBarrier context-switches us,
	      // the *only* safe option is to return inmmediately after we detect this, or we can race and corrupt core state. However, the information
	      // here is insufficient to do that, so we could wind up double-counting phases.
	      uint32_t newCid = TakeBarrier(tid, cid);
	      // NOTE: Upon further observation, we cannot race if newCid == cid, so this code should be enough.
	      // It may happen that we had an intervening context-switch and we are now back to the same core.
	      // This is fine, since the loop looks at core values directly and there are no locals involved,
	      // so we should just advance as needed and move on.
	      if (newCid != cid) break;  /*context-switch, we do not own this context anymore*/
	}
}
void SMTCore::BranchFunc(THREADID tid, ADDRINT pc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc) {
    static_cast<SMTCore*>(cores[tid])->branch(pc, taken, takenNpc, notTakenNpc);
}


/* OOOE: playback()
 * Description: Function to run through both UOP Q's and execute the interleaved streams 
 * Input: (Implicit) The two BblContext queues that are filled a certain amount 
 * Output: None 
 */
void SMTCore::playback() {
	futex_lock(&windowLock);
    //info("OOOE: playback(%d) curCycle: %lu", getpid(), curCycle);
	//info("OOOE: core(%p) window upon entry: ({%u,%u}, {%u,%u})\n", this, smtWindow->bblQueue[0].count(), smtWindow->bblQueue[0].pid, smtWindow->bblQueue[1].count(), smtWindow->bblQueue[1].pid);

	/* OOOE: Objects to keep track of UOP, Bbl's and if there was a move to the next Bbl in the same Q */
    DynUop* uop;
	BblContext* bblContext;
    uint8_t curQ = 0;
    bool curBblSwap = false; // OOOE: In the current Q, needed to move to the next Bbl
	uint8_t curBblSwapQ = 0;
    uint32_t prevDecCycle = 0;
    uint64_t lastCommitCycle = 0;  // used to find misprediction penalty

	// OOOE: TODO: what should happen when we switch away from fair arbitration? 
	// we'll have to exit once ONE rather than BOTH window queues are empty. 
	// then it gets tricky once one of the processes has terminated. 
	// maybe a semaphore can fix this.
    while (getUop(curQ, &uop, &bblContext, curBblSwap, curBblSwapQ)){
		/* OOOE: Check if you need to run func's for a Bbl finishing */
		if(curBblSwap){
			curBblSwap = false;

			/* OOOE: Update the stats for the finished Bbl */
			runBblStatUpdate(smtWindow->prevContext[curBblSwapQ]);
			/* OOOE: Run the other functions (BranchPred, iFetch, Decode) */
			runFrontend(curBblSwapQ, smtWindow->loadId[curBblSwapQ], smtWindow->storeId[curBblSwapQ], lastCommitCycle, smtWindow->prevContext[curBblSwapQ]);
			/* OOOE: Clear the load/store indexes since Bbl finished */
			smtWindow->loadId[curBblSwapQ] = smtWindow->storeId[curBblSwapQ] = 0;
		}

		/* OOOE: Always guaranteed to have a UOP with Bbl */
        assert (uop != nullptr);
		assert (bblContext != 0);

        /* OOOE: Run the uop here similar to bbl() */
        runUop(curQ, smtWindow->loadId[curQ], smtWindow->storeId[curQ], prevDecCycle, lastCommitCycle, uop, bblContext);

		/* OOOE: Keep the previous pointer to the last Bbl (on a per Q basis) */
		smtWindow->prevContext[curQ] = bblContext;
	}
	
    /* OOOE: Check if you need to run func's for a Bbl finishing */
    /* OOOE: Rerun after the while loop so that the last Bbl in both Q's has stats and the 
       frontend run for it */
	if(curBblSwap){
		curBblSwap = false;

		/* OOOE: Update the stats for the finished Bbl */
		runBblStatUpdate(smtWindow->prevContext[curBblSwapQ]);
		/* OOOE: Run the other functions (BranchPred, iFetch, Decode) */
		runFrontend(curBblSwapQ, smtWindow->loadId[curBblSwapQ], smtWindow->storeId[curBblSwapQ], lastCommitCycle, smtWindow->prevContext[curBblSwapQ]);
		/* OOOE: Clear the load/store indexes since Bbl finished */
		smtWindow->loadId[curBblSwapQ] = smtWindow->storeId[curBblSwapQ] = 0;
	}
	
	//info("OOOE: Exit Playback ({%d,%d}, {%d,%d})", smtWindow->bblQueue[0].count(), smtWindow->bblQueue[0].pid, smtWindow->bblQueue[1].count(), smtWindow->bblQueue[1].pid);
	//info("OOOE: playback(%d) updated curCycle: %lu", getpid(), curCycle);
	futex_unlock(&windowLock);
}

/* OOOE: printUop()
 * Description: Function to print what UOP is being run, in which BBL, of which process
 * Input: UOP, BBL and all other pointers/indices 
 * Output: None 
 */
static inline void printUop(DynUop uop, BblContext& cntxt, pid_t pid, uint8_t curQ, uint16_t numContextTot, uint32_t curUop, uint64_t curCycle) {
	std::ostringstream oss1, oss2;
	oss1 << "tests/traces/" << "itrace" << ".csv";
	FILE *tfile = fopen(oss1.str().c_str(), "a+");
	oss2 << "tests/traces/" << "itrace_verbose" << ".txt";
	FILE *tfileVerb = fopen(oss2.str().c_str(), "a+");
	fprintf(tfile, "%u, %u, %u, %u, %u, ", pid, curQ, numContextTot, curUop, cntxt.bbl->oooBbl[0].uops);
	fprintf(tfileVerb, "PID:%u Q:%u BBL:%u UOP:%u/%u UOPTYPE:", pid, curQ, numContextTot, curUop, cntxt.bbl->oooBbl[0].uops);
	fprintf(tfile, "%" PRIu64 ", ", curCycle);
	if ( cntxt.bbl->oooBbl[0].uop[curUop].type == UOP_LOAD ){
		fprintf(tfile, "LOAD\n");
		fprintf(tfileVerb, "LOAD\n");
	}
	else if ( cntxt.bbl->oooBbl[0].uop[curUop].type == UOP_STORE ){
		fprintf(tfile, "STORE\n");
		fprintf(tfileVerb, "STORE\n");
	}
	else{
		fprintf(tfile, "OTHER\n");
		fprintf(tfileVerb, "OTHER\n");
	}
	fclose(tfile);
	fclose(tfileVerb);
}

/* OOOE: getUop()
 * Description: Function to get a specific UOP and BblContext object to run in playback()
 * Input: A DynUop and BblContext reference (Do not want a copy of them)
 * Output: Bool telling whether a UOP was retrieved (Only would happen in the case both Q's are empty)
 */
bool SMTCore::getUop(uint8_t &curQ, DynUop ** uop, BblContext ** bblContext, bool &curBblSwap, uint8_t &curBblSwapQ) {
	/* OOOE: Arbitration section: The UOP chosen is based on the core state, etc */
	if(!smtWindow->bblQueue[1 - curQ].empty()) {
		curQ ^= 1; 
	}
	/* OOOE: End: Arbitration section */
	
	//info("Q[0]:%d Q[1]:%d\n", smtWindow->bblQueue[0].count(), smtWindow->bblQueue[1].count());
	while ( true ){
		/* OOOE: Determine if there is a valid context to read in the Q */
		BblContext* cntxt;
		if (smtWindow->bblQueue[curQ].back(&cntxt)){
		    //info("CntxtPtr:%p BblInfoPtr:%p | OooBblPtr:%p #Instr:%u #Bytes:%u | #Uops:%u Addr:%lu #ApproxInstrs:%u\n", cntxt, cntxt->bbl, cntxt->bbl->oooBbl, cntxt->bbl->instrs, cntxt->bbl->bytes, cntxt->bbl->oooBbl[0].uops, cntxt->bbl->oooBbl[0].addr, cntxt->bbl->oooBbl[0].approxInstrs);
			/* OOOE: Determine if a UOP is present */
			if ( cntxt->bbl && smtWindow->uopIdx[curQ] < cntxt->bbl->oooBbl[0].uops ){
				/* OOOE: Get UOP and BblContext from current Q */
				*uop = &(cntxt->bbl->oooBbl[0].uop[smtWindow->uopIdx[curQ]]);
				*bblContext = cntxt;
				printUop(cntxt->bbl->oooBbl[0].uop[smtWindow->uopIdx[curQ]], *cntxt, smtWindow->bblQueue[curQ].pid, curQ, smtWindow->bblQueue[curQ].count(), smtWindow->uopIdx[curQ], curCycle);
				smtWindow->uopIdx[curQ] += 1;
				return true;
			} 
			else {
				/* OOOE: UOP not found. In the current Q move to the next BblContext */
				curBblSwap = true;
				curBblSwapQ = curQ;
				*bblContext = cntxt;
				smtWindow->uopIdx[curQ] = 0;
				if(!smtWindow->bblQueue[curQ].pop()){
					/* OOOE: Should not happen */
					panic("BblQueue failed to pop");
				}
			}
		}
		else {
			/* OOOE: No Bbl are left in the current queue so exit */
			smtWindow->uopIdx[curQ] = 0;
			smtWindow->bblQueue[curQ].clear();
			return false;
		}
	}
}

/* OOOE: printBBL()
 * Description: Function to print what BBL has completed, of which process
 * Input: BBL and all other pointers/indices 
 * Output: None 
 */
static inline void printBbl(BblContext& cntxt, pid_t pid) {
	std::ostringstream oss1;
	oss1 << "tests/traces/" << "bbl_trace" << ".csv";
	FILE *tfile = fopen(oss1.str().c_str(), "a+");
	fprintf(tfile, "%u, %u", pid, cntxt.bbl->oooBbl[0].uops);
	fclose(tfile);
}

/* OOOE: runBblStatUpdate()
 * Description: Function to update the core globals for simulation stats 
 * Input: A BblContext reference
 * Output: None 
 */
void SMTCore::runBblStatUpdate(BblContext* bblContext){
	/* OOOE: TODO: Implement instrs */
	//instrs += bblInstrs;
	uops += bblContext->bbl->oooBbl[0].uops;
	bbls++;
	approxInstrs += bblContext->bbl->oooBbl[0].approxInstrs;
	/* OOOE: Print end of BBL */
	printBbl(*bblContext, getpid());

#ifdef BBL_PROFILING
	fprintf(stderr, "OOOE: BBlProfiling enabled\n");
	if (approxInstrs) Decoder::profileBbl(bblContext->bbl->bblIdx);
#endif
}

inline void SMTCore::printContention(){
	info("OOOE: (%d, %lu) (%d, %lu)", smtWindow->bblQueue[0].pid, smtWindow->contentionMap[smtWindow->bblQueue[0].pid].cache, smtWindow->bblQueue[1].pid, smtWindow->contentionMap[smtWindow->bblQueue[1].pid].cache);
}

/* OOOE: runFrontend()
 * Description: Function to update the appropriate cycle counts for branchPred, iFetch and decode 
 * Input: Load/Store pointers for the current bblContext, previous Bbl commitcycle, and the current BblContext reference
 * Output: None 
 * 
 * Simulate frontend for branch pred + fetch of this BBL
 * NOTE: We assume that the instruction length predecoder and the IQ are
 * weak enough that they can't hide any ifetch or bpred stalls. In fact,
 * predecoder stalls are incorporated in the decode stall component (see
 * decoder.cpp). So here, we compute fetchCycle, then use it to adjust
 * decodeCycle. 
 */
void SMTCore::runFrontend(uint8_t presQ, uint32_t& loadIdx, uint32_t& storeIdx, uint64_t& lastCommitCycle, BblContext* bblContext){
	// Check full match between expected and actual mem ops
	// If these assertions fail, most likely, something's off in the decoder
	// assert_msg(loadIdx == bblContext->loads, "%s: loadIdx(%d) != loads (%d)", name.c_str(), loadIdx, bblContext->loads);
	// assert_msg(storeIdx == bblContext->stores, "%s: storeIdx(%d) != stores (%d)", name.c_str(), storeIdx, bblContext->stores);
	bblContext->loads = bblContext->stores = 0;

	// Model fetch-decode delay (fixed, weak predec/IQ assumption)
	uint64_t fetchCycle = decodeCycle - (DECODE_STAGE - FETCH_STAGE);
	uint32_t lineSize = 1 << lineBits;

	// Simulate branch prediction (Misprediction)
	if (bblContext->branchPc && !branchPred.predict(bblContext->branchPc, bblContext->branchTaken)) {
		/* OOOE: AG:
		 * Note: Here they start talking about BTB (a very basic branch pred) 
		 * but the brach predictor object used here is a pAg predictor.
		 *
		 * Per Address there is a history register.
		 * Then there is a global storage data structure that holds the branch locations.
		 */

		/* Simulate wrong-path fetches
		 *
		 * This is not for a latency reason, but sometimes it increases fetched
		 * code footprint and L1I MPKI significantly. Also, we assume a perfect
		 * BTB here: we always have the right address to missfetch on, and we
		 * never need resteering.
		 *
		 * NOTE: Resteering due to BTB misses is done at the BAC unit, is
		 * relatively rare, and carries an 8-cycle penalty, which should be
		 * partially hidden if the branch is predicted correctly --- so we
		 * don't simulate it.
		 *
		 * Since we don't have a BTB, we just assume the next branch is not
		 * taken. With a typical branch mispred penalty of 17 cycles, we
		 * typically fetch 3-4 lines in advance (16B/cycle). This sets a higher
		 * limit, which can happen with branches that take a long time to
		 * resolve (because e.g., they depend on a load). To set this upper
		 * bound, assume a completely backpressured IQ (18 instrs), uop queue
		 * (28 uops), IW (36 uops), and 16B instr length predecoder buffer. At
		 * ~3.5 bytes/instr, 1.2 uops/instr, this is about 5 64-byte lines.
		 */

		// info("Mispredicted branch, %ld %ld %ld | %ld %ld", 
		// decodeCycle, curCycle, lastCommitCycle, lastCommitCycle-decodeCycle, lastCommitCycle-curCycle);
		
		mispredBranches++;
		Address wrongPathAddr = bblContext->branchTaken ? 
			bblContext->branchNotTakenNpc : bblContext->branchTakenNpc;
		
		uint64_t reqCycle = fetchCycle;
		for (uint32_t i = 0; i < (5 * 64) / lineSize; i++) {
			//uint64_t prev = smtWindow->contentionMap[smtWindow->bblQueue[presQ].pid].cache;
			uint64_t fetchLat = l1i->loadSeparate(wrongPathAddr + (i * lineSize), curCycle, &smtWindow->contentionMap[smtWindow->bblQueue[presQ].pid].branchPrediction) - curCycle;
			/*if (prev != smtWindow->contentionMap[smtWindow->bblQueue[presQ].pid].cache){
				printContention();
			}*/
			cRec.record(curCycle, curCycle, curCycle + fetchLat);
			uint64_t respCycle = reqCycle + fetchLat;
			if (respCycle > lastCommitCycle) {
				break;
			}
			// Model fetch throughput limit
			reqCycle = respCycle + lineSize/FETCH_BYTES_PER_CYCLE;
		}

		fetchCycle = lastCommitCycle;
	}
	
	bblContext->branchPc = 0;  // clear for next BBL
	
	// Simulate current bbl instruction fetch
	Address endAddr = bblContext->bblAddress + bblContext->bbl->bytes;
	for (Address fetchAddr = bblContext->bblAddress; fetchAddr < endAddr; fetchAddr += lineSize) {
		// The Nehalem frontend fetches instructions in 16-byte-wide accesses.
		// Do not model fetch throughput limit here, decoder-generated stalls already include it
		// We always call fetches with curCycle to avoid upsetting the weave
		// models (but we could move to a fetch-centric recorder to avoid this)
		//uint64_t prev = smtWindow->contentionMap[smtWindow->bblQueue[presQ].pid].cache;
		uint64_t fetchLat = l1i->loadSeparate(fetchAddr, curCycle, &smtWindow->contentionMap[smtWindow->bblQueue[presQ].pid].branchPrediction) - curCycle;
		/*if (prev != smtWindow->contentionMap[smtWindow->bblQueue[presQ].pid].cache){
			printContention();
		}*/
		//TODO: something with cycle for instruction cache Barak
		cRec.record(curCycle, curCycle, curCycle + fetchLat);
		fetchCycle += fetchLat;
	}

	// If fetch rules, take into account delay between fetch and decode;
	// If decode rules, different BBLs make the decoders skip a cycle
	decodeCycle++;
	uint64_t minFetchDecCycle = fetchCycle + (DECODE_STAGE - FETCH_STAGE);
	if (minFetchDecCycle > decodeCycle) {

#ifdef SMT_STALL_STATS
		profFetchStalls.inc(decodeCycle - minFetchDecCycle);
#endif
		decodeCycle = minFetchDecCycle;
	}
	//info("FrontEnd Update: decodeCycle:%lu", decodeCycle);
}

/* OOOE: runUop()
 * Description: Function to update the appropriate cycle counts for decode and "execution" with just a UOP
 * Input: Load/Store pointers for the current bblContext, previous Bbl commitcycle, and the current UOP/BblContext reference
 * Output: None 
 */
void SMTCore::runUop(uint8_t presQ, uint32_t &loadIdx, uint32_t &storeIdx, uint32_t prevDecCycle, uint64_t &lastCommitCycle, DynUop * uop, BblContext *  bblContext) {
    DynBbl* bbl = &(bblContext->bbl->oooBbl[0]);
    assert( bbl != nullptr );
    assert( uop != nullptr );
	//info("\nrunUop: New UOP");

    uint32_t decDiff = uop->decCycle - prevDecCycle;
    decodeCycle = MAX(decodeCycle + decDiff, uopQueue.minAllocCycle());
	//info("curCycle:%lu prevDecCycle:%d decodeCycle:%lu", curCycle, prevDecCycle, decodeCycle);
    if (decodeCycle > curCycle) {
        uint32_t cdDiff = decodeCycle - curCycle;
#ifdef SMT_STALL_STATS
        profDecodeStalls.inc(cdDiff);
#endif
        curCycleIssuedUops = 0;
        curCycleRFReads = 0;
        for (uint32_t i = 0; i < cdDiff; i++) insWindow.advancePos(curCycle);
    }
    prevDecCycle = uop->decCycle;
    uopQueue.markLeave(curCycle);

    if (curCycleIssuedUops >= ISSUES_PER_CYCLE) {
#ifdef SMT_STALL_STATS
        profIssueStalls.inc();
#endif
        curCycleIssuedUops = 0;
        curCycleRFReads = 0;
        insWindow.advancePos(curCycle);
    }
    curCycleIssuedUops++;
	//info("curCycleIssuedUops:%d", curCycleIssuedUops);

    // Kill dependences on invalid register
    // Using curCycle saves us two unpredictable branches in the RF read stalls code
    regScoreboard[0] = curCycle;

    uint64_t c0 = regScoreboard[uop->rs[0]];
    uint64_t c1 = regScoreboard[uop->rs[1]];

    // RF read stalls
    // if srcs are not available at issue time, we have to go thru the RF
    curCycleRFReads += ((c0 < curCycle)? 1 : 0) + ((c1 < curCycle)? 1 : 0);
    if (curCycleRFReads > RF_READS_PER_CYCLE) {
        curCycleRFReads -= RF_READS_PER_CYCLE;
        curCycleIssuedUops = 0;  // or 1? that's probably a 2nd-order detail
        insWindow.advancePos(curCycle);
    }
	//info("curCycleRFReads:%d", curCycleRFReads);

    uint64_t c2 = rob.minAllocCycle();
    uint64_t c3 = curCycle;

    uint64_t cOps = MAX(c0, c1);

    // Model RAT + ROB + RS delay between issue and dispatch
    uint64_t dispatchCycle = MAX(cOps, MAX(c2, c3) + (DISPATCH_STAGE - ISSUE_STAGE));
	//info("dispatchCycle:%lu", dispatchCycle);

    // NOTE: Schedule can adjust both cur and dispatch cycles
    insWindow.schedule(curCycle, dispatchCycle, uop->portMask, uop->extraSlots);

    // If we have advanced, we need to reset the curCycle counters
    if (curCycle > c3) {
        curCycleIssuedUops = 0;
        curCycleRFReads = 0;
    }

    uint64_t commitCycle;

    // LSU simulation
    // NOTE: Ever-so-slightly faster than if-else if-else if-else
    switch (uop->type) {
        case UOP_GENERAL:
            commitCycle = dispatchCycle + uop->lat;
			//info("GEN: commitCycle:%lu", commitCycle);
            break;

        case UOP_LOAD:
            {
                // dispatchCycle = MAX(loadQueue.minAllocCycle(), dispatchCycle);
                uint64_t lqCycle = loadQueue.minAllocCycle();
                if (lqCycle > dispatchCycle) {
#ifdef LSU_IW_BACKPRESSURE
                    insWindow.poisonRange(curCycle, lqCycle, 0x4 /*PORT_2, loads*/);
#endif
                    dispatchCycle = lqCycle;
                }

                // Wait for all previous store addresses to be resolved
                dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

				Address addr = bblContext->loadAddrs[loadIdx++];
                uint64_t reqSatisfiedCycle = dispatchCycle;
                if (addr != ((Address)-1L)) {
					//uint64_t prev = smtWindow->contentionMap[smtWindow->bblQueue[presQ].pid].cache;
                    reqSatisfiedCycle = l1d->loadSeparate(addr, dispatchCycle, &smtWindow->contentionMap[smtWindow->bblQueue[presQ].pid].cache) + L1D_LAT;
					/*if (prev != smtWindow->contentionMap[smtWindow->bblQueue[presQ].pid].cache){
						printContention();
					}*/
                    cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);
                }

                // Enforce st-ld forwarding
                uint32_t fwdIdx = (addr>>2) & (FWD_ENTRIES-1);
                if (fwdArray[fwdIdx].addr == addr) {
                    // info("0x%lx FWD %ld %ld", addr, reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                    /* Take the MAX (see FilterCache's code) Our fwdArray
                     * imposes more stringent timing constraints than the
                     * l1d, b/c FilterCache does not change the line's
                     * availCycle on a store. This allows FilterCache to
                     * track per-line, not per-word availCycles.
                     */
                    reqSatisfiedCycle = MAX(reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                }

                commitCycle = reqSatisfiedCycle;
				//info("LOAD: commitCycle:%lu", commitCycle);
                loadQueue.markRetire(commitCycle);
            }
            break;

        case UOP_STORE:
            {
                // dispatchCycle = MAX(storeQueue.minAllocCycle(), dispatchCycle);
                uint64_t sqCycle = storeQueue.minAllocCycle();
                if (sqCycle > dispatchCycle) {
#ifdef LSU_IW_BACKPRESSURE
                    insWindow.poisonRange(curCycle, sqCycle, 0x10 /*PORT_4, stores*/);
#endif
                    dispatchCycle = sqCycle;
                }

                // Wait for all previous store addresses to be resolved (not just ours :))
                dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

				Address addr = bblContext->storeAddrs[storeIdx++];
                uint64_t reqSatisfiedCycle = l1d->store(addr, dispatchCycle) + L1D_LAT;
                cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);

                // Fill the forwarding table
                fwdArray[(addr>>2) & (FWD_ENTRIES-1)].set(addr, reqSatisfiedCycle);

                commitCycle = reqSatisfiedCycle;
                lastStoreCommitCycle = MAX(lastStoreCommitCycle, reqSatisfiedCycle);
				//info("STORE: commitCycle:%lu", commitCycle);
                storeQueue.markRetire(commitCycle);
            }
            break;

        case UOP_STORE_ADDR:
            commitCycle = dispatchCycle + uop->lat;
			//info("STOREADDR: commitCycle:%lu", commitCycle);
            lastStoreAddrCommitCycle = MAX(lastStoreAddrCommitCycle, commitCycle);
            break;

        //case UOP_FENCE:  //make gcc happy
        default:
            assert((UopType) uop->type == UOP_FENCE);
            commitCycle = dispatchCycle + uop->lat;
            // info("%d %ld %ld", uop->lat, lastStoreAddrCommitCycle, lastStoreCommitCycle);
            // force future load serialization
            lastStoreAddrCommitCycle = MAX(commitCycle, MAX(lastStoreAddrCommitCycle, lastStoreCommitCycle + uop->lat));
            // info("%d %ld %ld X", uop->lat, lastStoreAddrCommitCycle, lastStoreCommitCycle);

			//info("FENCE: commitCycle:%lu", commitCycle);
    }

    // Mark retire at ROB
    rob.markRetire(commitCycle);

    // Record dependences
    regScoreboard[uop->rd[0]] = commitCycle;
    regScoreboard[uop->rd[1]] = commitCycle;

    lastCommitCycle = commitCycle;

    // info("0x%lx %3d [%3d %3d] -> [%3d %3d]  %8ld %8ld %8ld %8ld", bbl->addr, i, 
	//   uop->rs[0], uop->rs[1], uop->rd[0], uop->rd[1], decCycle, c3, dispatchCycle, commitCycle);
}
