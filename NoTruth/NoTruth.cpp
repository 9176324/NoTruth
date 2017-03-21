// Copyright (c) 2015-2016, tandasat. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements NoTruth functions.

#include "NoTruth.h"
#include <ntimage.h>
#define NTSTRSAFE_NO_CB_FUNCTIONS
#include <ntstrsafe.h>
#include "Wdm.h"
#include "ntddk.h"
#include "../HyperPlatform/HyperPlatform/common.h"
#include "../HyperPlatform/HyperPlatform/log.h"
#include "../HyperPlatform/HyperPlatform/util.h"
#include "../HyperPlatform/HyperPlatform/ept.h"
#include "../HyperPlatform/HyperPlatform/kernel_stl.h"
#include <array>
#include "shadow_hook.h"
#include "Ring3Hide.h"
////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//
char HiddenProcess[256][256];
////////////////////////////////////////////////////////////////////////////////
//
// types
//
extern UCHAR* testpool;
// A helper type for parsing a PoolTag value
union PoolTag {
  ULONG value;
  UCHAR chars[4];
};

// A callback type for EnumExportedSymbols()
using EnumExportedSymbolsCallbackType = bool (*)(
    ULONG index, ULONG_PTR base_address, PIMAGE_EXPORT_DIRECTORY directory,
    ULONG_PTR directory_base, ULONG_PTR directory_end, void* context);

// For SystemProcessInformation
enum SystemInformationClass {
  kSystemProcessInformation = 5,
};

// For NtQuerySystemInformation
struct SystemProcessInformation {
  ULONG next_entry_offset;
  ULONG number_of_threads;
  LARGE_INTEGER working_set_private_size;
  ULONG hard_fault_count;
  ULONG number_of_threads_high_watermark;
  ULONG64 cycle_time;
  LARGE_INTEGER create_time;
  LARGE_INTEGER user_time;
  LARGE_INTEGER kernel_time;
  UNICODE_STRING image_name;
  // omitted. see ole32!_SYSTEM_PROCESS_INFORMATION
};

typedef struct _SECTION_IMAGE_INFORMATION {
	PVOID EntryPoint;
	ULONG StackZeroBits;
	ULONG StackReserved;
	ULONG StackCommit;
	ULONG ImageSubsystem;
	SHORT SubsystemVersionLow;
	SHORT SubsystemVersionHigh;
	ULONG Unknown1;
	ULONG ImageCharacteristics;
	ULONG ImageMachineType;
	ULONG Unknown2[3];
} SECTION_IMAGE_INFORMATION, *PSECTION_IMAGE_INFORMATION;


////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C
    static void NoTruthpFreeAllocatedTrampolineRegions();

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C static NTSTATUS
    NoTruthpEnumExportedSymbols(_In_ ULONG_PTR base_address,
                               _In_ EnumExportedSymbolsCallbackType callback,
                               _In_opt_ void* context);

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C
    static bool NoTruthpEnumExportedSymbolsCallback(
        _In_ ULONG index, _In_ ULONG_PTR base_address,
        _In_ PIMAGE_EXPORT_DIRECTORY directory, _In_ ULONG_PTR directory_base,
        _In_ ULONG_PTR directory_end, _In_opt_ void* context);

static std::array<char, 5> NoTruthpTagToString(_In_ ULONG tag_value);

template <typename T>
static T NoTruthpFindOrignal(_In_ T handler);

static VOID NoTruthpHandleExQueueWorkItem(_Inout_ PWORK_QUEUE_ITEM work_item,
                                         _In_ WORK_QUEUE_TYPE queue_type);

static PVOID NoTruthpHandleExAllocatePoolWithTag(_In_ POOL_TYPE pool_type,
                                                _In_ SIZE_T number_of_bytes,
                                                _In_ ULONG tag);

static VOID NoTruthpHandleExFreePool(_Pre_notnull_ PVOID p);

static VOID NoTruthpHandleExFreePoolWithTag(_Pre_notnull_ PVOID p,
                                           _In_ ULONG tag);

static NTSTATUS NoTruthpHandleNtQuerySystemInformation(
    _In_ SystemInformationClass SystemInformationClass,
    _Inout_ PVOID SystemInformation, _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength);

#if defined(ALLOC_PRAGMA)
#pragma alloc_text(INIT, NoTruthInitialization)
#pragma alloc_text(INIT, NoTruthpEnumExportedSymbols)
#pragma alloc_text(INIT, NoTruthpEnumExportedSymbolsCallback)
#pragma alloc_text(PAGE, NoTruthTermination)
#pragma alloc_text(PAGE, NoTruthpFreeAllocatedTrampolineRegions)
#endif

typedef struct _SECURITY_ATTRIBUTES {
	DWORD  nLength;
	PVOID lpSecurityDescriptor;
	CHAR   bInheritHandle;
} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;



////////////////////////////////////////////////////////////////////////////////
//
// variables
//
SharedShadowHookData *g_share_Sh_Data;
bool isHidden = false;
// Defines where to install shadow KernelModeList and their handlers
//
// Because of simplified imlementation of NoTruth, NoTruth is unable to handle any
// of following exports properly:
//  - already unmapped exports (eg, ones on the INIT section) because it no
//    longer exists on memory
//  - exported data because setting 0xcc does not make any sense in this case
//  - functions does not comply x64 calling conventions, for example Zw*
//    functions. Because contents of stack do not hold expected values leading
//    handlers to failure of parameter analysis that may result in bug check.
//
// Also the following care should be taken:
//  - Function parameters may be an user-address space pointer and not
//    trusted. Even a kernel-address space pointer should not be trusted for
//    production level security. Vefity and capture all contents from user
//    surpplied address to VMM, then use them.
static ShadowHookTarget g_NoTruthp_hook_targets[] = {
    /*{
        RTL_CONSTANT_STRING(L"EXQUEUEWORKITEM"),
		NoTruthpHandleExQueueWorkItem,
        nullptr,
    },
    {
        RTL_CONSTANT_STRING(L"EXALLOCATEPOOLWITHTAG"),
        NoTruthpHandleExAllocatePoolWithTag, nullptr,
    },
    {
        RTL_CONSTANT_STRING(L"EXFREEPOOL"), 
		NoTruthpHandleExFreePool, 
		nullptr,
    },
    {
        RTL_CONSTANT_STRING(L"EXFREEPOOLWITHTAG"),	
        NoTruthpHandleExFreePoolWithTag, 
		nullptr,
    },*/
    {
        RTL_CONSTANT_STRING(L"NTQUERYSYSTEMINFORMATION"),
        NoTruthpHandleNtQuerySystemInformation, nullptr,
    },
};
PMDLX pagingLockProcessMemory(PVOID startAddr,
	ULONG len,
	PEPROCESS proc,
	PKAPC_STATE apcstate)
{
	PMDLX mdl = NULL;

	// Attach to process to ensure virtual addresses are correct
	//KeStackAttachProcess(proc, apcstate);

	// Create MDL to represent the image
	mdl = IoAllocateMdl(startAddr, (ULONG)len, FALSE, FALSE, NULL);

	if (mdl == NULL)
		return NULL;

	// Attempt to probe and lock the pages into memory

	MmProbeAndLockPages(mdl, UserMode, IoReadAccess);
	
	//KeUnstackDetachProcess(apcstate);

	return mdl;
}

void pagingUnlockProcessMemory(PEPROCESS proc, PKAPC_STATE apcstate, PMDLX mdl)
{
	// Attach to process to ensure virtual addresses are correct
	KeStackAttachProcess(proc, apcstate);

	// Unlock & free MDL and corresponding pages
	MmUnlockPages(mdl);
	IoFreeMdl(mdl);

	KeUnstackDetachProcess(apcstate);
}


#define TargetAppName "calc.exe"
#define TargetAppName2 "VTxRing3.exe"
PEPROCESS notepad_proc;


extern SharedShadowHookData* sharedata;

extern "C" {
	 PVOID PsGetProcessSectionBaseAddress(PEPROCESS);
	 PCHAR PsGetProcessImageFileName(PEPROCESS);
}
//--------------------------------------------------------------------------------------//
VOID HiddenStartByIOCTL(PEPROCESS proc, ULONG64 Address) {
	
	KAPC_STATE K; 
	ULONG64 cr3; 

	KeStackAttachProcess(proc, &K);
	cr3 = __readcr3(); 
	//ensure physical memory
	PMDLX mdl = pagingLockProcessMemory((PVOID)Address, PAGE_SIZE, proc, &K);
	kInitHiddenEngine(
		reinterpret_cast<SharedShadowHookData*>(sharedata), //included two list var_hide and hook_hide
		(PVOID)Address,										//Ring-3 hidden address, PE-Header
		0,													//used for callback
		"calcEproc",										//name
		true,												//var_hide/ hook_hide list 
		true,												//Is Ring3 or Ring 0 (TRUE/FALSE)?
		MmGetPhysicalAddress((PVOID)Address).QuadPart,		//Physical address used for Copy-On-Write
		cr3,
		mdl,
		proc
	);
	kStartHiddenEngine();
	KeUnstackDetachProcess(&K);
}

//--------------------------------------------------------------------------------------//
VOID ProcessMonitor(
	IN HANDLE  ParentId,
	IN HANDLE  ProcessId,
	IN BOOLEAN  Create)
{
	PVOID  PeHeaderVirt = NULL;
	USHORT numExecSections = 0;
	UCHAR* pePtr = NULL;
	PHYSICAL_ADDRESS phys = { 0 };
	char *procName;
	ULONG imageSize;
	PEPROCESS proc;
	PsLookupProcessByProcessId(ProcessId, &proc);
	procName = PsGetProcessImageFileName(proc);

	NTSTATUS status = STATUS_SUCCESS;
	HANDLE periodMeasureThreadHandle = NULL;
	OBJECT_ATTRIBUTES objectAttributes = { 0 };

	// test Ring-0 memory hidden - Debugport
	/*
	if (strncmp("notepad.exe", procName, strlen("notepad.exe")) == 0)
	{
		if (Create)
		{
			//Get EPROCESS 
			PsLookupProcessByProcessId(ProcessId, &notepad_proc);
			
			ShInstallHide(
				reinterpret_cast<SharedShadowHookData*>(sharedata),		//included two list var_hide and hook_hide
				(PVOID)notepad_proc,									//Ring-0 hidden address, EPROCESS
				0,														//used for callback
				"noteEproc",											//name
				true,													//var_hide/ hook_hide list 
				false,													//Is Ring3 or Ring 0 (TRUE/FALSE)?
				0,														//Physical address used for Copy-On-Write
				0
			);
			

			status = ShEnableHide();

			HYPERPLATFORM_LOG_INFO("Variable hidden 0x%I64X \r\n", (PVOID)notepad_proc);
			HYPERPLATFORM_LOG_INFO("NoTruth has been initialized.");
		}

	}
	*/
	// test Ring-3 memory hidden - PE Header / Any Function
	if (strncmp(TargetAppName, procName, strlen(TargetAppName)) == 0||
		strncmp(TargetAppName2, procName, strlen(TargetAppName2)) == 0)
	{
		if (Create) 
		{
			//HYPERPLATFORM_LOG_DEBUG("GetModuleBaseAddressOfProcess: %X \r\n", GetModuleBaseAddressOfProcess(proc, L"ntdll.dll"));
			/*KAPC_STATE apcstate;
			PeHeaderVirt = (PVOID)0x772A1860;// PsGetProcessSectionBaseAddress(proc);
			//ensure physical memory
			PMDLX mdl = pagingLockProcessMemory(PeHeaderVirt, PAGE_SIZE, proc, &apcstate);
											
			if (sharedata) {
				KeStackAttachProcess(proc, &apcstate);
				
				//set CR3 we need to protect~~~
				ULONG64 CR3 = __readcr3();
				
				HYPERPLATFORM_LOG_DEBUG("mdl->StartVa: 0x%I64x PeHeaderVirt: 0x%I64X pa:  0x%I64X  0x%I64X \r\n", 
					mdl->StartVa, PeHeaderVirt, (void*)UtilPaFromVa(PeHeaderVirt), UtilPaFromVa(PeHeaderVirt));

			
			
				ShInstallHide(
					reinterpret_cast<SharedShadowHookData*>(sharedata), //included two list var_hide and hook_hide
					PeHeaderVirt,										//Ring-3 hidden address, PE-Header
					0,													//used for callback
					"calcEproc",										//name
					true,												//var_hide/ hook_hide list 
					true,												//Is Ring3 or Ring 0 (TRUE/FALSE)?
					MmGetPhysicalAddress(PeHeaderVirt).QuadPart	,		//Physical address used for Copy-On-Write
					CR3,
					mdl,
					proc
				);
				
				status = ShEnableHide();
				HYPERPLATFORM_LOG_DEBUG("cr3: %I64X PeHeaderVirt : 0x%08x Val: 0x%08x  Pa: 0x%I64X ",
					CR3, PeHeaderVirt, *(PULONG)PeHeaderVirt, MmGetPhysicalAddress(PeHeaderVirt).QuadPart);


				KeUnstackDetachProcess(&apcstate);


				
			}*/
		}
		else
		{
			PMDLX mdl = GetHideMDL(reinterpret_cast<SharedShadowHookData*>(sharedata), proc);
			SetTerminateProcess(reinterpret_cast<SharedShadowHookData*>(sharedata), proc);
			kDisableHideByProcess(proc);
			if (mdl) {
				KAPC_STATE apcstate;
				pagingUnlockProcessMemory(proc, &apcstate, mdl);
			}
		}
	}
}
////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

// Initializes NoTruth
_Use_decl_annotations_ EXTERN_C NTSTATUS NoTruthInitialization(SharedShadowHookData* shared_sh_data) {
  //HYPERPLATFORM_COMMON_DBG_BREAK();

  // Get a base address of ntoskrnl
  auto nt_base = UtilPcToFileHeader(KdDebuggerEnabled);
  if (!nt_base) {
    return STATUS_UNSUCCESSFUL;
  }
  NTSTATUS status = STATUS_SUCCESS;
  // Install KernelModeList by enumerating exports of ntoskrnl, but not activate them yet
  
  /*
  auto status = NoTruthpEnumExportedSymbols(reinterpret_cast<ULONG_PTR>(nt_base),
                                           NoTruthpEnumExportedSymbolsCallback,
                                           shared_sh_data);
  if (!NT_SUCCESS(status)) {
    return status;
  }
  status = ShEnableHooks();
  
  if (!NT_SUCCESS(status)) {
    NoTruthpFreeAllocatedTrampolineRegions();
    return status;
  }
  */

  PsSetCreateProcessNotifyRoutine(ProcessMonitor, FALSE);

  
  return status;
}

// Terminates NoTruth
_Use_decl_annotations_ EXTERN_C void NoTruthTermination() {
  PAGED_CODE();

  //ShDisableHooks();
  kStopHiddenEngine();
  PsSetCreateProcessNotifyRoutine(ProcessMonitor, TRUE);

  UtilSleep(500);
  HYPERPLATFORM_LOG_INFO("NoTruth has been terminated.");
}

// Frees trampoline code allocated and stored in g_NoTruthp_hook_targets by
// NoTruthpEnumExportedSymbolsCallback()
_Use_decl_annotations_ EXTERN_C static void
NoTruthpFreeAllocatedTrampolineRegions() {
  PAGED_CODE();

  for (auto& target : g_NoTruthp_hook_targets) {
    if (target.original_call) {
      ExFreePoolWithTag(target.original_call, kHyperPlatformCommonPoolTag);
      target.original_call = nullptr;
    }
  }
}

// Enumerates all exports in a module specified by base_address.

_Use_decl_annotations_ EXTERN_C static NTSTATUS NoTruthpEnumExportedSymbols(
    ULONG_PTR base_address, EnumExportedSymbolsCallbackType callback,
    void* context) {
  PAGED_CODE();

  auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base_address);
  auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base_address + dos->e_lfanew);
  auto dir = reinterpret_cast<PIMAGE_DATA_DIRECTORY>(
      &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]);
  if (!dir->Size || !dir->VirtualAddress) {
    return STATUS_SUCCESS;
  }

  auto dir_base = base_address + dir->VirtualAddress;
  auto dir_end = base_address + dir->VirtualAddress + dir->Size - 1;
  auto exp_dir = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(base_address +
                                                           dir->VirtualAddress);
  for (auto i = 0ul; i < exp_dir->NumberOfNames; i++) {
    if (!callback(i, base_address, exp_dir, dir_base, dir_end, context)) {
      return STATUS_SUCCESS;
    }
  }
  return STATUS_SUCCESS;
}

// Checks if the export is listed as a hook target, and if so install a hook.
_Use_decl_annotations_ EXTERN_C static bool NoTruthpEnumExportedSymbolsCallback(
    ULONG index, ULONG_PTR base_address, PIMAGE_EXPORT_DIRECTORY directory,
    ULONG_PTR directory_base, ULONG_PTR directory_end, void* context) {
  PAGED_CODE(); 
  if (!context) {
    return false;
  }

  auto functions =
      reinterpret_cast<ULONG*>(base_address + directory->AddressOfFunctions);
  auto ordinals = reinterpret_cast<USHORT*>(base_address +
                                            directory->AddressOfNameOrdinals);
  auto names =
      reinterpret_cast<ULONG*>(base_address + directory->AddressOfNames);

  auto ord = ordinals[index];
  auto export_address = base_address + functions[ord];
  auto export_name = reinterpret_cast<const char*>(base_address + names[index]);

  // Check if an export is forwared one? If so, ignore it.
  if (UtilIsInBounds(export_address, directory_base, directory_end)) {
    return true;
  }

  // convert the name to UNICODE_STRING
  wchar_t name[100];
  auto status =
      RtlStringCchPrintfW(name, RTL_NUMBER_OF(name), L"%S", export_name);

  if (!NT_SUCCESS(status)) {
    return true;
  }
  UNICODE_STRING name_u = {};
  
  RtlInitUnicodeString(&name_u, name);
  //��vȫ�֔��M
  for (auto& target : g_NoTruthp_hook_targets) {
    // Is this export listed as a target
    if (!FsRtlIsNameInExpression(&target.target_name, &name_u, TRUE, nullptr)) {
      continue;
    }

    // Yes, install a hook to the export
    if (!ShInstallHide(reinterpret_cast<SharedShadowHookData*>(context),
                       reinterpret_cast<void*>(export_address), 
					   &target,
                       export_name,
					   false,
					   false,
					   0,
					   0,
					   NULL,
					   NULL)) 
	{	//modify 1
      // This is an error which should not happen
      NoTruthpFreeAllocatedTrampolineRegions();
      return false;
    }

    HYPERPLATFORM_LOG_INFO("Hook has been installed at %p %s.", export_address,
                           export_name);
  }

  return true;
}

// Converts a pool tag in integer to a printable string
_Use_decl_annotations_ static std::array<char, 5> NoTruthpTagToString(
    ULONG tag_value) {
  PoolTag tag = {tag_value};
  for (auto& c : tag.chars) {
    if (!c && isspace(c)) {
      c = ' ';
    }
    if (!isprint(c)) {
      c = '.';
    }
  }

  std::array<char, 5> str;
  auto status =
      RtlStringCchPrintfA(str.data(), str.size(), "%c%c%c%c", tag.chars[0],
                          tag.chars[1], tag.chars[2], tag.chars[3]);
  NT_VERIFY(NT_SUCCESS(status));
  return str;
}

// Finds a handler to call an original function
template <typename T>
static T NoTruthpFindOrignal(T handler) {
  for (const auto& target : g_NoTruthp_hook_targets) {
    if (target.handler == handler) {
      NT_ASSERT(target.original_call);
      return reinterpret_cast<T>(target.original_call);
    }
  }
  NT_ASSERT(false);
  return nullptr;
}

// The hook handler for ExFreePool(). Logs if ExFreePool() is called from where
// not backed by any image
_Use_decl_annotations_ static VOID NoTruthpHandleExFreePool(PVOID p) {
  const auto original = NoTruthpFindOrignal(NoTruthpHandleExFreePool);
  original(p);

  // Is inside image?
  auto return_addr = _ReturnAddress();
  if (UtilPcToFileHeader(return_addr)) {
    return;
  }

  HYPERPLATFORM_LOG_INFO_SAFE("%p: ExFreePool(P= %p)", return_addr, p);
}

// The hook handler for ExFreePoolWithTag(). Logs if ExFreePoolWithTag() is
// called from where not backed by any image.
_Use_decl_annotations_ static VOID NoTruthpHandleExFreePoolWithTag(PVOID p,
                                                                  ULONG tag) {
  const auto original = NoTruthpFindOrignal(NoTruthpHandleExFreePoolWithTag);
  original(p, tag);

  // Is inside image?
  auto return_addr = _ReturnAddress();
  if (UtilPcToFileHeader(return_addr)) {
    return;
  }

  HYPERPLATFORM_LOG_INFO_SAFE("%p: ExFreePoolWithTag(P= %p, Tag= %s)",
                              return_addr, p, NoTruthpTagToString(tag).data());
}

// The hook handler for ExQueueWorkItem(). Logs if a WorkerRoutine points to
// where not backed by any image.
_Use_decl_annotations_ static VOID NoTruthpHandleExQueueWorkItem(
    PWORK_QUEUE_ITEM work_item, WORK_QUEUE_TYPE queue_type) {
  const auto original = NoTruthpFindOrignal(NoTruthpHandleExQueueWorkItem);

  // Is inside image?
  if (UtilPcToFileHeader(work_item->WorkerRoutine)) {
    // Call an original after checking parameters. It is common that a work
    // routine frees a work_item object resulting in wrong analysis.
    original(work_item, queue_type);
    return;
  }

  auto return_addr = _ReturnAddress();
  HYPERPLATFORM_LOG_INFO_SAFE(
      "%p: ExQueueWorkItem({Routine= %p, Parameter= %p}, %d)", return_addr,
      work_item->WorkerRoutine, work_item->Parameter, queue_type);

  original(work_item, queue_type);
}

// The hook handler for ExAllocatePoolWithTag(). Logs if ExAllocatePoolWithTag()
// is called from where not backed by any image.
_Use_decl_annotations_ static PVOID NoTruthpHandleExAllocatePoolWithTag(
    POOL_TYPE pool_type, SIZE_T number_of_bytes, ULONG tag) {
  const auto original = NoTruthpFindOrignal(NoTruthpHandleExAllocatePoolWithTag);
  const auto result = original(pool_type, number_of_bytes, tag);

  // Is inside image?
  auto return_addr = _ReturnAddress();
  if (UtilPcToFileHeader(return_addr)) {
    return result;
  }

  HYPERPLATFORM_LOG_INFO_SAFE(
      "%p: ExAllocatePoolWithTag(POOL_TYPE= %08x, NumberOfBytes= %08X, Tag= "
      "%s) => %p",
      return_addr, pool_type, number_of_bytes, NoTruthpTagToString(tag).data(),
      result);
  return result;
}

// The hook handler for NtQuerySystemInformation(). Removes an entry for cmd.exe
// and hides it from being listed.
_Use_decl_annotations_ static NTSTATUS NoTruthpHandleNtQuerySystemInformation(
    SystemInformationClass system_information_class, PVOID system_information,
    ULONG system_information_length, PULONG return_length) {
  const auto original =
      NoTruthpFindOrignal(NoTruthpHandleNtQuerySystemInformation);
  const auto result = original(system_information_class, system_information,
                               system_information_length, return_length);
  if (!NT_SUCCESS(result)) {
    return result;
  }
  if (system_information_class != kSystemProcessInformation) {
    return result;
  }

  auto next = reinterpret_cast<SystemProcessInformation*>(system_information);
  while (next->next_entry_offset) {
    auto curr = next;
    next = reinterpret_cast<SystemProcessInformation*>(
        reinterpret_cast<UCHAR*>(curr) + curr->next_entry_offset);
    if (_wcsnicmp(next->image_name.Buffer, L"cmd.exe", 7) == 0) {
      if (next->next_entry_offset) {
        curr->next_entry_offset += next->next_entry_offset;
      } else {
        curr->next_entry_offset = 0;
      }
      next = curr;
    }
  }
  return result;
}