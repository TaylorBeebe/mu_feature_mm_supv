/** @file
Agent Module to load other modules to deploy SMM Entry Vector for X86 CPU.

Copyright (c) 2009 - 2019, Intel Corporation. All rights reserved.<BR>
Copyright (c) 2017, AMD Incorporated. All rights reserved.<BR>

SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Relocate.h"
#include "Mem/Mem.h"
#include "Services/MpService/MpService.h"
#include "MmSupervisorCore.h"

CPU_HOT_PLUG_DATA  mCpuHotPlugData = {
  CPU_HOT_PLUG_DATA_REVISION_1,                 // Revision
  0,                                            // Array Length of SmBase and APIC ID
  NULL,                                         // Pointer to APIC ID array
  NULL,                                         // Pointer to SMBASE array
  0,                                            // Reserved
  0,                                            // SmrrBase
  0                                             // SmrrSize
};

//
// SMM Relocation variables
//
volatile BOOLEAN  *mRebased;
volatile BOOLEAN  mIsBsp;

///
/// Handle for the SMM CPU Protocol
///
EFI_HANDLE  mSmmCpuHandle = NULL;

EFI_CPU_INTERRUPT_HANDLER  mExternalVectorTable[EXCEPTION_VECTOR_NUMBER];

//
// SMM stack information
//
UINTN  mSmmStackArrayBase;
UINTN  mSmmStackArrayEnd;
UINTN  mSmmStackSize;

UINTN  mSmmCpl3StackArrayBase;
UINTN  mSmmCpl3StackArrayEnd;

UINTN    mSmmShadowStackSize;
BOOLEAN  mCetSupported = TRUE;

UINTN  mMaxNumberOfCpus = 1;
UINTN  mNumberOfCpus    = 1;

//
// SMM ready to lock flag
//
BOOLEAN  mSmmReadyToLock = FALSE;

//
// Global used to cache PCD for SMM Code Access Check enable
//
BOOLEAN  mSmmCodeAccessCheckEnable = FALSE;

//
// Global copy of the PcdPteMemoryEncryptionAddressOrMask
//
UINT64  mAddressEncMask = 0;

//
// Spin lock used to serialize setting of SMM Code Access Check feature
//
SPIN_LOCK  *mConfigSmmCodeAccessCheckLock = NULL;

//
// Saved SMM ranges information
//
EFI_SMRAM_DESCRIPTOR  *mSmmCpuSmramRanges;
UINTN                 mSmmCpuSmramRangeCount;
//
// MSCHANGE [BEGIN] - Add flag to enable "test mode" for the SMM protections.
//                    NOTE: "Test mode" will only be enabled in DEBUG builds.
// Flag to indicate exception handling should be in test mode.
// This will cause exceptions to reset the system and/or log
// additional telemetry.
//

// Driver-wide global variable to hold CR3 inside SMM
UINT32  mSmmCr3;

/**
  Enable exception handling test mode.

  NOTE: This should only work on debug builds, otherwise return EFI_UNSUPPORTED.

  @retval EFI_SUCCESS            Test mode enabled.
  @retval EFI_UNSUPPORTED        Test mode could not be enabled.

**/
EFI_STATUS
EFIAPI
// MU_CHANGE
EnableSmmExceptionTestMode (
  VOID
  );

//
// Protocol for other drivers to enable test mode.
//
SMM_EXCEPTION_TEST_PROTOCOL  mSmmExceptionTestProtocol = {
  EnableSmmExceptionTestMode
};
EFI_HANDLE                   mSmmExceptionTestProtocolHandle = NULL;

BOOLEAN  mSmmRebootOnException = TRUE;
// MSCHANGE [END]

//
// Control register contents saved for SMM S3 resume state initialization.
//
UINT32  mSmmCr0;
UINT32  mSmmCr4;

/**
  Initialize IDT to setup exception handlers for SMM.

**/
VOID
InitializeSmmIdt (
  VOID
  )
{
  EFI_STATUS       Status;
  BOOLEAN          InterruptState;
  IA32_DESCRIPTOR  DxeIdtr;

  //
  // There are 32 (not 255) entries in it since only processor
  // generated exceptions will be handled.
  //
  gcSmiIdtr.Limit = (sizeof (IA32_IDT_GATE_DESCRIPTOR) * 32) - 1;
  //
  // Allocate page aligned IDT, because it might be set as read only.
  //
  gcSmiIdtr.Base = (UINTN)AllocateCodePages (EFI_SIZE_TO_PAGES (gcSmiIdtr.Limit + 1));
  ASSERT (gcSmiIdtr.Base != 0);
  ZeroMem ((VOID *)gcSmiIdtr.Base, gcSmiIdtr.Limit + 1);

  //
  // Disable Interrupt and save DXE IDT table
  //
  InterruptState = SaveAndDisableInterrupts ();
  AsmReadIdtr (&DxeIdtr);
  //
  // Load SMM temporary IDT table
  //
  AsmWriteIdtr (&gcSmiIdtr);
  //
  // Setup SMM default exception handlers, SMM IDT table
  // will be updated and saved in gcSmiIdtr
  //
  Status = InitializeCpuExceptionHandlers (NULL);
  ASSERT_EFI_ERROR (Status);
  //
  // Restore DXE IDT table and CPU interrupt
  //
  AsmWriteIdtr ((IA32_DESCRIPTOR *)&DxeIdtr);
  SetInterruptState (InterruptState);
}

/**
  Search module name by input IP address and output it.

  @param CallerIpAddress   Caller instruction pointer.

**/
VOID
DumpModuleInfoByIp (
  IN  UINTN  CallerIpAddress
  )
{
  UINTN  Pe32Data;
  VOID   *PdbPointer;

  //
  // Find Image Base
  //
  Pe32Data = PeCoffSearchImageBase (CallerIpAddress);
  if (Pe32Data != 0) {
    DEBUG ((DEBUG_ERROR, "It is invoked from the instruction before IP(0x%p)", (VOID *)CallerIpAddress));
    PdbPointer = PeCoffLoaderGetPdbPointer ((VOID *)Pe32Data);
    if (PdbPointer != NULL) {
      DEBUG ((DEBUG_ERROR, " in module (%a)\n", PdbPointer));
    }
  }
}

/**
  C function for SMI handler. To change all processor's SMMBase Register.

**/
VOID
EFIAPI
SmmInitHandler (
  VOID
  )
{
  UINT32  ApicId;
  UINTN   Index;

  //
  // Update SMM IDT entries' code segment and load IDT
  //
  AsmWriteIdtr (&gcSmiIdtr);
  ApicId = GetApicId ();

  ASSERT (mNumberOfCpus <= mMaxNumberOfCpus);

  for (Index = 0; Index < mNumberOfCpus; Index++) {
    if (ApicId == (UINT32)gSmmCpuPrivate->ProcessorInfo[Index].ProcessorId) {
      //
      // Initialize SMM specific features on the currently executing CPU
      //
      SmmCpuFeaturesInitializeProcessor (
        Index,
        mIsBsp,
        gSmmCpuPrivate->ProcessorInfo,
        &mCpuHotPlugData
        );

      //
      // Check XD and BTS features on each processor on normal boot
      //
      CheckFeatureSupported ();

      if (mIsBsp) {
        //
        // BSP rebase is already done above.
        // Initialize private data during S3 resume
        //
        InitializeMpSyncData ();
      }

      //
      // Hook return after RSM to set SMM re-based flag
      //
      SemaphoreHook (Index, &mRebased[Index]);

      return;
    }
  }

  ASSERT (FALSE);
}

/**
  Relocate SmmBases for each processor.

  Execute on first boot and all S3 resumes

**/
VOID
EFIAPI
SmmRelocateBases (
  VOID
  )
{
  UINT8                 BakBuf[BACK_BUF_SIZE];
  SMRAM_SAVE_STATE_MAP  BakBuf2;
  SMRAM_SAVE_STATE_MAP  *CpuStatePtr;
  UINT8                 *U8Ptr;
  UINT32                ApicId;
  UINTN                 Index;
  UINTN                 BspIndex;

  //
  // Make sure the reserved size is large enough for procedure SmmInitTemplate.
  //
  ASSERT (sizeof (BakBuf) >= gcSmmInitSize);

  //
  // Patch ASM code template with current CR0, CR3, and CR4 values
  //
  mSmmCr0 = (UINT32)AsmReadCr0 ();
  PatchInstructionX86 (gPatchSmmCr0, mSmmCr0, 4);
  PatchInstructionX86 (gPatchSmmCr3, AsmReadCr3 (), 4);
  mSmmCr4 = (UINT32)AsmReadCr4 ();
  PatchInstructionX86 (gPatchSmmCr4, mSmmCr4 & (~CR4_CET_ENABLE), 4);

  //
  // Patch GDTR for SMM base relocation
  //
  gcSmiInitGdtr.Base  = gcSmiGdtr.Base;
  gcSmiInitGdtr.Limit = gcSmiGdtr.Limit;

  U8Ptr       = (UINT8 *)(UINTN)(SMM_DEFAULT_SMBASE + SMM_HANDLER_OFFSET);
  CpuStatePtr = (SMRAM_SAVE_STATE_MAP *)(UINTN)(SMM_DEFAULT_SMBASE + SMRAM_SAVE_STATE_MAP_OFFSET);

  //
  // Backup original contents at address 0x38000
  //
  CopyMem (BakBuf, U8Ptr, sizeof (BakBuf));
  CopyMem (&BakBuf2, CpuStatePtr, sizeof (BakBuf2));

  //
  // Load image for relocation
  //
  CopyMem (U8Ptr, gcSmmInitTemplate, gcSmmInitSize);

  //
  // Retrieve the local APIC ID of current processor
  //
  ApicId = GetApicId ();

  //
  // Relocate SM bases for all APs
  // This is APs' 1st SMI - rebase will be done here, and APs' default SMI handler will be overridden by gcSmmInitTemplate
  //
  mIsBsp   = FALSE;
  BspIndex = (UINTN)-1;
  for (Index = 0; Index < mNumberOfCpus; Index++) {
    mRebased[Index] = FALSE;
    if (ApicId != (UINT32)gSmmCpuPrivate->ProcessorInfo[Index].ProcessorId) {
      SendSmiIpi ((UINT32)gSmmCpuPrivate->ProcessorInfo[Index].ProcessorId);
      //
      // Wait for this AP to finish its 1st SMI
      //
      while (!mRebased[Index]) {
      }
    } else {
      //
      // BSP will be Relocated later
      //
      BspIndex = Index;
    }
  }

  //
  // Relocate BSP's SMM base
  //
  ASSERT (BspIndex != (UINTN)-1);
  mIsBsp = TRUE;
  SendSmiIpi (ApicId);
  //
  // Wait for the BSP to finish its 1st SMI
  //
  while (!mRebased[BspIndex]) {
  }

  //
  // Restore contents at address 0x38000
  //
  CopyMem (CpuStatePtr, &BakBuf2, sizeof (BakBuf2));
  CopyMem (U8Ptr, BakBuf, sizeof (BakBuf));
}

EFI_STATUS
EFIAPI
SmmInitializeMemoryAttributesTable (
  IN CONST EFI_GUID  *Protocol,
  IN VOID            *Interface,
  IN EFI_HANDLE      Handle
  );

/**
  SMM Ready To Lock event notification handler.

  The CPU S3 data is copied to SMRAM for security and mSmmReadyToLock is set to
  perform additional lock actions that must be performed from SMM on the next SMI.

  @param[in] Protocol   Points to the protocol's unique identifier.
  @param[in] Interface  Points to the interface instance.
  @param[in] Handle     The handle on which the interface was installed.

  @retval EFI_SUCCESS   Notification handler runs successfully.
 **/
VOID
EFIAPI
LockMmCoreBeforeExit (
  VOID
  )
{
  EFI_STATUS  Status;

  // This will stand for the initial locking, which will cover that:
  // a. Core data and code set to supervisor pages
  // b. DXE Core MAT is not available at this moment, thus just mark everything but SMRAM as not present
  // c. Starting from here, all memory allocated by this driver shall be CPL0 unless otherwise noticed
  // d. Once common buffer is available, the core shall be notified to accept that memory buffer
  // e. IPL lock through SMM access protocol

  // Initializes MAT for SMM region (so far there is only Core memory pages)
  SmmInitializeMemoryAttributesTable (NULL, NULL, NULL);

  // Mark supervisor pages for critical regions
  // Core code and data
  // SmiEntry code
  // Exception handler
  // GDT and its buffer
  // Save State

  // We need to do this based off MM CR3
  SetPageTableBase (mSmmCr3);

  //
  // Start SMM Profile feature
  //
  if (FeaturePcdGet (PcdCpuSmmProfileEnable)) {
    SmmProfileStart ();
  }

  //
  // Create a mix of 2MB and 4KB page table. Update some memory ranges absent and execute-disable.
  //
  InitPaging ();

  // Grab all hob resource decriptors, find the ones that does not overlap with SMRAM, mark them
  // as not present
  SetNonSmmMemMapAttributes ();

  // Unblock the common regions reported during PEI phase
  SetCommonBufferRegionAttribute ();

  // Unblocked other requested regions reported during PEI phase
  SetUnblockRegionAttribute ();

  // Protect the requested regions reported during PEI phase
  SetProtectedRegionAttribute ();

  //
  // Mark critical region to be read-only in page table
  //
  SetMemMapAttributes ();

  Status = LockFfsBuffer ();
  ASSERT_EFI_ERROR (Status);

  if (IsRestrictedMemoryAccess ()) {
    //
    // Set page table itself to be read-only
    //
    SetPageTableAttributes ();
  }

  SmmCpuFeaturesCompleteSmmReadyToLock ();

  SetPageTableBase (0);
}

/**
  The module Entry Point of the CPU SMM driver.

  @param  ImageHandle    The firmware allocated handle for the EFI image.
  @param  SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS    The entry point is executed successfully.
  @retval Other          Some error occurs when executing this entry point.

**/
EFI_STATUS
EFIAPI
SetupSmiEntryExit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  VOID        *Buffer;
  UINTN       BufferPages;
  UINTN       TileCodeSize;
  UINTN       TileDataSize;
  UINTN       TileSize;
  UINT8       *Stacks;
  // VOID                       *Registration;
  UINT32  RegEax;
  UINT32  RegEbx;
  UINT32  RegEcx;
  UINT32  RegEdx;
  UINTN   FamilyId;
  UINTN   ModelId;
  UINT32  Cr3;

  UINT8  *Cpl3Stacks;

  //
  // Initialize address fixup
  //
  PiSmmCpuSmmInitFixupAddress ();
  PiSmmCpuSmiEntryFixupAddress ();

  //
  // Initialize Debug Agent to support source level debug in SMM code
  //
  InitializeDebugAgent (DEBUG_AGENT_INIT_SMM, NULL, NULL);

  //
  // Report the start of CPU SMM initialization.
  //
  REPORT_STATUS_CODE (
    EFI_PROGRESS_CODE,
    EFI_COMPUTING_UNIT_HOST_PROCESSOR | EFI_CU_HP_PC_SMM_INIT
    );

  mSmmRebootOnException = PcdGetBool (PcdSmmExceptionRebootInsteadOfHaltDefault); // MS_CHANGE

  //
  // Find out SMRR Base and SMRR Size
  //
  FindSmramInfo (&mCpuHotPlugData.SmrrBase, &mCpuHotPlugData.SmrrSize);

  //
  // Extract the MP information from the Hoblist
  //
  VOID  *GuidHob = NULL;

  GuidHob = GetFirstGuidHob (&gMpInformationHobGuid);
  ASSERT (GuidHob != NULL);
  MP_INFORMATION_HOB_DATA  *MpInformationHobData = GET_GUID_HOB_DATA (GuidHob);

  //
  // Use MP Services Protocol to retrieve the number of processors and number of enabled processors
  //
  mNumberOfCpus = MpInformationHobData->NumberOfProcessors;
  ASSERT (mNumberOfCpus <= PcdGet32 (PcdCpuMaxLogicalProcessorNumber));

  //
  // If support CPU hot plug, PcdCpuSmmEnableBspElection should be set to TRUE.
  // A constant BSP index makes no sense because it may be hot removed.
  //
  DEBUG_CODE (
    if (FeaturePcdGet (PcdCpuHotPlugSupport)) {
    ASSERT (FeaturePcdGet (PcdCpuSmmEnableBspElection));
  }

    );

  //
  // Save the PcdCpuSmmCodeAccessCheckEnable value into a global variable.
  //
  mSmmCodeAccessCheckEnable = PcdGetBool (PcdCpuSmmCodeAccessCheckEnable);
  DEBUG ((DEBUG_INFO, "PcdCpuSmmCodeAccessCheckEnable = %d\n", mSmmCodeAccessCheckEnable));

  //
  // Save the PcdPteMemoryEncryptionAddressOrMask value into a global variable.
  // Make sure AddressEncMask is contained to smallest supported address field.
  //
  mAddressEncMask = 0;// PcdGet64 (PcdPteMemoryEncryptionAddressOrMask) & PAGING_1G_ADDRESS_MASK_64;
  DEBUG ((DEBUG_INFO, "mAddressEncMask = 0x%lx\n", mAddressEncMask));

  //
  // If support CPU hot plug, we need to allocate resources for possibly hot-added processors
  //
  if (FeaturePcdGet (PcdCpuHotPlugSupport)) {
    mMaxNumberOfCpus = PcdGet32 (PcdCpuMaxLogicalProcessorNumber);
  } else {
    mMaxNumberOfCpus = mNumberOfCpus;
  }

  gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus = mMaxNumberOfCpus;

  //
  // The CPU save state and code for the SMI entry point are tiled within an SMRAM
  // allocated buffer.  The minimum size of this buffer for a uniprocessor system
  // is 32 KB, because the entry point is SMBASE + 32KB, and CPU save state area
  // just below SMBASE + 64KB.  If more than one CPU is present in the platform,
  // then the SMI entry point and the CPU save state areas can be tiles to minimize
  // the total amount SMRAM required for all the CPUs.  The tile size can be computed
  // by adding the   // CPU save state size, any extra CPU specific context, and
  // the size of code that must be placed at the SMI entry point to transfer
  // control to a C function in the native SMM execution mode.  This size is
  // rounded up to the nearest power of 2 to give the tile size for a each CPU.
  // The total amount of memory required is the maximum number of CPUs that
  // platform supports times the tile size.  The picture below shows the tiling,
  // where m is the number of tiles that fit in 32KB.
  //
  //  +-----------------------------+  <-- 2^n offset from Base of allocated buffer
  //  |   CPU m+1 Save State        |
  //  +-----------------------------+
  //  |   CPU m+1 Extra Data        |
  //  +-----------------------------+
  //  |   Padding                   |
  //  +-----------------------------+
  //  |   CPU 2m  SMI Entry         |
  //  +#############################+  <-- Base of allocated buffer + 64 KB
  //  |   CPU m-1 Save State        |
  //  +-----------------------------+
  //  |   CPU m-1 Extra Data        |
  //  +-----------------------------+
  //  |   Padding                   |
  //  +-----------------------------+
  //  |   CPU 2m-1 SMI Entry        |
  //  +=============================+  <-- 2^n offset from Base of allocated buffer
  //  |   . . . . . . . . . . . .   |
  //  +=============================+  <-- 2^n offset from Base of allocated buffer
  //  |   CPU 2 Save State          |
  //  +-----------------------------+
  //  |   CPU 2 Extra Data          |
  //  +-----------------------------+
  //  |   Padding                   |
  //  +-----------------------------+
  //  |   CPU m+1 SMI Entry         |
  //  +=============================+  <-- Base of allocated buffer + 32 KB
  //  |   CPU 1 Save State          |
  //  +-----------------------------+
  //  |   CPU 1 Extra Data          |
  //  +-----------------------------+
  //  |   Padding                   |
  //  +-----------------------------+
  //  |   CPU m SMI Entry           |
  //  +#############################+  <-- Base of allocated buffer + 32 KB == CPU 0 SMBASE + 64 KB
  //  |   CPU 0 Save State          |
  //  +-----------------------------+
  //  |   CPU 0 Extra Data          |
  //  +-----------------------------+
  //  |   Padding                   |
  //  +-----------------------------+
  //  |   CPU m-1 SMI Entry         |
  //  +=============================+  <-- 2^n offset from Base of allocated buffer
  //  |   . . . . . . . . . . . .   |
  //  +=============================+  <-- 2^n offset from Base of allocated buffer
  //  |   Padding                   |
  //  +-----------------------------+
  //  |   CPU 1 SMI Entry           |
  //  +=============================+  <-- 2^n offset from Base of allocated buffer
  //  |   Padding                   |
  //  +-----------------------------+
  //  |   CPU 0 SMI Entry           |
  //  +#############################+  <-- Base of allocated buffer == CPU 0 SMBASE + 32 KB
  //

  //
  // Retrieve CPU Family
  //
  AsmCpuid (CPUID_VERSION_INFO, &RegEax, NULL, NULL, NULL);
  FamilyId = (RegEax >> 8) & 0xf;
  ModelId  = (RegEax >> 4) & 0xf;
  if ((FamilyId == 0x06) || (FamilyId == 0x0f)) {
    ModelId = ModelId | ((RegEax >> 12) & 0xf0);
  }

  RegEdx = 0;
  AsmCpuid (CPUID_EXTENDED_FUNCTION, &RegEax, NULL, NULL, NULL);
  if (RegEax >= CPUID_EXTENDED_CPU_SIG) {
    AsmCpuid (CPUID_EXTENDED_CPU_SIG, NULL, NULL, NULL, &RegEdx);
  }

  //
  // Determine the mode of the CPU at the time an SMI occurs
  //   Intel(R) 64 and IA-32 Architectures Software Developer's Manual
  //   Volume 3C, Section 34.4.1.1
  //
  mSmmSaveStateRegisterLma = EFI_SMM_SAVE_STATE_REGISTER_LMA_32BIT;
  if ((RegEdx & BIT29) != 0) {
    mSmmSaveStateRegisterLma = EFI_SMM_SAVE_STATE_REGISTER_LMA_64BIT;
  }

  if (FamilyId == 0x06) {
    if ((ModelId == 0x17) || (ModelId == 0x0f) || (ModelId == 0x1c)) {
      mSmmSaveStateRegisterLma = EFI_SMM_SAVE_STATE_REGISTER_LMA_64BIT;
    }
  }

  DEBUG ((DEBUG_INFO, "PcdControlFlowEnforcementPropertyMask = %d\n", PcdGet32 (PcdControlFlowEnforcementPropertyMask)));
  if (PcdGet32 (PcdControlFlowEnforcementPropertyMask) != 0) {
    AsmCpuid (CPUID_SIGNATURE, &RegEax, NULL, NULL, NULL);
    if (RegEax >= CPUID_STRUCTURED_EXTENDED_FEATURE_FLAGS) {
      AsmCpuidEx (CPUID_STRUCTURED_EXTENDED_FEATURE_FLAGS, CPUID_STRUCTURED_EXTENDED_FEATURE_FLAGS_SUB_LEAF_INFO, NULL, NULL, &RegEcx, &RegEdx);
      DEBUG ((DEBUG_INFO, "CPUID[7/0] ECX - 0x%08x\n", RegEcx));
      DEBUG ((DEBUG_INFO, "  CET_SS  - 0x%08x\n", RegEcx & CPUID_CET_SS));
      DEBUG ((DEBUG_INFO, "  CET_IBT - 0x%08x\n", RegEdx & CPUID_CET_IBT));
      if ((RegEcx & CPUID_CET_SS) == 0) {
        mCetSupported = FALSE;
        PatchInstructionX86 (mPatchCetSupported, mCetSupported, 1);
      }

      if (mCetSupported) {
        AsmCpuidEx (CPUID_EXTENDED_STATE, CPUID_EXTENDED_STATE_SUB_LEAF, NULL, &RegEbx, &RegEcx, NULL);
        DEBUG ((DEBUG_INFO, "CPUID[D/1] EBX - 0x%08x, ECX - 0x%08x\n", RegEbx, RegEcx));
        AsmCpuidEx (CPUID_EXTENDED_STATE, 11, &RegEax, NULL, &RegEcx, NULL);
        DEBUG ((DEBUG_INFO, "CPUID[D/11] EAX - 0x%08x, ECX - 0x%08x\n", RegEax, RegEcx));
        AsmCpuidEx (CPUID_EXTENDED_STATE, 12, &RegEax, NULL, &RegEcx, NULL);
        DEBUG ((DEBUG_INFO, "CPUID[D/12] EAX - 0x%08x, ECX - 0x%08x\n", RegEax, RegEcx));
      }
    } else {
      mCetSupported = FALSE;
      PatchInstructionX86 (mPatchCetSupported, mCetSupported, 1);
    }
  } else {
    mCetSupported = FALSE;
    PatchInstructionX86 (mPatchCetSupported, mCetSupported, 1);
  }

  //
  // Compute tile size of buffer required to hold the CPU SMRAM Save State Map, extra CPU
  // specific context start starts at SMBASE + SMM_PSD_OFFSET, and the SMI entry point.
  // This size is rounded up to nearest power of 2.
  //
  TileCodeSize = GetSmiHandlerSize ();
  TileCodeSize = ALIGN_VALUE (TileCodeSize, SIZE_4KB);
  TileDataSize = (SMRAM_SAVE_STATE_MAP_OFFSET - SMM_PSD_OFFSET) + sizeof (SMRAM_SAVE_STATE_MAP);
  TileDataSize = ALIGN_VALUE (TileDataSize, SIZE_4KB);
  TileSize     = TileDataSize + TileCodeSize - 1;
  TileSize     = 2 * GetPowerOfTwo32 ((UINT32)TileSize);
  DEBUG ((DEBUG_INFO, "SMRAM TileSize = 0x%08x (0x%08x, 0x%08x)\n", TileSize, TileCodeSize, TileDataSize));

  //
  // If the TileSize is larger than space available for the SMI Handler of
  // CPU[i], the extra CPU specific context of CPU[i+1], and the SMRAM Save
  // State Map of CPU[i+1], then ASSERT().  If this ASSERT() is triggered, then
  // the SMI Handler size must be reduced or the size of the extra CPU specific
  // context must be reduced.
  //
  ASSERT (TileSize <= (SMRAM_SAVE_STATE_MAP_OFFSET + sizeof (SMRAM_SAVE_STATE_MAP) - SMM_HANDLER_OFFSET));

  //
  // Allocate buffer for all of the tiles.
  //
  // Intel(R) 64 and IA-32 Architectures Software Developer's Manual
  // Volume 3C, Section 34.11 SMBASE Relocation
  //   For Pentium and Intel486 processors, the SMBASE values must be
  //   aligned on a 32K Byte boundary or the processor will enter shutdown
  //   state during the execution of a RSM instruction.
  //
  // Intel486 processors: FamilyId is 4
  // Pentium processors : FamilyId is 5
  //
  BufferPages = EFI_SIZE_TO_PAGES (SIZE_32KB + TileSize * (mMaxNumberOfCpus - 1));
  if ((FamilyId == 4) || (FamilyId == 5)) {
    Buffer = AllocateAlignedCodePages (BufferPages, SIZE_32KB);
  } else {
    Buffer = AllocateAlignedCodePages (BufferPages, SIZE_4KB);
  }

  ASSERT (Buffer != NULL);
  DEBUG ((DEBUG_INFO, "SMRAM SaveState Buffer (0x%08x, 0x%08x)\n", Buffer, EFI_PAGES_TO_SIZE (BufferPages)));

  //
  // Allocate buffer for pointers to array in  SMM_CPU_PRIVATE_DATA.
  //
  gSmmCpuPrivate->ProcessorInfo = (EFI_PROCESSOR_INFORMATION *)AllocatePool (sizeof (EFI_PROCESSOR_INFORMATION) * mMaxNumberOfCpus);
  ASSERT (gSmmCpuPrivate->ProcessorInfo != NULL);

  gSmmCpuPrivate->Operation = (SMM_CPU_OPERATION *)AllocatePool (sizeof (SMM_CPU_OPERATION) * mMaxNumberOfCpus);
  ASSERT (gSmmCpuPrivate->Operation != NULL);

  gSmmCpuPrivate->CpuSaveStateSize = (UINTN *)AllocatePool (sizeof (UINTN) * mMaxNumberOfCpus);
  ASSERT (gSmmCpuPrivate->CpuSaveStateSize != NULL);

  gSmmCpuPrivate->CpuSaveState = (VOID **)AllocatePool (sizeof (VOID *) * mMaxNumberOfCpus);
  ASSERT (gSmmCpuPrivate->CpuSaveState != NULL);

  gSmmCpuPrivate->SmmCoreEntryContext.CpuSaveStateSize = gSmmCpuPrivate->CpuSaveStateSize;
  gSmmCpuPrivate->SmmCoreEntryContext.CpuSaveState     = gSmmCpuPrivate->CpuSaveState;

  //
  // Allocate buffer for pointers to array in CPU_HOT_PLUG_DATA.
  //
  mCpuHotPlugData.ApicId = (UINT64 *)AllocatePool (sizeof (UINT64) * mMaxNumberOfCpus);
  ASSERT (mCpuHotPlugData.ApicId != NULL);
  mCpuHotPlugData.SmBase = (UINTN *)AllocatePool (sizeof (UINTN) * mMaxNumberOfCpus);
  ASSERT (mCpuHotPlugData.SmBase != NULL);
  mCpuHotPlugData.ArrayLength = (UINT32)mMaxNumberOfCpus;

  //
  // Retrieve APIC ID of each enabled processor from the MP Services protocol.
  // Also compute the SMBASE address, CPU Save State address, and CPU Save state
  // size for each CPU in the platform
  //
  for (Index = 0; Index < mMaxNumberOfCpus; Index++) {
    mCpuHotPlugData.SmBase[Index]           = (UINTN)Buffer + Index * TileSize - SMM_HANDLER_OFFSET;
    gSmmCpuPrivate->CpuSaveStateSize[Index] = sizeof (SMRAM_SAVE_STATE_MAP);
    gSmmCpuPrivate->CpuSaveState[Index]     = (VOID *)(mCpuHotPlugData.SmBase[Index] + SMRAM_SAVE_STATE_MAP_OFFSET);
    gSmmCpuPrivate->Operation[Index]        = SmmCpuNone;

    if (Index < mNumberOfCpus) {
      CopyMem (&gSmmCpuPrivate->ProcessorInfo[Index], &MpInformationHobData->ProcessorInfoBuffer[Index], sizeof (EFI_PROCESSOR_INFORMATION));
      mCpuHotPlugData.ApicId[Index] = gSmmCpuPrivate->ProcessorInfo[Index].ProcessorId;

      DEBUG ((
        DEBUG_INFO,
        "CPU[%03x]  APIC ID=%04x  SMBASE=%08x  SaveState=%08x  Size=%08x\n",
        Index,
        (UINT32)gSmmCpuPrivate->ProcessorInfo[Index].ProcessorId,
        mCpuHotPlugData.SmBase[Index],
        gSmmCpuPrivate->CpuSaveState[Index],
        gSmmCpuPrivate->CpuSaveStateSize[Index]
        ));
    } else {
      gSmmCpuPrivate->ProcessorInfo[Index].ProcessorId = INVALID_APIC_ID;
      mCpuHotPlugData.ApicId[Index]                    = INVALID_APIC_ID;
    }
  }

  //
  // Allocate SMI stacks for all processors.
  //
  mSmmStackSize = EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (PcdGet32 (PcdCpuSmmStackSize)));
  if (FeaturePcdGet (PcdCpuSmmStackGuard)) {
    //
    // SMM Stack Guard Enabled
    //   2 more pages is allocated for each processor, one is guard page and the other is known good stack.
    //
    // +--------------------------------------------------+-----+--------------------------------------------------+
    // | Known Good Stack | Guard Page |     SMM Stack    | ... | Known Good Stack | Guard Page |     SMM Stack    |
    // +--------------------------------------------------+-----+--------------------------------------------------+
    // |        4K        |    4K       PcdCpuSmmStackSize|     |        4K        |    4K       PcdCpuSmmStackSize|
    // |<---------------- mSmmStackSize ----------------->|     |<---------------- mSmmStackSize ----------------->|
    // |                                                  |     |                                                  |
    // |<------------------ Processor 0 ----------------->|     |<------------------ Processor n ----------------->|
    //
    mSmmStackSize += EFI_PAGES_TO_SIZE (2);
  }

  mSmmShadowStackSize = 0;
  if ((PcdGet32 (PcdControlFlowEnforcementPropertyMask) != 0) && mCetSupported) {
    mSmmShadowStackSize = EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (PcdGet32 (PcdCpuSmmShadowStackSize)));

    if (FeaturePcdGet (PcdCpuSmmStackGuard)) {
      //
      // SMM Stack Guard Enabled
      // Append Shadow Stack after normal stack
      //   2 more pages is allocated for each processor, one is guard page and the other is known good shadow stack.
      //
      // |= Stacks
      // +--------------------------------------------------+---------------------------------------------------------------+
      // | Known Good Stack | Guard Page |    SMM Stack     | Known Good Shadow Stack | Guard Page |    SMM Shadow Stack    |
      // +--------------------------------------------------+---------------------------------------------------------------+
      // |         4K       |    4K      |PcdCpuSmmStackSize|            4K           |    4K      |PcdCpuSmmShadowStackSize|
      // |<---------------- mSmmStackSize ----------------->|<--------------------- mSmmShadowStackSize ------------------->|
      // |                                                                                                                  |
      // |<-------------------------------------------- Processor N ------------------------------------------------------->|
      //
      mSmmShadowStackSize += EFI_PAGES_TO_SIZE (2);
    } else {
      //
      // SMM Stack Guard Disabled (Known Good Stack is still required for potential stack switch.)
      //   Append Shadow Stack after normal stack with 1 more page as known good shadow stack.
      //   1 more pages is allocated for each processor, it is known good stack.
      //
      //
      // |= Stacks
      // +-------------------------------------+--------------------------------------------------+
      // | Known Good Stack |    SMM Stack     | Known Good Shadow Stack |    SMM Shadow Stack    |
      // +-------------------------------------+--------------------------------------------------+
      // |        4K        |PcdCpuSmmStackSize|          4K             |PcdCpuSmmShadowStackSize|
      // |<---------- mSmmStackSize ---------->|<--------------- mSmmShadowStackSize ------------>|
      // |                                                                                        |
      // |<-------------------------------- Processor N ----------------------------------------->|
      //
      mSmmShadowStackSize += EFI_PAGES_TO_SIZE (1);
      mSmmStackSize       += EFI_PAGES_TO_SIZE (1);
    }
  }

  Stacks = (UINT8 *)AllocatePages (gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus * (EFI_SIZE_TO_PAGES (mSmmStackSize + mSmmShadowStackSize)));
  ASSERT (Stacks != NULL);
  mSmmStackArrayBase = (UINTN)Stacks;
  mSmmStackArrayEnd  = mSmmStackArrayBase + gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus * (mSmmStackSize + mSmmShadowStackSize) - 1;

  DEBUG ((DEBUG_INFO, "Stacks                   - 0x%x\n", Stacks));
  DEBUG ((DEBUG_INFO, "mSmmStackSize            - 0x%x\n", mSmmStackSize));
  DEBUG ((DEBUG_INFO, "PcdCpuSmmStackGuard      - 0x%x\n", FeaturePcdGet (PcdCpuSmmStackGuard)));
  if ((PcdGet32 (PcdControlFlowEnforcementPropertyMask) != 0) && mCetSupported) {
    DEBUG ((DEBUG_INFO, "mSmmShadowStackSize      - 0x%x\n", mSmmShadowStackSize));
  }

  // Allocate per-cpu stack for ring3
  Status = MmAllocatePages (
             AllocateAnyPages,
             EfiRuntimeServicesData,
             gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus * (EFI_SIZE_TO_PAGES (mSmmStackSize)),
             (EFI_PHYSICAL_ADDRESS *)&Cpl3Stacks
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a Failed to allocate user mode stacks - %r!!!\n", __FUNCTION__, Status));
    ASSERT_EFI_ERROR (Status);
  }

  mSmmCpl3StackArrayBase = (UINTN)Cpl3Stacks;
  mSmmCpl3StackArrayEnd  = mSmmCpl3StackArrayBase + gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus * mSmmStackSize - 1;

  //
  // Set SMI stack for SMM base relocation
  //
  PatchInstructionX86 (
    gPatchSmmInitStack,
    (UINTN)(Stacks + mSmmStackSize - sizeof (UINTN)),
    sizeof (UINTN)
    );

  //
  // Initialize IDT
  //
  InitializeSmmIdt ();

  //
  // Relocate SMM Base addresses to the ones allocated from SMRAM
  //
  mRebased = (BOOLEAN *)AllocateZeroPool (sizeof (BOOLEAN) * mMaxNumberOfCpus);
  ASSERT (mRebased != NULL);
  SmmRelocateBases ();

  //
  // Call hook for BSP to perform extra actions in normal mode after all
  // SMM base addresses have been relocated on all CPUs
  //
  SmmCpuFeaturesSmmRelocationComplete ();

  DEBUG ((DEBUG_INFO, "mXdSupported - 0x%x\n", mXdSupported));

  //
  // SMM Time initialization
  //
  InitializeSmmTimer ();

  //
  // Initialize MP globals
  //
  Cr3 = InitializeMpServiceData (Stacks, mSmmStackSize, mSmmShadowStackSize);

  if ((PcdGet32 (PcdControlFlowEnforcementPropertyMask) != 0) && mCetSupported) {
    for (Index = 0; Index < gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus; Index++) {
      SetShadowStack (
        Cr3,
        (EFI_PHYSICAL_ADDRESS)(UINTN)Stacks + mSmmStackSize + (mSmmStackSize + mSmmShadowStackSize) * Index,
        mSmmShadowStackSize
        );
      if (FeaturePcdGet (PcdCpuSmmStackGuard)) {
        SetNotPresentPage (
          Cr3,
          (EFI_PHYSICAL_ADDRESS)(UINTN)Stacks + mSmmStackSize + EFI_PAGES_TO_SIZE (1) + (mSmmStackSize + mSmmShadowStackSize) * Index,
          EFI_PAGES_TO_SIZE (1)
          );
      }
    }
  }

  //
  // Fill in SMM Reserved Regions
  //
  gSmmCpuPrivate->SmmReservedSmramRegion[0].SmramReservedStart = 0;
  gSmmCpuPrivate->SmmReservedSmramRegion[0].SmramReservedSize  = 0;

  //
  // Install the SMM Configuration Protocol onto a new handle on the handle database.
  // The entire SMM Configuration Protocol is allocated from SMRAM, so only a pointer
  // to an SMRAM address will be present in the handle database
  //
  Status = gMmCoreMmst.MmInstallProtocolInterface (
                         &gSmmCpuPrivate->SmmCpuHandle,
                         &gEfiSmmConfigurationProtocolGuid,
                         EFI_NATIVE_INTERFACE,
                         &gSmmCpuPrivate->SmmConfiguration
                         );
  ASSERT_EFI_ERROR (Status);

  // MSCHANGE [BEGIN] - Add flag to enable "test mode" for the SMM protections.
  //                    NOTE: "Test mode" will only be enabled in DEBUG builds.
  if (FeaturePcdGet (PcdSmmExceptionTestModeSupport)) {
    Status = gMmCoreMmst.MmInstallProtocolInterface (
                           &mSmmExceptionTestProtocolHandle,
                           &gSmmExceptionTestProtocolGuid,
                           EFI_NATIVE_INTERFACE,
                           &mSmmExceptionTestProtocol
                           );
    ASSERT_EFI_ERROR (Status);
  }

  // MSCHANGE [END]

  //
  // Initialize global buffer for MM MP.
  //
  InitializeDataForMmMp ();

  //
  // Initialize Package First Thread Index Info.
  //
  InitPackageFirstThreadIndexInfo ();

  //
  // Initialize SMM Profile feature
  //
  InitSmmProfile (Cr3);
  mSmmCr3 = Cr3;

  DEBUG ((DEBUG_INFO, "SMM CPU Module exit from SMRAM with EFI_SUCCESS\n"));

  return EFI_SUCCESS;
}

/**

  Find out SMRAM information including SMRR base and SMRR size.

  @param          SmrrBase          SMRR base
  @param          SmrrSize          SMRR size

**/
VOID
FindSmramInfo (
  OUT UINT32  *SmrrBase,
  OUT UINT32  *SmrrSize
  )
{
  EFI_SMRAM_DESCRIPTOR            *CurrentSmramRange;
  EFI_HOB_GUID_TYPE               *GuidHob;
  EFI_MMRAM_HOB_DESCRIPTOR_BLOCK  *HobData;
  UINTN                           Index;
  UINT64                          MaxSize;
  BOOLEAN                         Found;

  //
  // Get SMRAM hob
  //
  GuidHob = GetFirstGuidHob (&gEfiMmPeiMmramMemoryReserveGuid);
  if (GuidHob == NULL) {
    GuidHob = GetFirstGuidHob (&gEfiSmmSmramMemoryGuid);
    if (GuidHob == NULL) {
      DEBUG ((DEBUG_ERROR, "[%a] - Critical HOB missing that describes MMRAM regions. Cannot load MM.\n", __FUNCTION__));
      ASSERT (GuidHob != NULL);
      if (mSmmRebootOnException) {
        DEBUG ((DEBUG_ERROR, "%a - Specifically invoke break point exception to log telemetry.\n", __FUNCTION__));
        CpuBreakpoint ();
        ResetWarm ();
      }

      CpuDeadLoop ();
    }
  }

  //
  // Get SMRAM information
  //
  HobData = GET_GUID_HOB_DATA (GuidHob);

  mSmmCpuSmramRangeCount = HobData->NumberOfMmReservedRegions;
  mSmmCpuSmramRanges     = (EFI_SMRAM_DESCRIPTOR *)AllocatePool (mSmmCpuSmramRangeCount * sizeof (EFI_SMRAM_DESCRIPTOR));
  ASSERT (mSmmCpuSmramRanges != NULL);

  CopyMem (mSmmCpuSmramRanges, HobData->Descriptor, mSmmCpuSmramRangeCount * sizeof (EFI_SMRAM_DESCRIPTOR));

  //
  // Find the largest SMRAM range between 1MB and 4GB that is at least 256K - 4K in size
  //
  CurrentSmramRange = NULL;
  for (Index = 0, MaxSize = SIZE_256KB - EFI_PAGE_SIZE; Index < mSmmCpuSmramRangeCount; Index++) {
    //
    // Skip any SMRAM region that is already allocated, needs testing, or needs ECC initialization
    //
    if ((mSmmCpuSmramRanges[Index].RegionState & (EFI_ALLOCATED | EFI_NEEDS_TESTING | EFI_NEEDS_ECC_INITIALIZATION)) != 0) {
      continue;
    }

    if (mSmmCpuSmramRanges[Index].CpuStart >= BASE_1MB) {
      if ((mSmmCpuSmramRanges[Index].CpuStart + mSmmCpuSmramRanges[Index].PhysicalSize) <= SMRR_MAX_ADDRESS) {
        if (mSmmCpuSmramRanges[Index].PhysicalSize >= MaxSize) {
          MaxSize           = mSmmCpuSmramRanges[Index].PhysicalSize;
          CurrentSmramRange = &mSmmCpuSmramRanges[Index];
        }
      }
    }
  }

  ASSERT (CurrentSmramRange != NULL);

  *SmrrBase = (UINT32)CurrentSmramRange->CpuStart;
  *SmrrSize = (UINT32)CurrentSmramRange->PhysicalSize;

  do {
    Found = FALSE;
    for (Index = 0; Index < mSmmCpuSmramRangeCount; Index++) {
      if ((mSmmCpuSmramRanges[Index].CpuStart < *SmrrBase) &&
          (*SmrrBase == (mSmmCpuSmramRanges[Index].CpuStart + mSmmCpuSmramRanges[Index].PhysicalSize)))
      {
        *SmrrBase = (UINT32)mSmmCpuSmramRanges[Index].CpuStart;
        *SmrrSize = (UINT32)(*SmrrSize + mSmmCpuSmramRanges[Index].PhysicalSize);
        Found     = TRUE;
      } else if (((*SmrrBase + *SmrrSize) == mSmmCpuSmramRanges[Index].CpuStart) && (mSmmCpuSmramRanges[Index].PhysicalSize > 0)) {
        *SmrrSize = (UINT32)(*SmrrSize + mSmmCpuSmramRanges[Index].PhysicalSize);
        Found     = TRUE;
      }
    }
  } while (Found);

  DEBUG ((DEBUG_INFO, "SMRR Base: 0x%x, SMRR Size: 0x%x\n", *SmrrBase, *SmrrSize));
}

/**
Configure SMM Code Access Check feature on an AP.
SMM Feature Control MSR will be locked after configuration.

@param[in,out] Buffer  Pointer to private data buffer.
**/
VOID
EFIAPI
ConfigSmmCodeAccessCheckOnCurrentProcessor (
  IN OUT VOID  *Buffer
  )
{
  UINTN   CpuIndex;
  UINT64  SmmFeatureControlMsr;
  UINT64  NewSmmFeatureControlMsr;

  //
  // Retrieve the CPU Index from the context passed in
  //
  CpuIndex = *(UINTN *)Buffer;

  //
  // Get the current SMM Feature Control MSR value
  //
  SmmFeatureControlMsr = SmmCpuFeaturesGetSmmRegister (CpuIndex, SmmRegFeatureControl);

  //
  // Compute the new SMM Feature Control MSR value
  //
  NewSmmFeatureControlMsr = SmmFeatureControlMsr;
  if (mSmmCodeAccessCheckEnable) {
    NewSmmFeatureControlMsr |= SMM_CODE_CHK_EN_BIT;
    if (FeaturePcdGet (PcdCpuSmmFeatureControlMsrLock)) {
      NewSmmFeatureControlMsr |= SMM_FEATURE_CONTROL_LOCK_BIT;
    }
  }

  //
  // Only set the SMM Feature Control MSR value if the new value is different than the current value
  //
  if (NewSmmFeatureControlMsr != SmmFeatureControlMsr) {
    SmmCpuFeaturesSetSmmRegister (CpuIndex, SmmRegFeatureControl, NewSmmFeatureControlMsr);
  }

  //
  // Release the spin lock user to serialize the updates to the SMM Feature Control MSR
  //
  ReleaseSpinLock (mConfigSmmCodeAccessCheckLock);
}

/**
Configure SMM Code Access Check feature for all processors.
SMM Feature Control MSR will be locked after configuration.
**/
VOID
ConfigSmmCodeAccessCheck (
  VOID
  )
{
  UINTN       Index;
  EFI_STATUS  Status;

  //
  // Check to see if the Feature Control MSR is supported on this CPU
  //
  Index = gSmmCpuPrivate->SmmCoreEntryContext.CurrentlyExecutingCpu;
  if (!SmmCpuFeaturesIsSmmRegisterSupported (Index, SmmRegFeatureControl)) {
    mSmmCodeAccessCheckEnable = FALSE;
    return;
  }

  //
  // Check to see if the CPU supports the SMM Code Access Check feature
  // Do not access this MSR unless the CPU supports the SmmRegFeatureControl
  //
  if ((AsmReadMsr64 (EFI_MSR_SMM_MCA_CAP) & SMM_CODE_ACCESS_CHK_BIT) == 0) {
    mSmmCodeAccessCheckEnable = FALSE;
    return;
  }

  //
  // Initialize the lock used to serialize the MSR programming in BSP and all APs
  //
  InitializeSpinLock (mConfigSmmCodeAccessCheckLock);

  //
  // Acquire Config SMM Code Access Check spin lock.  The BSP will release the
  // spin lock when it is done executing ConfigSmmCodeAccessCheckOnCurrentProcessor().
  //
  AcquireSpinLock (mConfigSmmCodeAccessCheckLock);

  //
  // Enable SMM Code Access Check feature on the BSP.
  //
  ConfigSmmCodeAccessCheckOnCurrentProcessor (&Index);

  //
  // Enable SMM Code Access Check feature for the APs.
  //
  for (Index = 0; Index < gMmCoreMmst.NumberOfCpus; Index++) {
    if (Index != gSmmCpuPrivate->SmmCoreEntryContext.CurrentlyExecutingCpu) {
      if (gSmmCpuPrivate->ProcessorInfo[Index].ProcessorId == INVALID_APIC_ID) {
        //
        // If this processor does not exist
        //
        continue;
      }

      //
      // Acquire Config SMM Code Access Check spin lock.  The AP will release the
      // spin lock when it is done executing ConfigSmmCodeAccessCheckOnCurrentProcessor().
      //
      AcquireSpinLock (mConfigSmmCodeAccessCheckLock);

      //
      // Call SmmStartupThisAp() to enable SMM Code Access Check on an AP.
      //
      Status = gMmCoreMmst.MmStartupThisAp (ConfigSmmCodeAccessCheckOnCurrentProcessor, Index, &Index);
      ASSERT_EFI_ERROR (Status);

      //
      // Wait for the AP to release the Config SMM Code Access Check spin lock.
      //
      while (!AcquireSpinLockOrFail (mConfigSmmCodeAccessCheckLock)) {
        CpuPause ();
      }

      //
      // Release the Config SMM Code Access Check spin lock.
      //
      ReleaseSpinLock (mConfigSmmCodeAccessCheckLock);
    }
  }
}

// MSCHANGE [BEGIN] - Add flag to enable "test mode" for the SMM protections.
//                    NOTE: "Test mode" will only be enabled in DEBUG builds.

/**
  Enable exception handling test mode.

  NOTE: This should only work on debug builds, otherwise return EFI_UNSUPPORTED.

  @retval EFI_SUCCESS            Test mode enabled.
  @retval EFI_UNSUPPORTED        Test mode could not be enabled.

**/
EFI_STATUS
EFIAPI
// MU_CHANGE
EnableSmmExceptionTestMode (
  VOID
  )
{
  EFI_STATUS  Status = EFI_UNSUPPORTED;

  if (FeaturePcdGet (PcdSmmExceptionTestModeSupport)) {
    // MU_CHANGE START
    DEBUG ((DEBUG_INFO, "%a - Test mode enabled!\n", __FUNCTION__));
    // MU_CHANGE END
    mSmmRebootOnException = TRUE;
    Status                = EFI_SUCCESS;
  }

  return Status;
}

// MSCHANGE [END]
