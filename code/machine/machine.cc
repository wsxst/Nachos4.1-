// machine.cc
//	Routines for simulating the execution of user programs.
//
//  DO NOT CHANGE -- part of the machine emulation
//
// Copyright (c) 1992-1996 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation
// of liability and disclaimer of warranty provisions.

#include "copyright.h"
#include "machine.h"
#include "main.h"
#include <limits.h>

// Textual names of the exceptions that can be generated by user program
// execution, for debugging.
static char *exceptionNames[] = {"no exception", "syscall",
                                 "page fault", "tlb miss", "page read only",
                                 "bus error", "address error", "overflow",
                                 "illegal instruction"};

//----------------------------------------------------------------------
// CheckEndian
// 	Check to be sure that the host really uses the format it says it
//	does, for storing the bytes of an integer.  Stop on error.
//----------------------------------------------------------------------

static void CheckEndian()
{
    union checkit {
        char charword[4];
        unsigned int intword;
    } check;

    check.charword[0] = 1;
    check.charword[1] = 2;
    check.charword[2] = 3;
    check.charword[3] = 4;

#ifdef HOST_IS_BIG_ENDIAN
    ASSERT(check.intword == 0x01020304);
#else
    ASSERT(check.intword == 0x04030201);
#endif
}

//----------------------------------------------------------------------
// Machine::Machine
// 	Initialize the simulation of user program execution.
//
//	"debug" -- if TRUE, drop into the debugger after each user instruction
//		is executed.
//----------------------------------------------------------------------

Machine::Machine(bool debug)
{
    int i;

    mmBitmap = new Bitmap(NumPhysPages);
    for (i = 0; i < NumTotalRegs; i++)
        registers[i] = 0;
    mainMemory = new char[MemorySize];
    for (i = 0; i < MemorySize; i++)
        mainMemory[i] = 0;
#ifdef USE_TLB
    tlb = new TranslationEntry[TLBSize];
    for (i = 0; i < TLBSize; i++)
    {
        tlb[i].reset();
    }
#else
    tlb = NULL;
#endif
#ifdef USE_RPT
    pt = new TranslationEntry[NumPhysPages];
    for(int i=0;i<NumPhysPages;++i) pt[i].reset();
#else
    pt = NULL;
#endif

    singleStep = debug;
    CheckEndian();
}

//----------------------------------------------------------------------
// Machine::~Machine
// 	De-allocate the data structures used to simulate user program execution.
//----------------------------------------------------------------------

Machine::~Machine()
{
    delete mmBitmap;
    delete[] mainMemory;
    if (tlb != NULL)
        delete[] tlb;
}

//----------------------------------------------------------------------
// Machine::RaiseException
// 	Transfer control to the Nachos kernel from user mode, because
//	the user program either invoked a system call, or some exception
//	occured (such as the address translation failed).
//
//	"which" -- the cause of the kernel trap
//	"badVaddr" -- the virtual address causing the trap, if appropriate
//----------------------------------------------------------------------

void Machine::RaiseException(ExceptionType which, int badVAddr)
{
    DEBUG(dbgMach, "Exception: " << exceptionNames[which]);

    registers[BadVAddrReg] = badVAddr;
    DelayedLoad(0, 0); // finish anything in progress
    kernel->interrupt->setStatus(SystemMode);
    ExceptionHandler(which); // interrupts are enabled at this point
    kernel->interrupt->setStatus(UserMode);
}

//----------------------------------------------------------------------
// Machine::Debugger
// 	Primitive debugger for user programs.  Note that we can't use
//	gdb to debug user programs, since gdb doesn't run on top of Nachos.
//	It could, but you'd have to implement *a lot* more system calls
//	to get it to work!
//
//	So just allow single-stepping, and printing the contents of memory.
//----------------------------------------------------------------------

void Machine::Debugger()
{
    char *buf = new char[80];
    int num;
    bool done = FALSE;

    kernel->interrupt->DumpState();
    DumpState();
    while (!done)
    {
        // read commands until we should proceed with more execution
        // prompt for input, giving current simulation time in the prompt
        cout << kernel->stats->totalTicks << ">";
        // read one line of input (80 chars max)
        cin.get(buf, 80);
        if (sscanf(buf, "%d", &num) == 1)
        {
            runUntilTime = num;
            done = TRUE;
        }
        else
        {
            runUntilTime = 0;
            switch (*buf)
            {
            case '\0':
                done = TRUE;
                break;
            case 'c':
                singleStep = FALSE;
                done = TRUE;
                break;
            case '?':
                cout << "Machine commands:\n";
                cout << "    <return>  execute one instruction\n";
                cout << "    <number>  run until the given timer tick\n";
                cout << "    c         run until completion\n";
                cout << "    ?         print help message\n";
                break;
            default:
                cout << "Unknown command: " << buf << "\n";
                cout << "Type ? for help.\n";
            }
        }
        // consume the newline delimiter, which does not get
        // eaten by cin.get(buf,80) above.
        buf[0] = cin.get();
    }
    delete[] buf;
}

//----------------------------------------------------------------------
// Machine::DumpState
// 	Print the user program's CPU state.  We might print the contents
//	of memory, but that seemed like overkill.
//----------------------------------------------------------------------

void Machine::DumpState()
{
    int i;

    cout << "Machine registers:\n";
    for (i = 0; i < NumGPRegs; i++)
    {
        switch (i)
        {
        case StackReg:
            cout << "\tSP(" << i << "):\t" << registers[i];
            break;

        case RetAddrReg:
            cout << "\tRA(" << i << "):\t" << registers[i];
            break;

        default:
            cout << "\t" << i << ":\t" << registers[i];
            break;
        }
        if ((i % 4) == 3)
        {
            cout << "\n";
        }
    }

    cout << "\tHi:\t" << registers[HiReg];
    cout << "\tLo:\t" << registers[LoReg];
    cout << "\tPC:\t" << registers[PCReg];
    cout << "\tNextPC:\t" << registers[NextPCReg];
    cout << "\tPrevPC:\t" << registers[PrevPCReg];
    cout << "\tLoad:\t" << registers[LoadReg];
    cout << "\tLoadV:\t" << registers[LoadValueReg] << "\n";
}

//----------------------------------------------------------------------
// Machine::ReadRegister/WriteRegister
//   	Fetch or write the contents of a user program register.
//----------------------------------------------------------------------

int Machine::ReadRegister(int num)
{
    ASSERT((num >= 0) && (num < NumTotalRegs));
    return registers[num];
}

void Machine::WriteRegister(int num, int value)
{
    ASSERT((num >= 0) && (num < NumTotalRegs));
    registers[num] = value;
}

int Machine::findAvailablePageFrame()
{
    return mmBitmap->FindAndSet();
}

void Machine::showTLB()
{
    cerr<<"TLB now:\nvpn\tppn\ttID\tvalid\treadonly\tuse\tdirty\tFIFO\tLRU\n";
    for(int i=0;i<TLBSize;++i)
    {
        cerr<<tlb[i].vpn<<"\t"<<tlb[i].ppn<<"\t"<<tlb[i].tID<<"\t"<<tlb[i].valid<<"\t"<<tlb[i].readOnly<<"\t"<<tlb[i].use<<"\t"<<tlb[i].dirty<<"\t"<<tlb[i].FIFOFlag<<"\t"<<tlb[i].LRUFlag<<endl;
    }
}

void Machine::showRPT()
{
    cerr<<"RPT now:\nppn\tvpn\tTID\tvalid\treadonly\tuse\tdirty\tFIFO\tLRU\n";
    for(int i=0;i<NumPhysPages;++i)
    {
        if(pt[i].vpn!=-1)
        {
            cerr<<i<<"\t"<<pt[i].vpn<<"\t"<<pt[i].tID<<"\t"<<pt[i].valid<<"\t"<<pt[i].readOnly<<"\t"<<pt[i].use<<"\t"<<pt[i].dirty<<"\t"<<pt[i].FIFOFlag<<"\t"<<pt[i].LRUFlag<<endl;
        }
    }
}

/*
 * type:0 refers to page table,1 refers to TLB
 */
int Machine::findOneToReplace(TranslationEntry* t,int type)
{
	int targetv,targeti=0,len;
    if(type == 1)
    {
        len = TLBSize;
    }
    else
    {
#ifdef USE_RPT
        len = NumPhysPages;
#else
        len = pageTableSize;
#endif
    }
    targetv = INT_MAX;
    for(int i=0;i<len;++i)
    {
#ifdef FIFO_REPLACE
        if(targetv>t[i].FIFOFlag)
        {
            targetv = t[i].FIFOFlag;
            targeti = i;
        }
#endif
#ifdef LRU_REPLACE
        if(targetv>t[i].LRUFlag)
        {
            targetv = t[i].LRUFlag;
            targeti = i;
        }
#endif
    }
    return targeti;
}

void Machine::updateFIFOFlag(TranslationEntry* t, int pos, int len)
{
    int num=-1;
    for(int i=0;i<len;++i)
    {
        if(t[i].valid&&i!=pos)
        {
            num = num>=t[i].FIFOFlag?num:t[i].FIFOFlag;
        }
    }
    (*(t+pos)).FIFOFlag = num + 1;
}

void Machine::updateLRUFlag(TranslationEntry* t, int pos, int len)
{
	int num=-1;
    for(int i=0;i<len;++i)
    {
        if(t[i].valid&&i!=pos)
        {
            num = num>=t[i].LRUFlag?num:t[i].LRUFlag;
        }
    }
    (*(t+pos)).LRUFlag = num + 1;
}

void Machine::updateTLB(TranslationEntry* tlb, TranslationEntry entry)
{
    DEBUG(dbgAddr,"Update TLB!");
    int i;
    for(i=0;i<TLBSize;++i)
    {
        if(!tlb[i].valid) break;
    }
    if(i == TLBSize)
    {
        i = findOneToReplace(tlb, 1);
        if(debug->IsEnabled('a')) cerr<<"Replace tlb #"<<i<<endl;
    }
    *(tlb+i) = entry;
#ifdef FIFO_REPLACE
    updateFIFOFlag(tlb, i, TLBSize);
#endif
}