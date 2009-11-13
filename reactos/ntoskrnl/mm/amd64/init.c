/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/mm/amd64/init.c
 * PURPOSE:         Memory Manager Initialization for amd64
 *
 * PROGRAMMERS:     Timo kreuzer (timo.kreuzer@reactos.org)
 */

/* INCLUDES ***************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#include "../ARM3/miarm.h"

extern PMMPTE MmDebugPte;

/* GLOBALS *****************************************************************/

ULONG64 MmUserProbeAddress = 0x7FFFFFF0000ULL;
PVOID MmHighestUserAddress = (PVOID)0x7FFFFFEFFFFULL;
PVOID MmSystemRangeStart = (PVOID)KSEG0_BASE; // FFFF080000000000

/* Size of session view, pool, and image */
ULONG64 MmSessionSize = MI_SESSION_SIZE;
ULONG64 MmSessionViewSize = MI_SESSION_VIEW_SIZE;
ULONG64 MmSessionPoolSize = MI_SESSION_POOL_SIZE;
ULONG64 MmSessionImageSize = MI_SESSION_IMAGE_SIZE;

/* Session space addresses */
PVOID MiSessionSpaceEnd = MI_SESSION_SPACE_END; // FFFFF98000000000
PVOID MiSessionImageEnd;    // FFFFF98000000000 = MiSessionSpaceEnd
PVOID MiSessionImageStart;  // ?FFFFF97FFF000000 = MiSessionImageEnd - MmSessionImageSize
PVOID MiSessionViewEnd;     // FFFFF97FFF000000
PVOID MiSessionViewStart;   //  = MiSessionViewEnd - MmSessionViewSize
PVOID MiSessionPoolEnd;     //  = MiSessionViewStart
PVOID MiSessionPoolStart;   // FFFFF90000000000 = MiSessionPoolEnd - MmSessionPoolSize
PVOID MmSessionBase;        // FFFFF90000000000 = MiSessionPoolStart

/* System view */
ULONG64 MmSystemViewSize = MI_SYSTEM_VIEW_SIZE;
PVOID MiSystemViewStart;

ULONG64 MmMinimumNonPagedPoolSize = 256 * 1024;
ULONG64 MmSizeOfNonPagedPoolInBytes;
ULONG64 MmMaximumNonPagedPoolInBytes;
ULONG64 MmMaximumNonPagedPoolPercent;
ULONG64 MmMinAdditionNonPagedPoolPerMb = 32 * 1024;
ULONG64 MmMaxAdditionNonPagedPoolPerMb = 400 * 1024;
ULONG64 MmDefaultMaximumNonPagedPool = 1024 * 1024; 
PVOID MmNonPagedSystemStart;
PVOID MmNonPagedPoolStart;
PVOID MmNonPagedPoolExpansionStart;
PVOID MmNonPagedPoolEnd = MI_NONPAGED_POOL_END;

ULONG64 MmSizeOfPagedPoolInBytes = MI_MIN_INIT_PAGED_POOLSIZE;
PVOID MmPagedPoolStart = MI_PAGED_POOL_START;
PVOID MmPagedPoolEnd;


ULONG64 MmBootImageSize;
PPHYSICAL_MEMORY_DESCRIPTOR MmPhysicalMemoryBlock;
RTL_BITMAP MiPfnBitMap;
ULONG MmNumberOfPhysicalPages, MmHighestPhysicalPage, MmLowestPhysicalPage = -1; // FIXME: ULONG64
ULONG64 MmNumberOfSystemPtes;
PMMPTE MmSystemPagePtes;
ULONG64 MxPfnAllocation;
ULONG64 MxPfnSizeInBytes;

PVOID MmSystemCacheStart;
PVOID MmSystemCacheEnd;
MMSUPPORT MmSystemCacheWs;


///////////////////////////////////////////////

PMEMORY_ALLOCATION_DESCRIPTOR MxFreeDescriptor;
MEMORY_ALLOCATION_DESCRIPTOR MxOldFreeDescriptor;

PFN_NUMBER MxFreePageBase;
ULONG64 MxFreePageCount = 0;

ULONG
NoDbgPrint(const char *Format, ...)
{
    return 0;
}

VOID
NTAPI
MiEvaluateMemoryDescriptors(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PMEMORY_ALLOCATION_DESCRIPTOR MdBlock;
    PLIST_ENTRY ListEntry;
    PFN_NUMBER LastPage;

    /* Loop the memory descriptors */
    for (ListEntry = LoaderBlock->MemoryDescriptorListHead.Flink;
         ListEntry != &LoaderBlock->MemoryDescriptorListHead;
         ListEntry = ListEntry->Flink)
    {
        /* Get the memory descriptor */
        MdBlock = CONTAINING_RECORD(ListEntry,
                                    MEMORY_ALLOCATION_DESCRIPTOR,
                                    ListEntry);

        /* Skip pages that are not part of the PFN database */
        if ((MdBlock->MemoryType == LoaderFirmwarePermanent) ||
            (MdBlock->MemoryType == LoaderBBTMemory) ||
            (MdBlock->MemoryType == LoaderHALCachedMemory) ||
            (MdBlock->MemoryType == LoaderSpecialMemory) ||
            (MdBlock->MemoryType == LoaderBad))
        {
            continue;
        }

        /* Add this to the total of pages */
        MmNumberOfPhysicalPages += MdBlock->PageCount;

        /* Check if this is the new lowest page */
        if (MdBlock->BasePage < MmLowestPhysicalPage)
        {
            /* Update the lowest page */
            MmLowestPhysicalPage = MdBlock->BasePage;
        }

        /* Check if this is the new highest page */
        LastPage = MdBlock->BasePage + MdBlock->PageCount - 1;
        if (LastPage > MmHighestPhysicalPage)
        {
            /* Update the highest page */
            MmHighestPhysicalPage = LastPage;
        }

        /* Check if this is currently free memory */
        if ((MdBlock->MemoryType == LoaderFree) ||
            (MdBlock->MemoryType == LoaderLoadedProgram) ||
            (MdBlock->MemoryType == LoaderFirmwareTemporary) ||
            (MdBlock->MemoryType == LoaderOsloaderStack))
        {
            /* Check if this is the largest memory descriptor */
            if (MdBlock->PageCount > MxFreePageCount)
            {
                /* For now, it is */
                MxFreeDescriptor = MdBlock;
                MxFreePageBase = MdBlock->BasePage;
                MxFreePageCount = MdBlock->PageCount;
            }
        }
    }
}

PFN_NUMBER
NTAPI
MxGetNextPage(IN PFN_NUMBER PageCount)
{
    PFN_NUMBER Pfn;

    /* Make sure we have enough pages */
    if (PageCount > MxFreePageCount)
    {
        /* Crash the system */
        KeBugCheckEx(INSTALL_MORE_MEMORY,
                     MmNumberOfPhysicalPages,
                     MxFreeDescriptor->PageCount,
                     MxOldFreeDescriptor.PageCount,
                     PageCount);
    }

    /* Use our lowest usable free pages */
    Pfn = MxFreePageBase;
    MxFreePageBase += PageCount;
    MxFreePageCount -= PageCount;
    return Pfn;
}

PMMPTE
NTAPI
MxGetPte(PVOID Address)
{
    PMMPTE Pte;
    MMPTE TmpPte;

    /* Setup template pte */
    TmpPte.u.Long = 0;
    TmpPte.u.Flush.Valid = 1;
    TmpPte.u.Flush.Write = 1;

    /* Get a pointer to the PXE */
    Pte = MiAddressToPxe(Address);
    if (!Pte->u.Hard.Valid)
    {
        /* It's not valid, map it! */
        TmpPte.u.Hard.PageFrameNumber = MxGetNextPage(1);
        *Pte = TmpPte;
    }

    /* Get a pointer to the PPE */
    Pte = MiAddressToPpe(Address);
    if (!Pte->u.Hard.Valid)
    {
        /* It's not valid, map it! */
        TmpPte.u.Hard.PageFrameNumber = MxGetNextPage(1);
        *Pte = TmpPte;
    }

    /* Get a pointer to the PDE */
    Pte = MiAddressToPde(Address);
    if (!Pte->u.Hard.Valid)
    {
        /* It's not valid, map it! */
        TmpPte.u.Hard.PageFrameNumber = MxGetNextPage(1);
        *Pte = TmpPte;
    }

    /* Get a pointer to the PTE */
    Pte = MiAddressToPte(Address);
    return Pte;
}

VOID
MxMapPageRange(PVOID Address, ULONG64 PageCount)
{
    MMPTE TmpPte, *Pte;

    /* Setup template pte */
    TmpPte.u.Long = 0;
    TmpPte.u.Flush.Valid = 1;
    TmpPte.u.Flush.Write = 1;

    while (PageCount--)
    {
        /* Get the PTE for that page */
        Pte = MxGetPte(Address);
        ASSERT(Pte->u.Hard.Valid == 0);

        /* Map a physical page */
        TmpPte.u.Hard.PageFrameNumber = MxGetNextPage(1);
        *Pte = TmpPte;

        /* Goto next page */
        Address = (PVOID)((ULONG64)Address + PAGE_SIZE);
    }
}

VOID
NTAPI
MiArmConfigureMemorySizes(IN PLOADER_PARAMETER_BLOCK LoaderBloc)
{
    /* Get the size of the boot loader's image allocations */
    MmBootImageSize = KeLoaderBlock->Extension->LoaderPagesSpanned * PAGE_SIZE;
    MmBootImageSize = ROUND_UP(MmBootImageSize, 4 * 1024 * 1024);

    /* Check if this is a machine with less than 256MB of RAM, and no overide */
    if ((MmNumberOfPhysicalPages <= MI_MIN_PAGES_FOR_NONPAGED_POOL_TUNING) &&
        !(MmSizeOfNonPagedPoolInBytes))
    {
        /* Force the non paged pool to be 2MB so we can reduce RAM usage */
        MmSizeOfNonPagedPoolInBytes = 2 * 1024 * 1024;
    }

    /* Check if the user gave a ridicuously large nonpaged pool RAM size */
    if ((MmSizeOfNonPagedPoolInBytes >> PAGE_SHIFT) >
        (MmNumberOfPhysicalPages * 7 / 8))
    {
        /* More than 7/8ths of RAM was dedicated to nonpaged pool, ignore! */
        MmSizeOfNonPagedPoolInBytes = 0;
    }

    /* Check if no registry setting was set, or if the setting was too low */
    if (MmSizeOfNonPagedPoolInBytes < MmMinimumNonPagedPoolSize)
    {
        /* Start with the minimum (256 KB) and add 32 KB for each MB above 4 */
        MmSizeOfNonPagedPoolInBytes = MmMinimumNonPagedPoolSize;
        MmSizeOfNonPagedPoolInBytes += (MmNumberOfPhysicalPages - 1024) /
                                       256 * MmMinAdditionNonPagedPoolPerMb;
    }

    /* Check if the registy setting or our dynamic calculation was too high */
    if (MmSizeOfNonPagedPoolInBytes > MI_MAX_INIT_NONPAGED_POOL_SIZE)
    {
        // Set it to the maximum */
        MmSizeOfNonPagedPoolInBytes = MI_MAX_INIT_NONPAGED_POOL_SIZE;
    }

    /* Check if a percentage cap was set through the registry */
    if (MmMaximumNonPagedPoolPercent)
    {
        /* Don't feel like supporting this right now */
        UNIMPLEMENTED;
    }

    /* Page-align the nonpaged pool size */
    MmSizeOfNonPagedPoolInBytes &= ~(PAGE_SIZE - 1);
    
    /* Now, check if there was a registry size for the maximum size */
    if (!MmMaximumNonPagedPoolInBytes)
    {
        /* Start with the default (1MB) and add 400 KB for each MB above 4 */
        MmMaximumNonPagedPoolInBytes = MmDefaultMaximumNonPagedPool;
        MmMaximumNonPagedPoolInBytes += (MmNumberOfPhysicalPages - 1024) /
                                         256 * MmMaxAdditionNonPagedPoolPerMb;
    }
    
    /* Don't let the maximum go too high */
    if (MmMaximumNonPagedPoolInBytes > MI_MAX_NONPAGED_POOL_SIZE)
    {
        /* Set it to the upper limit */
        MmMaximumNonPagedPoolInBytes = MI_MAX_NONPAGED_POOL_SIZE;
    }

    // MmSessionImageSize
}

VOID
NTAPI
MiArmInitializeMemoryLayout(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Set up session space */
    MiSessionSpaceEnd = (PVOID)MI_SESSION_SPACE_END;

    /* This is where we will load Win32k.sys and the video driver */
    MiSessionImageEnd = MiSessionSpaceEnd;
    MiSessionImageStart = (PVOID)((ULONG_PTR)MiSessionImageEnd -
                                  MmSessionImageSize);

    /* The view starts right below the session working set (itself below
     * the image area) */
    MiSessionViewEnd = MI_SESSION_VIEW_END;
    MiSessionViewStart = (PVOID)((ULONG_PTR)MiSessionViewStart -
                                 MmSessionViewSize);

    /* Session pool follows */
    MiSessionPoolEnd = MiSessionViewStart;
    MiSessionPoolStart = (PVOID)((ULONG_PTR)MiSessionPoolEnd -
                                 MmSessionPoolSize);

    /* And it all begins here */
    MmSessionBase = MiSessionPoolStart;

    /* System view space ends at session space, so now that we know where
     * this is, we can compute the base address of system view space itself. */
    MiSystemViewStart = (PVOID)((ULONG_PTR)MmSessionBase -
                                MmSystemViewSize);

    /* Use the default */
    MmNumberOfSystemPtes = 22000;

    ASSERT(MiSessionViewEnd <= MiSessionImageStart);
    ASSERT(MmSessionBase <= MiSessionPoolStart);
}

VOID
MiArmInitializePageTable()
{
    ULONG64 PageFrameOffset;
    PMMPTE Pte, StartPte, EndPte;

    /* Get current directory base */
    PageFrameOffset = ((PMMPTE)PXE_SELFMAP)->u.Hard.PageFrameNumber << PAGE_SHIFT;
    ASSERT(PageFrameOffset == __readcr3());

    /* Set directory base for the system process */
    PsGetCurrentProcess()->Pcb.DirectoryTableBase[0] = PageFrameOffset;

    /* HACK: don't use freeldr debug pront anymore */
    FrLdrDbgPrint = NoDbgPrint;

#if 1
    /* Clear user mode mappings in PML4 */
    StartPte = MiAddressToPxe(0);
    EndPte = MiAddressToPxe(MmHighestUserAddress);

    for (Pte = StartPte; Pte <= EndPte; Pte++)
    {
        /* Zero the pte */
        Pte->u.Long = 0;
    }
#else
    /* Clear user mode mappings in PML4 */
    StartPte = MiAddressToPte(0);
    EndPte = MiAddressToPte((PVOID)0xa00000);

    for (Pte = StartPte; Pte < EndPte; Pte++)
    {
        /* Zero the pte */
        //Pte->u.Long = 0;
    }

    /* Flush the TLB */
    KeFlushCurrentTb();

//    MiAddressToPde(0)->u.Long = 0;
//    MiAddressToPde((PVOID)0x200000)->u.Long = 0;
//    MiAddressToPde((PVOID)0x400000)->u.Long = 0;
//    MiAddressToPde((PVOID)0x600000)->u.Long = 0;
//    MiAddressToPde((PVOID)0x800000)->u.Long = 0;

   // MiAddressToPpe->u.Long = 0;

#endif

    /* Flush the TLB */
    KeFlushCurrentTb();

    /* Setup debug mapping pte */
    MmDebugPte = MxGetPte(MI_DEBUG_MAPPING);
}


VOID
NTAPI
MiArmPreparePfnDatabse(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PMEMORY_ALLOCATION_DESCRIPTOR MdBlock;
    PLIST_ENTRY ListEntry;
    PFN_COUNT PageCount;
    PVOID PageBase;

    /* The PFN database is at the start of the non paged region */
    MmPfnDatabase = (PVOID)((ULONG64)MmNonPagedPoolEnd - MmMaximumNonPagedPoolInBytes);

    /* Loop the memory descriptors */
    for (ListEntry = LoaderBlock->MemoryDescriptorListHead.Flink;
         ListEntry != &LoaderBlock->MemoryDescriptorListHead;
         ListEntry = ListEntry->Flink)
    {
        /* Get the memory descriptor */
        MdBlock = CONTAINING_RECORD(ListEntry,
                                    MEMORY_ALLOCATION_DESCRIPTOR,
                                    ListEntry);

        /* Skip pages that are not part of the PFN database */
        if ((MdBlock->MemoryType == LoaderFirmwarePermanent) ||
            (MdBlock->MemoryType == LoaderBBTMemory) ||
            (MdBlock->MemoryType == LoaderHALCachedMemory) ||
            (MdBlock->MemoryType == LoaderSpecialMemory) ||
            (MdBlock->MemoryType != LoaderBad))
        {
            continue;
        }

        /* Map pages for the PFN database */
        PageCount = ROUND_TO_PAGES(MdBlock->PageCount * sizeof(MMPFN)) / PAGE_SIZE;
        PageBase = PAGE_ALIGN(&MmPfnDatabase[MdBlock->BasePage]);
        MxMapPageRange(PageBase, PageCount);

        /* Zero out the pages */
        RtlZeroMemory(PageBase, PageCount * PAGE_SIZE);
    }

    /* Calculate the number of bytes, and then convert to pages */
    MxPfnSizeInBytes = ROUND_TO_PAGES(MmHighestPhysicalPage + 1) * sizeof(MMPFN);
    MxPfnAllocation = MxPfnSizeInBytes >> PAGE_SHIFT;

    /* Reduce maximum pool size */
    MmMaximumNonPagedPoolInBytes -= MxPfnSizeInBytes;
}


VOID
NTAPI
MiArmPrepareNonPagedPool()
{
    PFN_NUMBER PageCount;
    PVOID Address;

    /* Non paged pool comes after the PFN database */
    MmNonPagedPoolStart = (PVOID)((ULONG64)MmPfnDatabase +
                                  MxPfnSizeInBytes);
    ASSERT((ULONG64)MmNonPagedPoolEnd == (ULONG64)MmNonPagedPoolStart +
                                  MmMaximumNonPagedPoolInBytes);

    /* Calculate the nonpaged pool expansion start region */
    MmNonPagedPoolExpansionStart = (PVOID)((ULONG_PTR)MmNonPagedPoolEnd -
                                  MmMaximumNonPagedPoolInBytes +
                                  MmSizeOfNonPagedPoolInBytes);
    MmNonPagedPoolExpansionStart = (PVOID)PAGE_ALIGN(MmNonPagedPoolExpansionStart);

    /* Now calculate the nonpaged system VA region, which includes the
     * nonpaged pool expansion (above) and the system PTEs. Note that it is
     * then aligned to a PDE boundary (4MB). */
    MmNonPagedSystemStart = (PVOID)((ULONG_PTR)MmNonPagedPoolExpansionStart -
                                    (MmNumberOfSystemPtes + 1) * PAGE_SIZE);
    MmNonPagedSystemStart = (PVOID)((ULONG_PTR)MmNonPagedSystemStart &
                                    ~((4 * 1024 * 1024) - 1));

    /* Don't let it go below the minimum */
    if (MmNonPagedSystemStart < (PVOID)MI_NON_PAGED_SYSTEM_START_MIN)
    {
        /* This is a hard-coded limit in the Windows NT address space */
        MmNonPagedSystemStart = (PVOID)MI_NON_PAGED_SYSTEM_START_MIN;

        /* Reduce the amount of system PTEs to reach this point */
        MmNumberOfSystemPtes = ((ULONG_PTR)MmNonPagedPoolExpansionStart -
                                (ULONG_PTR)MmNonPagedSystemStart) >>
                                PAGE_SHIFT;
        MmNumberOfSystemPtes--;
        ASSERT(MmNumberOfSystemPtes > 1000);
    }

    /* Map the nonpaged pool */
    PageCount = (MmSizeOfNonPagedPoolInBytes + PAGE_SIZE - 1) / PAGE_SIZE;
    MxMapPageRange(MmNonPagedPoolStart, PageCount);

    /* Create PTEs for the paged pool extension */
    for (Address = MmNonPagedPoolExpansionStart;
         Address < MmNonPagedPoolEnd;
         Address = (PVOID)((ULONG64)Address + PAGE_SIZE))
    {
        /* Create PXE, PPE, PDE and set PTE to 0*/
        MxGetPte(Address)->u.Long = 0;
    }

//DPRINT1("MmNonPagedPoolStart = %p, Pte=%p \n", MmNonPagedPoolStart, MiAddressToPte(MmNonPagedPoolStart));
    /* Sanity check */
    ASSERT(MiAddressToPte(MmNonPagedSystemStart) <
           MiAddressToPte(MmNonPagedPoolExpansionStart));

}

NTSTATUS
NTAPI
MmArmInitSystem(IN ULONG Phase,
                IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    if (Phase == 0)
    {
        /* Parse memory descriptors */
        MiEvaluateMemoryDescriptors(LoaderBlock);

        /* Configure the memory sizes */
        MiArmConfigureMemorySizes(LoaderBlock);

        /* Initialize the memory layout */
        MiArmInitializeMemoryLayout(LoaderBlock);

        /* Prepare PFN database mappings */
        MiArmPreparePfnDatabse(LoaderBlock);

        /* Initialize some mappings */
        MiArmInitializePageTable();

        /* Prepare paged pool mappings */
        MiArmPrepareNonPagedPool();

        /* Initialize the ARM3 nonpaged pool */
        MiInitializeArmPool();

        /* Update the memory descriptor, to make sure the pages we used
           won't get inserted into the PFN database */
        MxOldFreeDescriptor = *MxFreeDescriptor;
        MxFreeDescriptor->BasePage = MxFreePageBase;
        MxFreeDescriptor->PageCount = MxFreePageCount;
    }
    else if (Phase == 1)
    {
        /* The PFN database was created, restore the free descriptor */
        *MxFreeDescriptor = MxOldFreeDescriptor;

        ASSERT(FALSE);

    }

    return STATUS_SUCCESS;
}

