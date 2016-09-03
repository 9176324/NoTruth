// Copyright (c) 2015-2016, tandasat. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements VMM initialization functions.

#include "vm.h"
#include <intrin.h>
#include "asm.h"
#include "common.h"
#include "ept.h"
#include "log.h"
#include "util.h"
#include "vmm.h"
#include "../../DdiMon/ddi_mon.h"
#include "../../DdiMon/shadow_hook.h"

extern "C" {
////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

////////////////////////////////////////////////////////////////////////////////
//
// types
//

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

_IRQL_requires_max_(PASSIVE_LEVEL) static bool VmpIsVmxAvailable();

_IRQL_requires_(DISPATCH_LEVEL) static NTSTATUS
    VmpSetLockBitCallback(_In_opt_ void *context);

_IRQL_requires_max_(PASSIVE_LEVEL) static SharedProcessorData *VmpInitializeSharedData();

_IRQL_requires_(DISPATCH_LEVEL) static NTSTATUS VmpStartVM(_In_opt_ void *context);

static void VmpInitializeVm(_In_ ULONG_PTR guest_stack_pointer,
                            _In_ ULONG_PTR guest_instruction_pointer,
                            _In_opt_ void *context);

static bool VmpEnterVmxMode(_Inout_ ProcessorData *processor_data);

static bool VmpInitializeVMCS(_Inout_ ProcessorData *processor_data);

static bool VmpSetupVMCS(_In_ const ProcessorData *processor_data,
                         _In_ ULONG_PTR guest_stack_pointer,
                         _In_ ULONG_PTR guest_instruction_pointer,
                         _In_ ULONG_PTR vmm_stack_pointer);

static void VmpLaunchVM();

static ULONG VmpGetSegmentAccessRight(_In_ USHORT segment_selector);

static SegmentDesctiptor *VmpGetSegmentDescriptor(
    _In_ ULONG_PTR descriptor_table_base, _In_ USHORT segment_selector);

static ULONG_PTR VmpGetSegmentBaseByDescriptor(
    _In_ const SegmentDesctiptor *segment_descriptor);

static ULONG_PTR VmpGetSegmentBase(_In_ ULONG_PTR gdt_base,
                                   _In_ USHORT segment_selector);

static ULONG VmpAdjustControlValue(_In_ Msr msr, _In_ ULONG requested_value);

static NTSTATUS VmpStopVM(_In_opt_ void *context);

static KSTART_ROUTINE VmpVmxOffThreadRoutine;

static void VmpFreeProcessorData(_In_opt_ ProcessorData *processor_data);

static bool VmpIsVmmInstalled();

#if defined(ALLOC_PRAGMA)
#pragma alloc_text(INIT, VmInitialization)
#pragma alloc_text(INIT, VmpIsVmxAvailable)
#pragma alloc_text(INIT, VmpSetLockBitCallback)
#pragma alloc_text(INIT, VmpInitializeSharedData)
#pragma alloc_text(INIT, VmpStartVM)
#pragma alloc_text(INIT, VmpInitializeVm)
#pragma alloc_text(INIT, VmpEnterVmxMode)
#pragma alloc_text(INIT, VmpInitializeVMCS)
#pragma alloc_text(INIT, VmpSetupVMCS)
#pragma alloc_text(INIT, VmpLaunchVM)
#pragma alloc_text(INIT, VmpGetSegmentAccessRight)
#pragma alloc_text(INIT, VmpGetSegmentBase)
#pragma alloc_text(INIT, VmpGetSegmentDescriptor)
#pragma alloc_text(INIT, VmpGetSegmentBaseByDescriptor)
#pragma alloc_text(INIT, VmpAdjustControlValue)
#pragma alloc_text(PAGE, VmTermination)
#pragma alloc_text(PAGE, VmpVmxOffThreadRoutine)
#endif

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

// Define GetSegmentLimit if it is not defined yet (it is only defined on x64)
#if !defined(GetSegmentLimit)
inline ULONG GetSegmentLimit(_In_ ULONG selector) {
  return __segmentlimit(selector);
}
#endif

// Checks if a VMM can be installed, and so, installs it

//��ʼ������VM�C��
// 1. �z�y�Ƿ��ѽ����bVM
// 2. �z�y�C���Ƿ�֧��VT
// 3. ��ʼ��HOOKDATA,���Զ��x��һ��MSR
// 4. ��vÿһ������, ����vmcall���{ ʹ�M��VMM
// 5. �M��VMM��
SharedShadowHookData* sharedata;
_Use_decl_annotations_ NTSTATUS VmInitialization() {
  //��挦�R
  PAGED_CODE();

  //�Д��Ƿ��ѽ����bvm
  if (VmpIsVmmInstalled()) {
    return STATUS_CANCELLED;
  }
  //ͨ?CPUID�Д��Ƿ�VMX����
  if (!VmpIsVmxAvailable()) {
    return STATUS_HV_FEATURE_UNAVAILABLE;
  }
  //��ʼ��MSR-Bitmap�K����һ�ݿյ�HOOKDATA���M
  //Prepared a MST-Bitmap and EMPTY HOOKDATA data array

  const auto shared_data = VmpInitializeSharedData();
  if (!shared_data) {
    return STATUS_MEMORY_NOT_ALLOCATED;
  }

  // Virtualize all processors
 
  // ͸�^DPC, �ְl̓�M�����{
  // ̓�M���^�̴�����������:
  
  // ��춮�ǰCPU (���{��:vmpstartVM)
  // 1. ����ProcessorData
  // 2. ProcessorData->ept_data << ����EPT퓱�
  // 3. ProcessorData->sh_data  << ���估��ʼ��ShadowHookData , ��������̎���һ�Δ���
  // 4. ����vmm�õė�
  // 5. �ķ��䵽�ĵ�ַ,���ϴ�С = ����ʼ��ַ (��闣�����°lչ)
  // 6. ����ProcessorDataָ�
  // 7. �ى���MAXULONG_PTR
  // 8. ������ǿ��õ���������ַ�����g
  // 9. Processor_data->shared_data << shared_data ����������
  //10. ����VMX-Region �� VMCS, �����ľS�o�Y��ͬһ?->�Ķ���ʼ�����vmcs������-> ���O��VMEXIT���{���� -> ���к���VmmVmExitHandler�����, �ְlexitԭ��
  //11. ����-> vmcs�O���� -> �ÅR��VMLAUNCHָ��, ����VM

  //
  //���: ÿһ��CPU�����Լ�һ�ݔ���.....
  //���а���: 
  //1. cpu�Լ��ė����g��С��..
  //2. ept퓱�(processor_data->ept_data)							 
  //3. ����һ��̎���EPT_Violation����(processor_data->sh_data)   ; EPT_Violation�r����, MTF�r�֏�
  //4. ���õ�hook code/hide data���M(shared_data->shared_sh_data ; ���EPT_violation�r
  auto status = UtilForEachProcessor(VmpStartVM, shared_data);
  if (!NT_SUCCESS(status)) {
    UtilForEachProcessor(VmpStopVM, nullptr);
    return status;
  }
  sharedata = reinterpret_cast<SharedShadowHookData*>(shared_data->shared_sh_data);
  status = DdimonInitialization(shared_data->shared_sh_data);
  if (!NT_SUCCESS(status)) {
    UtilForEachProcessor(VmpStopVM, nullptr);
    return status;
  }
  return status;
}

// Checks if the system supports virtualization
_Use_decl_annotations_ static bool VmpIsVmxAvailable() {
  PAGED_CODE();

  // See: DISCOVERING SUPPORT FOR VMX
  // If CPUID.1:ECX.VMX[bit 5]=1, then VMX operation is supported.
  int cpu_info[4] = {};
  __cpuid(cpu_info, 1);
  const CpuFeaturesEcx cpu_features = {static_cast<ULONG_PTR>(cpu_info[2])};
  if (!cpu_features.fields.vmx) {
    HYPERPLATFORM_LOG_ERROR("VMX features are not supported.");
    return false;
  }

  // See: BASIC VMX INFORMATION
  // The first processors to support VMX operation use the write-back type.
  const Ia32VmxBasicMsr vmx_basic_msr = {UtilReadMsr64(Msr::kIa32VmxBasic)};
  if (static_cast<memory_type>(vmx_basic_msr.fields.memory_type) !=
      memory_type::kWriteBack) {
    HYPERPLATFORM_LOG_ERROR("Write-back cache type is not supported.");
    return false;
  }

  // See: ENABLING AND ENTERING VMX OPERATION
  Ia32FeatureControlMsr vmx_feature_control = {
      UtilReadMsr64(Msr::kIa32FeatureControl)};
  if (!vmx_feature_control.fields.lock) {
    HYPERPLATFORM_LOG_INFO("The lock bit is clear. Attempting to set 1.");
    const auto status = UtilForEachProcessor(VmpSetLockBitCallback, nullptr);
    if (!NT_SUCCESS(status)) {
      return false;
    }
  }
  if (!vmx_feature_control.fields.enable_vmxon) {
    HYPERPLATFORM_LOG_ERROR("VMX features are not enabled.");
    return false;
  }

  if (!EptIsEptAvailable()) {
    HYPERPLATFORM_LOG_ERROR("EPT features are not fully supported.");
    return false;
  }
  return true;
}

// Sets 1 to the lock bit of the IA32_FEATURE_CONTROL MSR
_Use_decl_annotations_ static NTSTATUS VmpSetLockBitCallback(void *context) {
  UNREFERENCED_PARAMETER(context);

  Ia32FeatureControlMsr vmx_feature_control = {
      UtilReadMsr64(Msr::kIa32FeatureControl)};
  if (vmx_feature_control.fields.lock) {
    return STATUS_SUCCESS;
  }
  vmx_feature_control.fields.lock = true;
  UtilWriteMsr64(Msr::kIa32FeatureControl, vmx_feature_control.all);
  vmx_feature_control.all = UtilReadMsr64(Msr::kIa32FeatureControl);
  if (!vmx_feature_control.fields.lock) {
    HYPERPLATFORM_LOG_ERROR("The lock bit is still clear.");
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }
  return STATUS_SUCCESS;
}

// Initialize shared processor data
// ��ʼ�����픵�� 
// 1. ��ʼ��MSR?�D
// 2. ��ʼ��Hook data���M
_Use_decl_annotations_ static SharedProcessorData *VmpInitializeSharedData() 
{
  PAGED_CODE();

  //����Ƿ�?�ȴ� shared_data ����hook����
  const auto shared_data = reinterpret_cast<SharedProcessorData *>(ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(SharedProcessorData),
                            kHyperPlatformCommonPoolTag));
  if (!shared_data) {
    return nullptr;
  }

  RtlZeroMemory(shared_data, sizeof(SharedProcessorData));
  HYPERPLATFORM_LOG_DEBUG("SharedData=        %p", shared_data);

  // Set up the MSR bitmap

  //�Զ��xMSRλ�D
  const auto msr_bitmap = ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE,
                                                kHyperPlatformCommonPoolTag);
  if (!msr_bitmap) {
    ExFreePoolWithTag(shared_data, kHyperPlatformCommonPoolTag);
    return nullptr;
  }
  RtlZeroMemory(msr_bitmap, PAGE_SIZE);

  //hook msr
  shared_data->msr_bitmap = msr_bitmap;

  // Checks MSRs causing #GP and should not cause VM-exit from 0 to 0xfff.
  //�z�y?�õ�MSR����l��#GP����
  bool unsafe_msr_map[0x1000] = {};
  for (auto msr = 0ul; msr < RTL_NUMBER_OF(unsafe_msr_map); ++msr) {
    __try {
      UtilReadMsr(static_cast<Msr>(msr));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      unsafe_msr_map[msr] = true;
    }
  }

  // Activate VM-exit for RDMSR against all MSRs
  //bitmap �ߵ�λ
  const auto bitmap_read_low = reinterpret_cast<UCHAR *>(msr_bitmap);
  const auto bitmap_read_high = bitmap_read_low + 1024;	//��һ?��С

  //��0xFF��ʼ��
  RtlFillMemory(bitmap_read_low, 1024, 0xff);   // read        0 -     1fff
  RtlFillMemory(bitmap_read_high, 1024, 0xff);  // read c0000000 - c0001fff

  // But ignore IA32_MPERF (000000e7) and IA32_APERF (000000e8)
  // ��ʼ��λ�D
  RTL_BITMAP bitmap_read_low_header = {};
  RtlInitializeBitMap(&bitmap_read_low_header, reinterpret_cast<PULONG>(bitmap_read_low), 1024 * 8);
  RtlClearBits(&bitmap_read_low_header, 0xe7, 2);

  // Also ignore the unsage MSRs
  for (auto msr = 0ul; msr < RTL_NUMBER_OF(unsafe_msr_map); ++msr) {
    const auto ignore = unsafe_msr_map[msr];
    if (ignore) {
      RtlClearBits(&bitmap_read_low_header, msr, 1);
    }
  }

  // But ignore IA32_GS_BASE (c0000101) and IA32_KERNEL_GS_BASE (c0000102)
  RTL_BITMAP bitmap_read_high_header = {};
  RtlInitializeBitMap(&bitmap_read_high_header,
                      reinterpret_cast<PULONG>(bitmap_read_high), 1024 * 8);
  RtlClearBits(&bitmap_read_high_header, 0x101, 2);

  // Set up shared shadow hook data
  // �����Ӕ��M
  shared_data->shared_sh_data = ShAllocateSharedShaowHookData();
  if (!shared_data->shared_sh_data) {
    ExFreePoolWithTag(msr_bitmap, kHyperPlatformCommonPoolTag);
    ExFreePoolWithTag(shared_data, kHyperPlatformCommonPoolTag);
    return nullptr;
  }
  return shared_data;
}

// Virtualize the current processor
_Use_decl_annotations_ static NTSTATUS VmpStartVM(void *context) 
{
  HYPERPLATFORM_LOG_INFO("Initializing VMX for the processor %d.",
                         KeGetCurrentProcessorNumberEx(nullptr));

  // ��춮�ǰCPU
  // 1. ����ProcessorData
  // 2. ProcessorData->ept_data << ����EPT퓱�
  // 3. ProcessorData->sh_data  << ���估��ʼ��ShadowHookData ��乴��Ԕ���Y��
  // 4. ����vmm�õė�
  // 5. �ķ��䵽�ĵ�ַ,���ϴ�С = ��?���ַ (��闣�����°lչ)
  // 6. ����ProcessorDataָ�
  // 7. �ى���MAXULONG_PTR
  // 8. ������ǿ��õ���������ַ�����g
  // 9. Processor_data->shared_data << shared_data ����������
  //10. ����VMX-Region �� VMCS, �����ľS?�Y��ͬһ?->?����ʼ�����vmcs��?��-> ��?��VMEXIT��?���� -> ���к���VmmVmExitHandler�����, �ְlexitԭ��
  //11. ����-> vmcs?���� -> ?�ÅR��VMLAUNCHָ��, ����VM
  const auto ok = AsmInitializeVm(VmpInitializeVm, context);

  NT_ASSERT(VmpIsVmmInstalled() == ok);

  if (!ok) {
    return STATUS_UNSUCCESSFUL;
  }

  HYPERPLATFORM_LOG_INFO("Initialized successfully.");

  return STATUS_SUCCESS;
}

// Allocates structures for virtualization, initializes VMCS and virtualizes
// the current processor
// ��춮�ǰCPU
// 1. ����ProcessorData
// 2. ProcessorData->ept_data << ����EPT?��
// 3. ProcessorData->sh_data  << ���估��ʼ��ShadowHookData ??����?��?��
// 4. ����vmm�õė�
// 5. �ķ��䵽�ĵ�ַ,���ϴ�С = ��?���ַ (��闣�����°lչ)
// 6. ����ProcessorDataָ?
// 7. �ى���MAXULONG_PTR
// 8. ������ǿ��õ���������ַ����?
// 9. Processor_data->shared_data << shared_data ����������
//10. ����VMX-Region �� VMCS, �����ľS?�Y��ͬһ?->?����ʼ�����vmcs��?��-> ��?��VMEXIT��?���� -> ���к���VmmVmExitHandler�����, �ְlexitԭ��
//11. ����-> vmcs?���� -> ?�ÅR��VMLAUNCHָ��, ����VM
_Use_decl_annotations_ static void VmpInitializeVm(
    ULONG_PTR guest_stack_pointer,		//����VM�r�ė�ָ?
	ULONG_PTR guest_instruction_pointer,//����VM�r��RIP/EIP
    void *context						//?��SharedProcessorData������(������CPU���픵��, MSRλ�D,�Լ��ȴ�?�ص�HOOK�Y����)
) {					

  //�Ѯ�ǰCPUʹ�õĔ��� �����������x..
  const auto shared_data = reinterpret_cast<SharedProcessorData *>(context);
  if (!shared_data) {
    return;
  }

  // Allocate related structures
  // ���܁K��������vmxҪ�õĔ���
  const auto processor_data = reinterpret_cast<ProcessorData *>(ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(ProcessorData), kHyperPlatformCommonPoolTag));
  
  if (!processor_data) {
    return;
  }
  RtlZeroMemory(processor_data, sizeof(ProcessorData));

  // Set up EPT
  // ����EPT?��,�K???�m�ăȴ�K��EPT?�� ӳ�䵽EPT?��(������һ��, ��: ����ֵ��10 , �t���ŵ�10), ��ÿ����һ?PML4 �������·�����?3�� ��512*512*512?
  // �Ķ�����EPT?��(����?�m�ȴ�, LAPICӳ��ȴ�, Pre-alloc��ept��?(?�г�ʼ��))   
  processor_data->ept_data = EptInitialization();
  if (!processor_data->ept_data) {
    goto ReturnFalse;
  }

  //?��CPU��shadow hook , ���估��ʼ����0
  processor_data->sh_data = ShAllocateShadowHookData();
  if (!processor_data->sh_data) {
    goto ReturnFalse;
  }
  //����һ�K24kb , 6000�ֹ���?�m��?����vmm�ė���? �K���ػ�ַ (ע��: �������ϰlչ, ���ԗ���base����Ҫ����size�ŵó�����ַ ������ֱ��ʹ��)
  const auto vmm_stack_limit = UtilAllocateContiguousMemory(KERNEL_STACK_SIZE);
  //����vmcs?��
  const auto vmcs_region =
      reinterpret_cast<VmControlStructure *>(ExAllocatePoolWithTag(
          NonPagedPoolNx, kVmxMaxVmcsSize, kHyperPlatformCommonPoolTag));
  //����vmxon?��
  const auto vmxon_region =
      reinterpret_cast<VmControlStructure *>(ExAllocatePoolWithTag(
          NonPagedPoolNx, kVmxMaxVmcsSize, kHyperPlatformCommonPoolTag));

  // Initialize the management structure
  processor_data->vmm_stack_limit = vmm_stack_limit;
  processor_data->vmcs_region = vmcs_region;
  processor_data->vmxon_region = vmxon_region;

  if (!vmm_stack_limit || !vmcs_region || !vmxon_region) {
    goto ReturnFalse;
  }
  RtlZeroMemory(vmm_stack_limit, KERNEL_STACK_SIZE);
  RtlZeroMemory(vmcs_region, kVmxMaxVmcsSize);
  RtlZeroMemory(vmxon_region, kVmxMaxVmcsSize);

  // Initialize stack memory for VMM like this:
  //
  // (High)
  // +------------------+  <- vmm_stack_region_base      (eg, AED37000)
  // | processor_data   |
  // +------------------+  <- vmm_stack_data             (eg, AED36FFC)
  // | MAXULONG_PTR     |
  // +------------------+  <- vmm_stack_base (initial SP)(eg, AED36FF8)
  // |                  |    v
  // | (VMM Stack)      |    v (grow)
  // |                  |    v
  // +------------------+  <- vmm_stack_limit            (eg, AED34000)
  // (Low)

  //��ַ+����С�õ�����ߵ�ַ
  const auto vmm_stack_region_base = reinterpret_cast<ULONG_PTR>(vmm_stack_limit) + KERNEL_STACK_SIZE;
  //���ψD, ����ֱ�ӌ��뗣��? 
  const auto vmm_stack_data = vmm_stack_region_base - sizeof(void *);
  //���ψD, ����ֱ�ӌ��뗣��?
  const auto vmm_stack_base = vmm_stack_data - sizeof(void *);

  //?��log
  HYPERPLATFORM_LOG_DEBUG("VmmStackTop=       %p", vmm_stack_limit);
  HYPERPLATFORM_LOG_DEBUG("VmmStackBottom=    %p", vmm_stack_region_base);
  HYPERPLATFORM_LOG_DEBUG("VmmStackData=      %p", vmm_stack_data);
  HYPERPLATFORM_LOG_DEBUG("ProcessorData=     %p stored at %p", processor_data,vmm_stack_data);
  HYPERPLATFORM_LOG_DEBUG("VmmStackBase=      %p", vmm_stack_base);
  HYPERPLATFORM_LOG_DEBUG("GuestStackPointer= %p", guest_stack_pointer);
  HYPERPLATFORM_LOG_DEBUG("GuestInstPointer=  %p", guest_instruction_pointer);

  //��D?�� ֱ�ӌ��뗣��?
  *reinterpret_cast<ULONG_PTR *>(vmm_stack_base) = MAXULONG_PTR;
  //��D?�� ֱ�ӌ��뗣��?
  *reinterpret_cast<ProcessorData **>(vmm_stack_data) = processor_data;

  //CPU���픵��, ���а������픵����CPU����, �Լ�msrλ�D, �ȴ�?�ص�HOOK
  processor_data->shared_data = shared_data;

  InterlockedIncrement(&processor_data->shared_data->reference_count);

  // Set up VMCS 

  // ��ʼ��VMXON_REGION :
  // 1. ?��VMCS�汾
  // 2. ?�ÅR������,?��VMXON
  if (!VmpEnterVmxMode(processor_data)) {
    goto ReturnFalse;
  }
  //��ʼ��VMCS_REGION
  if (!VmpInitializeVMCS(processor_data)) {
    goto ReturnFalseWithVmxOff;
  }
  //��ʼ��vmcs
  if (!VmpSetupVMCS(processor_data, guest_stack_pointer,
                    guest_instruction_pointer, vmm_stack_base)) {
    goto ReturnFalseWithVmxOff;
  }

  // Do virtualize the processor
  //����vm
  VmpLaunchVM();

// Here is not be executed with successful vmlaunch. Instead, the context
// jumps to an address specified by guest_instruction_pointer.

ReturnFalseWithVmxOff:;
  __vmx_off();

ReturnFalse:;
  VmpFreeProcessorData(processor_data);
}

// See: VMM SETUP & TEAR DOWN
//?��vmxģʽ, ������Process����
/*
struct ProcessorData {
	SharedProcessorData* shared_data;         ///< Shared data
	void* vmm_stack_limit;                    ///< A head of VA for VMM stack
	struct VmControlStructure* vmxon_region;  ///< VA of a VMXON region
	struct VmControlStructure* vmcs_region;   ///< VA of a VMCS region
	struct EptData* ept_data;                 ///< A pointer to EPT related data
	struct ShadowHookData* sh_data;           ///< Per-processor shadow hook data
};
*/

//��Ҫ�������?��VMXON_REGION?��, Ȼ�ᆢ��CPU VMXģʽ
_Use_decl_annotations_ static bool VmpEnterVmxMode(ProcessorData *processor_data) {
  // Apply FIXED bits

  // ��cr0�Ĵ���ĳЩλ����α�����...(��Щ��0, ��Щ��1) ��cpu����, ??��MSR�Ĵ���
  const Cr0 cr0_fixed0 = {UtilReadMsr(Msr::kIa32VmxCr0Fixed0)};	//?ȡcr0��?Ҫ������0��λ
  const Cr0 cr0_fixed1 = {UtilReadMsr(Msr::kIa32VmxCr0Fixed1)}; //?ȡcr0��?Ҫ������1��λ
  Cr0 cr0 = {__readcr0()};										//?ȡcr0�Ĵ���
	
  cr0.all &= cr0_fixed1.all;		//?��
  cr0.all |= cr0_fixed0.all;		//?��

  __writecr0(cr0.all);				//���������õ�cr0

  //ͬ��
  const Cr4 cr4_fixed0 = {UtilReadMsr(Msr::kIa32VmxCr4Fixed0)};
  const Cr4 cr4_fixed1 = {UtilReadMsr(Msr::kIa32VmxCr4Fixed1)};
  Cr4 cr4 = {__readcr4()};
  cr4.all &= cr4_fixed1.all;
  cr4.all |= cr4_fixed0.all;
  __writecr4(cr4.all);				//���������õ�cr4

  // Write a VMCS revision identifier
  // ?ȡVMCS�汾?
  const Ia32VmxBasicMsr vmx_basic_msr = {UtilReadMsr64(Msr::kIa32VmxBasic)};
  // ����VMCS�汾?��VMCS
  processor_data->vmxon_region->revision_identifier = vmx_basic_msr.fields.revision_identifier;
  
  //?ȡ����?��������ַ
  auto vmxon_region_pa = UtilPaFromVa(processor_data->vmxon_region);
  
  //����, ��VMCS����
  /*  �����ֲ�:
   *  ����TVMXON�r ��Ҫ�Լ�����һ?VMXON_REGION (ÿһ?����(???����)��?Ҫ�Ќ���һ�ݵ�VMX_REGION)
   *  ��???��������ַ����,��������
   */
  if (__vmx_on(&vmxon_region_pa)) {
    return false;
  }
  //?������EPT��oЧ
  UtilInveptAll();
  return true;
}

// See: VMM SETUP & TEAR DOWN
// ��ʼ��VMCS
_Use_decl_annotations_ static bool VmpInitializeVMCS(ProcessorData *processor_data) {
  // Write a VMCS revision identifier

  const Ia32VmxBasicMsr vmx_basic_msr = {UtilReadMsr64(Msr::kIa32VmxBasic)};

  //?��VMCS��ʽ�İ汾?
  processor_data->vmcs_region->revision_identifier =
      vmx_basic_msr.fields.revision_identifier;

  //?ȡVMCS�����ַ
  auto vmcs_region_pa = UtilPaFromVa(processor_data->vmcs_region);

  //�����ǰVMCS�Ľ���
  if (__vmx_vmclear(&vmcs_region_pa)) {
    return false;
  }
  //����VMCS����ǰCPU
  if (__vmx_vmptrld(&vmcs_region_pa)) {
    return false;
  }

  // The launch state of current VMCS is "clear"
  // VM-ENTRY�r�Ġ�BҪ��
  // VMLAUCH -> clear	
  // VMRESUME-> launched 
  // ���ؕrVMCS�ѽ�������ǰCPU, ��B��CLEAR

  return true;
}

// See: PREPARATION AND LAUNCHING A VIRTUAL MACHINE
_Use_decl_annotations_ static bool VmpSetupVMCS(
    const ProcessorData *processor_data, 
	ULONG_PTR guest_stack_pointer,
    ULONG_PTR guest_instruction_pointer, 
	ULONG_PTR vmm_stack_pointer) 
{
  //����gdtr
  Gdtr gdtr = {};
  __sgdt(&gdtr);
  
  //����idtr
  Idtr idtr = {};
  __sidt(&idtr);

  // See: Algorithms for Determining VMX Capabilities

  const auto use_true_msrs = Ia32VmxBasicMsr{UtilReadMsr64(Msr::kIa32VmxBasic)}.fields.vmx_capability_hint;
  
  //�������һ��ѵ�vmcs�Y��
  VmxVmEntryControls vm_entryctl_requested = {};
  //�Ƿ�֧��64λ
  vm_entryctl_requested.fields.ia32e_mode_guest = IsX64();

  /* VMCS�Y��:
  *  1. Guest-state area:						//�͑��C��B��(��vmware) , ?��VMM�r���� ??VMM�r�֏�
		 Cr0, Cr3, Cr4
		 Dr7
		 Rsp, Rip �򌦑���32λ�Ĵ���
		 ���ж�?����(����16λ?����?,�λ�ַ,?������,�δ�С)
		 GDTR,LDTR
		 ����MSR:
			 IA32_DEBUGCTL
			 IA32_SYSENTER_CS
			 IA32_SYSENTER_ESP & EIP
			 IA32_PERF_GLOBAL_CTRL
			 IA32_PAT
			 IA32_EFER
		SMBASE�Ĵ���	

		Activity State(32bit)				//CPU��Ӡ�B		
			0: Active						//�����
			1: HLT							//���ڈ���HLTָ��
			2: ShutDown						//���3��??,����?�C			
			3: Wait-for-SIPI					//�ȴ���?�MCPU,�l�͆���Startup-IPI

		Interruptibility State(32bit)		//���Д��Ԡ�B
			bit[0]: Blocking by STI			//��ʾSTI����Ŀǰ��Ч��			
			bit[1]: Blocking by mov SS		//��ʾMOV SS����Ŀǰ��Ч��
			bit[2]: Blocking by SMI			//��ʾSMI����Ŀǰ��Ч��
			bit[3]: Blocking by NMI			//��ʾNMI����Ŀǰ��Ч��
			bit[31:4]: 0					//����λ, �����??

		Pending debug Exceptions(64/32bit)	
			bit[3:0]:  B3-B0					// ÿһλ��ʾ�����Ĕ�?��B, DR7?��?�Þ������VMM��B
			bit[11:4]: ����λ				// ����, ����tVM entryʧ��
			bit[12]:   enabled bp			// ��ʾ��С��һ?���?������?��I/O��? ���� �K��������DR7����
			bit[14]:   bs					// ��ʾ???������?�l�β�����
			bit[15]:   ����λ				// ����, ����tVM entryʧ��
			bit[16]:   RTM					// ��ʾ??�¼��l����RTM?��
			bit[63:17]: ����λ				// ����, ����tVM entryʧ��

		VMCS Link Pointer(64bit)			// ���VMCS Shadow = 1�r��Ч, ��?��vmcs�t��??VMCS��?��, ��t��ԭ��VMCS?��(������?)
											// ���Õrȫ?�Þ�1

		vmx-preemption timer value(32bit)			// activate VMX-preemption timer = 1	 ��Ч
													// ?�Ó�ռʽ?�r����ֵ

		Page-directory-pointer-table entry(64bit)	// Enable EPT = 1 �r��Ч 
													// PDPTE?���X86 ?��?

		Guest Interrupt status(16bit)				// virtual-interrupt delivery = 1�r��Ч
			Request virtual interrupt				// λ8λ, 
			Servcing virtual interrupt				// ��8λ

		PML Index(16bit)							// Enable PML = 1   �r��Ч , PML������ / PML address VM-exec. ��ַ ,  ����������0~511 , 
													// ������Ч�r, ͬ�r?��EPTP[6]?�Þ�1, ��??λ��1 �t:������?�����Ɍ���,�K?��dirty bit
													// ����?���?������?����?(bit[8], ?���r?��) �� dirty bit (bit[9], ����r?��)
													// ����EPT PML4�����ַ(4�ֹ���?) ?��?Ŀ?
													// Ia32VmxEptVpidCapMsr ����֪���Ƿ�֧��PML

  * 2. Host-state area							//�����C��B��(����C��),  ??VMM�r���� ?��VMM�r�֏�
		ֻ��Guest-state area�ļĴ�����

  * 3. VM-execution control fields			// ??vm��?��
	3.1	Pin-based VM-execution control:		// Ҫ�鿴MSR����λ���?��
			External-Interrupt exiting		// �Ƿ�?�ⲿ�Д�
			NMI Exiting						// �Ƿ�?nmi�Д�
			virtual NMI						// �Ƿ�??�MNMI�Д�
			Activate VMX-Preemption Timer	// ���ռʽ?�r��
			Process posted interrupts		// 

	3.2	Processor-based VM-execution control //�֞���Ҫ�ֶ� �� ��Ҫ�ֶ�	
	 3.2.1	Primary Process-based VM-exec. control(32bit):		//��Ҫ�ֶ�

			 bit[2]:  Interrupt-Window				//����ָ��RFLAGS.IF = 1 �Լ�?�������Д��t����VMM
			 bit[3]:  Use TSC offseting				//MSR�r?�Ĵ�����?
			 bit[7]:  HLT exiting					//����HLTָ��r , �Ƿ�l��VMEXIT(����VMM)
			 bit[9]:  INVLPG exiting				//ͬ��
			 bit[10]: MWAIT exiting					//ͬ��
			 bit[11]: RDPMC exiting					//ͬ��
			 bit[12]: RDTSX exitng					//ͬ��
			 bit[15]: CR-3 loading					//����CR3��ֵ,�Ƿ�l��VMEXIT
			 bit[16]: CR-3 store						//?ȡCR3��ֵ,�Ƿ�l��VMEXIT
			 bit[19]: CR-8 loading					//ͬ��
			 bit[20]: CR-8 loading					//ͬ��
			 bit[21]: Use TRP shadow				//�Ƿ�ʹ��TRP?�M��/ APIC?�M��
			 bit[22]: NMI-Window exiting				//?��NMI���Εr, �κ�ָ��a��VMEXIT
			 bit[23]: MOV DR exiting				//����mov dr ָ�� �Ƿ�l��VMEXIT
			 bit[24]: Unconditional I/O				//�o�l��I/O, �Ƿ��ڈ�������I/Oָ��r�l��VMEXIT
			 bit[25]: Use I/O bitmap				//I/Oλ�D , ��ʹ��I/O bitmap, �t���ԟo�l��I/O
			 bit[27]: Monitor trap flag				//�Ƿ�O?�β�����
			 bit[28]: Use MSR bitmaps				//�Ƿ�ʹ��MSR�Ĵ���λ�D �����RDMSR��WRMSRָ��
			 bit[29]: MONITOR exiting				//����MONITOR �Ƿ�VMEXIT
			 bit[30]: PAUSE exiting					//����PAUSE �Ƿ�VMEXIT
			 bit[31]: Activate Secondary Control	//�Ƿ�ʹ�ô�Ҫ�ֶ�(����ept���ܵı�)

	 3.2.2	Secondary Process-based VM-exec. control(32bit): //��Ҫ�ֶ�
			 bit[0]: Virtual APIC access			//APIC?�M����?
			 bit[1]: Enable EPT						//�Ƿ���EPT?��
			 bit[2]: Descriptor table exiting		//���������������r �Ƿ�a��VMEXIT
			 bit[3]: Enable RDTSCP					//����RDTSCP �Ƿ�a��#UD
			 bit[4]: Virtualize x2APIC				//APIC?�M����?
			 bit[5]: Enable VPID					//?�Mcpu id ��춾�?������?�Ե�ַ, ���Ч��
			 bit[6]: WBINVD exiting					//WBINVDָ�� �Ƿ�a��VMEXIT
			 bit[7]: Unrestricted guest				//�Q���͑��C����?���ڷǷ�?��?ģʽ �� ��ģʽ
			 bit[8]: APIC-register virtualization   //APIC?�M����?
			 bit[9]: Virtual-interrupt delivery		//�Д�?�M�� �Լ�ģ�Ɍ���APIC�ļĴ��� �����Д����ȼ�
			 bit[10]: PAUSE-loop exiting				//
			 bit[11]: RDRAND exiting				//����RDRAND  �Ƿ�a��VMEXIT
			 bit[12]: Enable INVPCID				//����INVPCID �Ƿ�a��#UD����
			 bit[13]: Enable VM function			//�Ƿ񆢄�VMFUNC
			 bit[14]: VMCS Shadowing				//VMREAD/VMWRITE ?��Ӱ��VMCS
			 bit[16]: RDSEED exiting				//RDSEED �Ƿ�a��VMEXIT
			 bit[17]: Enable PML					//�Ƿ���Page-modification log, ?���ȴ�r?��dirty bit
			 bit[18]: EPT-violation (#VE)			//?����?�M�����ַ ?����EPT���ҵ�(һ?ʼֻ��?�m�ăȴ�K��ŵ�EPT?��)
			 bit[20]: Enable XSAVES/SRSTORS			//XSAVES/XRSTORS �Ƿ�a��#UD����
			 bit[25]: Use TSC scaling				//����RDTSC/RDTSCR/RDMSRָ��, �Ƿ񷵻ر��޸ĵ�ֵ
	3.3 Exception Bitmap(32bit)							//����λ�D, �����l���r->��32bit?��1λ, ��??λ��ֵ��1, �����t�a��VMEXIT, ��t��������IDT?��
	3.6 VM-Function Controls(64bit)					    //��Ҫ�ֶ�:Enable VM function = 1 , �Լ�?�ù���?�rʹ��(������?��������0)
	3.7 I/O Bitmap Address(64bit physical address) A/B  //use I/O bitmaps  = 1 �rʹ��
			A����: 0000~07fff 
			B����: 8000~fffff
	3.8 Time-stamp Counter Offset and Multipler	(64bit)	//���r?��?�S
	3.9 Guest/Host masks CR0/CR4						//����CR0/CR4 ��mask ?����?����?��??�Ĵ����ę�
	3.10 read shadow for CR0/CR4(32/64bit)				//?ȡcr0 ��cr3,?ȡ�ĕr��,���،�����read shadow�е�ֵ
	3.11 CR3-Target controls(32/64bit)					//��4?cr3-target values �Լ� 1?cr3-target count
														//CR3-count = N, ����ֻ��?��N?CR3 �Ƿ�һ��, ���һ��,�t����VMM
														//CR3-count = 0, �t����CR3�r�o�l���l��VMEXIT, ����VMM
  3.12 Control for APIC Virtualization
		������:
		  - LAPIC�����xAPICģʽ, �t����͸?�ȴ�ӳ��?��LAPIC�Ĵ���, �������ַ��IA32_APIC_BASE MSR
		  - LAPIC�����x2APICģʽ, �t����͸?RDMSR��WRMSR?��LAPIC�Ĵ���
		  - 64λģʽ, ����ʹ��MOV CR8, ?��TPR
		?�M��:
			APIC-access Address(64bits)					//��virtualize APIC accesses = 1 �r��Ч , = 0 �t������
			Virtual-APIC Address(64bits)					//����,��Use TPR shadow = 1(ֻ�����?��?�õ�CPU����)
														//����ַָ��4kb�������ַ,��?�MAPIC? 
														//���?�M���Д� ��?��APIC�Ĵ���
			TPR threshold(32bits)						//��

  3.13 MSR-Bitmap address								//Use MSR bitmap = 1, ��?��msr�r, ecx�ڹ����� �t�a��VMEXIT ����VMM
		Read bitmap for low MSRs  [000000000~00001FFF]  
		Read bitmap for high MSRs [C00000000~C0001FFF]
		Write bitmap for low MSRs [000000000~00001FFF]
		Write bitmap for high MSRs[C00000000~C0001FFF] 
  3.14 Executive-VMCS Pointer							//���SMM+SMI ???��
  3.15 EPT Pointer(64bits)								//enable EPT = 1 �r��Ч
		bit[2:0] - Memory Type							//EPT?��: 6-�،�/0-���ɾ�? , ���鿴MSR IA32_VMX_EPT_VPID_CAP ֧�ֵ�EPT?��
		bit[5:3] - Page walk lenght						//EPT���Ӕ�
		bit[6]   - enabled accesses/dirty bit			//�����?��, ��������CPU֧��??����,  ���鿴MSR ͬ��
		bit[11:7]- ������								//��0
		bit[N-1:12] - 4KB��?��PML4�����ַ				//N�����Ƿ������ַ����, ����EAX�������ַ���Ȟ�[7:0] 
  3.16 VPID												//?�Mcpu��id,������TLB��??
  3.17 Control for PAUSE-Loop exit(32bit field)			//PLE_GAP / PLE_WINDOW
  3.17 Page-Modification Logging(64bit address)			//��Ҫ�ֶ�:Enable PML = 1 �rʹ��
  3.18 VM Function Control								//����?�õ�VM����, ����?��?��0 �ǾͰ�0λ?�Þ�1 ������0
  3.19 vmcs shadowing bitmap address(64bit physical addr)// VMCS Shadowing = 1, vmread / vmwrite ?��??��ַ ������ԭ?��vmcs
  3.20 Virtualization Exception							//������ַ,??�����ĵ�ַ,eptp index : �l��??��eptp����
  3.21 xss-exiting bitmap								//enable XSAVES/XRSTORES = 1, �tʹ�������r��?��??BITMAP ������xss�Ĵ���
  * 5. VM-exit control fields							
				
  * 6. VM-entry control feilds

  * 7. VM-exit information fields
  */

  //����?��MSR��
  VmxVmEntryControls vm_entryctl = {VmpAdjustControlValue(
      (use_true_msrs) ? Msr::kIa32VmxTrueEntryCtls : Msr::kIa32VmxEntryCtls,
      vm_entryctl_requested.all)};

  VmxVmExitControls vm_exitctl_requested = {};
  vm_exitctl_requested.fields.acknowledge_interrupt_on_exit = true;
  vm_exitctl_requested.fields.host_address_space_size = IsX64();
  
  VmxVmExitControls vm_exitctl = {VmpAdjustControlValue(
      (use_true_msrs) ? Msr::kIa32VmxTrueExitCtls : Msr::kIa32VmxExitCtls,
      vm_exitctl_requested.all)};

  VmxPinBasedControls vm_pinctl_requested = {};

  VmxPinBasedControls vm_pinctl = {
      VmpAdjustControlValue((use_true_msrs) ? Msr::kIa32VmxTruePinbasedCtls
                                            : Msr::kIa32VmxPinbasedCtls,
                            vm_pinctl_requested.all)};

  //��һ��Processor-based vm-Execution�ֶε��Զ��x?��
  VmxProcessorBasedControls vm_procctl_requested = {};
  vm_procctl_requested.fields.invlpg_exiting = false;	 //?������INVLPG����VMM(INVLPG XXX ?�ð�����xxx��TLB?��?��?�Þ�oЧ�r?��)
  vm_procctl_requested.fields.rdtsc_exiting = false;	 //?ȡtsc�Ĵ����r����vmm
  vm_procctl_requested.fields.cr3_load_exiting = true;	 //����cr3�Ĵ����r����vmm
  vm_procctl_requested.fields.cr3_store_exiting = true;
  vm_procctl_requested.fields.cr8_load_exiting = false;  //����cr8�Ĵ����r����vmm NB: very frequent
  vm_procctl_requested.fields.mov_dr_exiting = true;	 //����drx�Ĵ����r����VMM
  vm_procctl_requested.fields.use_msr_bitmaps = true;	 //ʹ��MSRλ�D
  vm_procctl_requested.fields.activate_secondary_control = true;

  //?�õ�һ��Processor-based vm-Execution�ֶ�, �������?�ô�ź�
  VmxProcessorBasedControls vm_procctl = {
      VmpAdjustControlValue((use_true_msrs) ? Msr::kIa32VmxTrueProcBasedCtls
                                            : Msr::kIa32VmxProcBasedCtls,
                            vm_procctl_requested.all)};

  //�ڶ���Processor-based vm-Execution�ֶε��Զ��x?��
  VmxSecondaryProcessorBasedControls vm_procctl2_requested = {};
  vm_procctl2_requested.fields.enable_ept = true;	  //����ept
  vm_procctl2_requested.fields.enable_rdtscp = true;  //Required for Win10
  vm_procctl2_requested.fields.descriptor_table_exiting = true; //���ж�?���ӕr����vmm

  // required for Win10 , ���??λ��0 , ���l#UD����
  vm_procctl2_requested.fields.enable_xsaves_xstors = true;
  // ?�õڶ���Processor-based vm-Execution�ֶ�
  VmxSecondaryProcessorBasedControls vm_procctl2 = {VmpAdjustControlValue(Msr::kIa32VmxProcBasedCtls2, vm_procctl2_requested.all)};

  // Set up CR0 and CR4 bitmaps
  // - Where a bit is     masked, the shadow bit appears
  // - Where a bit is not masked, the actual bit appears
  // VM-exit occurs when a guest modifies any of those fields
  Cr0 cr0_mask = {};
  Cr4 cr4_mask = {};

  // See: PDPTE Registers
  // If PAE paging would be in use following an execution of MOV to CR0 or MOV
  // to CR4 (see Section 4.1.1) and the instruction is modifying any of CR0.CD,
  // CR0.NW, CR0.PG, CR4.PAE, CR4.PGE, CR4.PSE, or CR4.SMEP; then the PDPTEs are
  // loaded from the address in CR3.

  // �Ƿ�PAEģʽ, �����PAEģʽ �t?������λ
  if (UtilIsX86Pae()) {
    cr0_mask.fields.pg = true;
    cr0_mask.fields.cd = true;
    cr0_mask.fields.nw = true;
    cr4_mask.fields.pae = true;
    cr4_mask.fields.pge = true;
    cr4_mask.fields.pse = true;
    cr4_mask.fields.smep = true;
  }
  //��λֻ#BP����������
  const auto exception_bitmap =
      1 << InterruptionVector::kBreakpointException |
      1 << InterruptionVector::kGeneralProtectionException |
      1 << InterruptionVector::kPageFaultException |
	  1 << InterruptionVector::kTrapFlags |
      0;

  // clang-format off
  /* 16-Bit Control Field */

  /* 16-Bit Guest-State Fields */
  /*�������ж�?����*/
  auto error = VmxStatus::kOk;
  error |= UtilVmWrite(VmcsField::kGuestEsSelector, AsmReadES());
  error |= UtilVmWrite(VmcsField::kGuestCsSelector, AsmReadCS());
  error |= UtilVmWrite(VmcsField::kGuestSsSelector, AsmReadSS());
  error |= UtilVmWrite(VmcsField::kGuestDsSelector, AsmReadDS());
  error |= UtilVmWrite(VmcsField::kGuestFsSelector, AsmReadFS());
  error |= UtilVmWrite(VmcsField::kGuestGsSelector, AsmReadGS());
  error |= UtilVmWrite(VmcsField::kGuestLdtrSelector, AsmReadLDTR());
  error |= UtilVmWrite(VmcsField::kGuestTrSelector, AsmReadTR());

  /* 16-Bit Host-State Fields */
  // RPL and TI have to be 0
  /*�������ж��x���� ��RPL / TIλ��0 (δ֪ԭ��)*/ 
  error |= UtilVmWrite(VmcsField::kHostEsSelector, AsmReadES() & 0xf8);
  error |= UtilVmWrite(VmcsField::kHostCsSelector, AsmReadCS() & 0xf8);
  error |= UtilVmWrite(VmcsField::kHostSsSelector, AsmReadSS() & 0xf8);
  error |= UtilVmWrite(VmcsField::kHostDsSelector, AsmReadDS() & 0xf8);
  error |= UtilVmWrite(VmcsField::kHostFsSelector, AsmReadFS() & 0xf8);
  error |= UtilVmWrite(VmcsField::kHostGsSelector, AsmReadGS() & 0xf8);
  error |= UtilVmWrite(VmcsField::kHostTrSelector, AsmReadTR() & 0xf8);

  /* 64-Bit Control Fields */
  /* �Զ��xMSR + �Զ��xEPT */
  error |= UtilVmWrite64(VmcsField::kIoBitmapA, 0);
  error |= UtilVmWrite64(VmcsField::kIoBitmapB, 0);	
  error |= UtilVmWrite64(VmcsField::kMsrBitmap, UtilPaFromVa(processor_data->shared_data->msr_bitmap));	//��ʹ���Լ�һ��msr
  error |= UtilVmWrite64(VmcsField::kEptPointer, EptGetEptPointer(processor_data->ept_data));			//ʹ���Լ�ept(??�Ǆ�����ʼ����?�m�ăȴ��ept)

  /* 64-Bit Guest-State Fields */
  error |= UtilVmWrite64(VmcsField::kVmcsLinkPointer, MAXULONG64);//��ʹ��Ӱ��VMCS
  error |= UtilVmWrite64(VmcsField::kGuestIa32Debugctl, UtilReadMsr64(Msr::kIa32Debugctl));
  if (UtilIsX86Pae()) {
    UtilLoadPdptes(__readcr3());
  }

  /* 32-Bit Control Fields */

  error |= UtilVmWrite(VmcsField::kPinBasedVmExecControl, vm_pinctl.all);		//ʹ��??ֵ
  error |= UtilVmWrite(VmcsField::kCpuBasedVmExecControl, vm_procctl.all);		//��Ҫ�ֶ�, �����Զ��x?��
  error |= UtilVmWrite(VmcsField::kExceptionBitmap, exception_bitmap);			//�Լ�?���
  error |= UtilVmWrite(VmcsField::kPageFaultErrorCodeMask, 0);					
  error |= UtilVmWrite(VmcsField::kPageFaultErrorCodeMatch, 0);					
  error |= UtilVmWrite(VmcsField::kCr3TargetCount, 0);							//**����cr3��Ҫ��?**
  error |= UtilVmWrite(VmcsField::kVmExitControls, vm_exitctl.all);				//EXIT CONTROL�Զ��x�����Д�
  error |= UtilVmWrite(VmcsField::kVmExitMsrStoreCount, 0);
  error |= UtilVmWrite(VmcsField::kVmExitMsrLoadCount, 0);
  error |= UtilVmWrite(VmcsField::kVmEntryControls, vm_entryctl.all);			//���ؕr������, ??ֵ
  error |= UtilVmWrite(VmcsField::kVmEntryMsrLoadCount, 0);
  error |= UtilVmWrite(VmcsField::kVmEntryIntrInfoField, 0);
  error |= UtilVmWrite(VmcsField::kSecondaryVmExecControl, vm_procctl2.all);	//?�ô�Ҫ�ֶ�

  /* 32-Bit Guest-State Fields */
  /* ��ʼ���͑��Ķ��x���ӵ�32λ�Ĵ�С,���� */
  error |= UtilVmWrite(VmcsField::kGuestEsLimit, GetSegmentLimit(AsmReadES()));	
  error |= UtilVmWrite(VmcsField::kGuestCsLimit, GetSegmentLimit(AsmReadCS()));
  error |= UtilVmWrite(VmcsField::kGuestSsLimit, GetSegmentLimit(AsmReadSS()));
  error |= UtilVmWrite(VmcsField::kGuestDsLimit, GetSegmentLimit(AsmReadDS()));
  error |= UtilVmWrite(VmcsField::kGuestFsLimit, GetSegmentLimit(AsmReadFS()));
  error |= UtilVmWrite(VmcsField::kGuestGsLimit, GetSegmentLimit(AsmReadGS()));
  error |= UtilVmWrite(VmcsField::kGuestLdtrLimit, GetSegmentLimit(AsmReadLDTR()));
  error |= UtilVmWrite(VmcsField::kGuestTrLimit, GetSegmentLimit(AsmReadTR()));
  error |= UtilVmWrite(VmcsField::kGuestGdtrLimit, gdtr.limit);
  error |= UtilVmWrite(VmcsField::kGuestIdtrLimit, idtr.limit);
  error |= UtilVmWrite(VmcsField::kGuestEsArBytes, VmpGetSegmentAccessRight(AsmReadES()));
  error |= UtilVmWrite(VmcsField::kGuestCsArBytes, VmpGetSegmentAccessRight(AsmReadCS()));
  error |= UtilVmWrite(VmcsField::kGuestSsArBytes, VmpGetSegmentAccessRight(AsmReadSS()));
  error |= UtilVmWrite(VmcsField::kGuestDsArBytes, VmpGetSegmentAccessRight(AsmReadDS()));
  error |= UtilVmWrite(VmcsField::kGuestFsArBytes, VmpGetSegmentAccessRight(AsmReadFS()));
  error |= UtilVmWrite(VmcsField::kGuestGsArBytes, VmpGetSegmentAccessRight(AsmReadGS()));
  error |= UtilVmWrite(VmcsField::kGuestLdtrArBytes, VmpGetSegmentAccessRight(AsmReadLDTR()));
  error |= UtilVmWrite(VmcsField::kGuestTrArBytes, VmpGetSegmentAccessRight(AsmReadTR()));
  error |= UtilVmWrite(VmcsField::kGuestInterruptibilityInfo, 0);
  error |= UtilVmWrite(VmcsField::kGuestActivityState, 0);
  error |= UtilVmWrite(VmcsField::kGuestSysenterCs, UtilReadMsr(Msr::kIa32SysenterCs));	   //���a�� XP��0
  
  /* 32-Bit Host-State Field */
  error |= UtilVmWrite(VmcsField::kHostIa32SysenterCs, UtilReadMsr(Msr::kIa32SysenterCs)); //ͬ��

  /* Natural-Width Control Fields */
  error |= UtilVmWrite(VmcsField::kCr0GuestHostMask, cr0_mask.all);	//?�ÿ͑��C��CR0 MASK ��0, ��?��?����
  error |= UtilVmWrite(VmcsField::kCr4GuestHostMask, cr4_mask.all); //ͬ��
  error |= UtilVmWrite(VmcsField::kCr0ReadShadow, __readcr0());		//��ǰCR0
  error |= UtilVmWrite(VmcsField::kCr4ReadShadow, __readcr4());		//��ǰCR4

  /* Natural-Width Guest-State Fields */
  //����cr0 , cr3 , cr4
  error |= UtilVmWrite(VmcsField::kGuestCr0, __readcr0());			//?��͑�CR0
  error |= UtilVmWrite(VmcsField::kGuestCr3, __readcr3());			//?��͑�CR3
  error |= UtilVmWrite(VmcsField::kGuestCr4, __readcr4());			//?��͑�CR4
#if defined(_AMD64_)												//���¶���64λ��ֵ
  //����͑��C��?���ӵĻ�ַ
  error |= UtilVmWrite(VmcsField::kGuestEsBase, 0);					
  error |= UtilVmWrite(VmcsField::kGuestCsBase, 0);
  error |= UtilVmWrite(VmcsField::kGuestSsBase, 0);
  error |= UtilVmWrite(VmcsField::kGuestDsBase, 0);
  error |= UtilVmWrite(VmcsField::kGuestFsBase, UtilReadMsr(Msr::kIa32FsBase));
  error |= UtilVmWrite(VmcsField::kGuestGsBase, UtilReadMsr(Msr::kIa32GsBase));
#else
  error |= UtilVmWrite(VmcsField::kGuestEsBase, VmpGetSegmentBase(gdtr.base, AsmReadES()));
  error |= UtilVmWrite(VmcsField::kGuestCsBase, VmpGetSegmentBase(gdtr.base, AsmReadCS()));
  error |= UtilVmWrite(VmcsField::kGuestSsBase, VmpGetSegmentBase(gdtr.base, AsmReadSS()));
  error |= UtilVmWrite(VmcsField::kGuestDsBase, VmpGetSegmentBase(gdtr.base, AsmReadDS()));
  error |= UtilVmWrite(VmcsField::kGuestFsBase, VmpGetSegmentBase(gdtr.base, AsmReadFS()));
  error |= UtilVmWrite(VmcsField::kGuestGsBase, VmpGetSegmentBase(gdtr.base, AsmReadGS()));
#endif

  //���� ����͑��C���������� ���VMENTRY�r�֏�
  error |= UtilVmWrite(VmcsField::kGuestLdtrBase, VmpGetSegmentBase(gdtr.base, AsmReadLDTR()));
  error |= UtilVmWrite(VmcsField::kGuestTrBase, VmpGetSegmentBase(gdtr.base, AsmReadTR()));
  error |= UtilVmWrite(VmcsField::kGuestGdtrBase, gdtr.base);				//����GDT��ַ
  error |= UtilVmWrite(VmcsField::kGuestIdtrBase, idtr.base);				//����IDT��ַ
  error |= UtilVmWrite(VmcsField::kGuestDr7, __readdr(7));					//���뮔ǰCPU DR7�Ĵ���
  error |= UtilVmWrite(VmcsField::kGuestRsp, guest_stack_pointer);			//����͑��C?��VMXǰ��rsp
  error |= UtilVmWrite(VmcsField::kGuestRip, guest_instruction_pointer);	//����͑��C?��VMXǰ��rip
  error |= UtilVmWrite(VmcsField::kGuestRflags, __readeflags());			//����͑��C��RLAGS
  error |= UtilVmWrite(VmcsField::kGuestSysenterEsp, UtilReadMsr(Msr::kIa32SysenterEsp));//�͑��C��ϵ�y?��ESP ��EIP	
  error |= UtilVmWrite(VmcsField::kGuestSysenterEip, UtilReadMsr(Msr::kIa32SysenterEip));

  /* Natural-Width Host-State Fields */
 
  error |= UtilVmWrite(VmcsField::kHostCr0, __readcr0());		//CR0
  error |= UtilVmWrite(VmcsField::kHostCr3, __readcr3());		//CR3 
  error |= UtilVmWrite(VmcsField::kHostCr4, __readcr4());		//CR4
#if defined(_AMD64_)
  error |= UtilVmWrite(VmcsField::kHostFsBase, UtilReadMsr(Msr::kIa32FsBase));
  error |= UtilVmWrite(VmcsField::kHostGsBase, UtilReadMsr(Msr::kIa32GsBase));
#else
  error |= UtilVmWrite(VmcsField::kHostFsBase, VmpGetSegmentBase(gdtr.base, AsmReadFS()));
  error |= UtilVmWrite(VmcsField::kHostGsBase, VmpGetSegmentBase(gdtr.base, AsmReadGS()));
#endif
  error |= UtilVmWrite(VmcsField::kHostTrBase, VmpGetSegmentBase(gdtr.base, AsmReadTR()));
  error |= UtilVmWrite(VmcsField::kHostGdtrBase, gdtr.base);
  error |= UtilVmWrite(VmcsField::kHostIdtrBase, idtr.base);
  error |= UtilVmWrite(VmcsField::kHostIa32SysenterEsp, UtilReadMsr(Msr::kIa32SysenterEsp));
  error |= UtilVmWrite(VmcsField::kHostIa32SysenterEip, UtilReadMsr(Msr::kIa32SysenterEip));
  //�o���ė���?
  error |= UtilVmWrite(VmcsField::kHostRsp, vmm_stack_pointer);
  //?��VMEXIT��?����(�R�����F)
  error |= UtilVmWrite(VmcsField::kHostRip, reinterpret_cast<ULONG_PTR>(AsmVmmEntryPoint));
  // clang-format on

  const auto vmx_status = static_cast<VmxStatus>(error);
  return vmx_status == VmxStatus::kOk;
}

// Executes vmlaunch
/*_Use_decl_annotations_*/ static void VmpLaunchVM() {
  auto error_code = UtilVmRead(VmcsField::kVmInstructionError);
  if (error_code) {
    HYPERPLATFORM_LOG_WARN("VM_INSTRUCTION_ERROR = %d", error_code);
  }
  HYPERPLATFORM_COMMON_DBG_BREAK();
  auto vmx_status = static_cast<VmxStatus>(__vmx_vmlaunch());

  // Here is not be executed with successful vmlaunch. Instead, the context
  // jumps to an address specified by GUEST_RIP.
  if (vmx_status == VmxStatus::kErrorWithStatus) {
    error_code = UtilVmRead(VmcsField::kVmInstructionError);
    HYPERPLATFORM_LOG_ERROR("VM_INSTRUCTION_ERROR = %d", error_code);
  }
  HYPERPLATFORM_COMMON_DBG_BREAK();
}

// Returns access right of the segment specified by the SegmentSelector for VMX
_Use_decl_annotations_ static ULONG VmpGetSegmentAccessRight(
    USHORT segment_selector) {
  VmxRegmentDescriptorAccessRight access_right = {};
  const SegmentSelector ss = {segment_selector};
  if (segment_selector) {
    auto native_access_right = AsmLoadAccessRightsByte(ss.all);
    native_access_right >>= 8;
    access_right.all = static_cast<ULONG>(native_access_right);
    access_right.fields.reserved1 = 0;
    access_right.fields.reserved2 = 0;
    access_right.fields.unusable = false;
  } else {
    access_right.fields.unusable = true;
  }
  return access_right.all;
}

// Returns a base address of the segment specified by SegmentSelector
_Use_decl_annotations_ static ULONG_PTR VmpGetSegmentBase(
    ULONG_PTR gdt_base, USHORT segment_selector) {
  const SegmentSelector ss = {segment_selector};
  if (!ss.all) {
    return 0;
  }

  if (ss.fields.ti) {
    const auto local_segment_descriptor =
        VmpGetSegmentDescriptor(gdt_base, AsmReadLDTR());
    const auto ldt_base =
        VmpGetSegmentBaseByDescriptor(local_segment_descriptor);
    const auto segment_descriptor =
        VmpGetSegmentDescriptor(ldt_base, segment_selector);
    return VmpGetSegmentBaseByDescriptor(segment_descriptor);
  } else {
    const auto segment_descriptor =
        VmpGetSegmentDescriptor(gdt_base, segment_selector);
    return VmpGetSegmentBaseByDescriptor(segment_descriptor);
  }
}

// Returns the segment descriptor corresponds to the SegmentSelector
_Use_decl_annotations_ static SegmentDesctiptor *VmpGetSegmentDescriptor(
    ULONG_PTR descriptor_table_base, USHORT segment_selector) {
  const SegmentSelector ss = {segment_selector};
  return reinterpret_cast<SegmentDesctiptor *>(
      descriptor_table_base + ss.fields.index * sizeof(SegmentDesctiptor));
}

// Returns a base address of segment_descriptor
_Use_decl_annotations_ static ULONG_PTR VmpGetSegmentBaseByDescriptor(
    const SegmentDesctiptor *segment_descriptor) {
  // Caluculate a 32bit base address
  const auto base_high = segment_descriptor->fields.base_high << (6 * 4);
  const auto base_middle = segment_descriptor->fields.base_mid << (4 * 4);
  const auto base_low = segment_descriptor->fields.base_low;
  ULONG_PTR base = (base_high | base_middle | base_low) & MAXULONG;
  // Get upper 32bit of the base address if needed
  if (IsX64() && !segment_descriptor->fields.system) {
    auto desc64 =
        reinterpret_cast<const SegmentDesctiptorX64 *>(segment_descriptor);
    ULONG64 base_upper32 = desc64->base_upper32;
    base |= (base_upper32 << 32);
  }
  return base;
}

// Adjust the requested control value with consulting a value of related MSR
_Use_decl_annotations_ static ULONG VmpAdjustControlValue(
    Msr msr, ULONG requested_value) {
  LARGE_INTEGER msr_value = {};

  //?ȡ��ҰMSR��ֵ
  msr_value.QuadPart = UtilReadMsr64(msr);
  //Ҫ������ֵ
  auto adjusted_value = requested_value;

  // bit == 0 in high word ==> must be zero ���϶�..
  adjusted_value &= msr_value.HighPart;
  // bit == 1 in low word  ==> must be one
  adjusted_value |= msr_value.LowPart;
  return adjusted_value;
}

// Terminates VM
_Use_decl_annotations_ void VmTermination() {
  PAGED_CODE();
  // Create a thread dedicated to de-virtualizing processors. For some reasons,
  // de-virtualizing processors from this thread makes the system stop
  // processing all timer related events and functioning properly.
  HANDLE thread_handle = nullptr;
  auto status =
      PsCreateSystemThread(&thread_handle, GENERIC_ALL, nullptr, nullptr,
                           nullptr, VmpVmxOffThreadRoutine, nullptr);
  if (NT_SUCCESS(status)) {
    // Wait until the thread ends its work.
    status = ZwWaitForSingleObject(thread_handle, FALSE, nullptr);
    status = ZwClose(thread_handle);
  } else {
    HYPERPLATFORM_COMMON_DBG_BREAK();
  }
  NT_ASSERT(!VmpIsVmmInstalled());
}

// De-virtualizing all processors
_Use_decl_annotations_ static void VmpVmxOffThreadRoutine(void *start_context) {
  UNREFERENCED_PARAMETER(start_context);
  PAGED_CODE();

  HYPERPLATFORM_LOG_INFO("Uninstalling VMM.");
  DdimonTermination();
  auto status = UtilForEachProcessor(VmpStopVM, nullptr);
  if (NT_SUCCESS(status)) {
    HYPERPLATFORM_LOG_INFO("The VMM has been uninstalled.");
  } else {
    HYPERPLATFORM_LOG_WARN("The VMM has not been uninstalled (%08x).", status);
  }
  PsTerminateSystemThread(status);
}

// Stops virtualization through a hypercall and frees all related memory
_Use_decl_annotations_ static NTSTATUS VmpStopVM(void *context) {
  UNREFERENCED_PARAMETER(context);

  HYPERPLATFORM_LOG_INFO("Terminating VMX for the processor %d.",
                         KeGetCurrentProcessorNumberEx(nullptr));

  // Stop virtualization and get an address of the management structure
  ProcessorData *processor_data = nullptr;
  auto status = UtilVmCall(HypercallNumber::kTerminateVmm, &processor_data);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  VmpFreeProcessorData(processor_data);
  return STATUS_SUCCESS;
}

// Frees all related memory
_Use_decl_annotations_ static void VmpFreeProcessorData(
    ProcessorData *processor_data) {
  if (!processor_data) {
    return;
  }
  if (processor_data->vmm_stack_limit) {
    UtilFreeContiguousMemory(processor_data->vmm_stack_limit);
  }
  if (processor_data->vmcs_region) {
    ExFreePoolWithTag(processor_data->vmcs_region, kHyperPlatformCommonPoolTag);
  }
  if (processor_data->vmxon_region) {
    ExFreePoolWithTag(processor_data->vmxon_region,
                      kHyperPlatformCommonPoolTag);
  }
  if (processor_data->sh_data) {
    ShFreeShadowHookData(processor_data->sh_data);
  }
  if (processor_data->ept_data) {
    EptTermination(processor_data->ept_data);
  }

  // Free shared data if this is the last reference
  if (processor_data->shared_data &&
      InterlockedDecrement(&processor_data->shared_data->reference_count) ==
          0) {
    HYPERPLATFORM_LOG_DEBUG("Freeing shared data...");
    if (processor_data->shared_data->msr_bitmap) {
      ExFreePoolWithTag(processor_data->shared_data->msr_bitmap,
                        kHyperPlatformCommonPoolTag);
    }
    if (processor_data->shared_data->shared_sh_data) {
      ShFreeSharedShadowHookData(processor_data->shared_data->shared_sh_data);
    }
    ExFreePoolWithTag(processor_data->shared_data, kHyperPlatformCommonPoolTag);
  }

  ExFreePoolWithTag(processor_data, kHyperPlatformCommonPoolTag);
}

// Tests if the VMM is already installed using a backdoor command
/*_Use_decl_annotations_*/ static bool VmpIsVmmInstalled() {
  int cpu_info[4] = {};
  __cpuidex(cpu_info, 0, kHyperPlatformVmmBackdoorCode);
  char vendor_id[13] = {};
  RtlCopyMemory(&vendor_id[0], &cpu_info[1], 4);  // ebx
  RtlCopyMemory(&vendor_id[4], &cpu_info[3], 4);  // edx
  RtlCopyMemory(&vendor_id[8], &cpu_info[2], 4);  // ecx
  return RtlCompareMemory(vendor_id, "Pong by VMM!\0", sizeof(vendor_id)) ==
         sizeof(vendor_id);
}

}  // extern "C"
