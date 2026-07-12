/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Joseph Lee; 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>

#include <rex/hook.h>
#include <rex/kernel/xbdm/private.h>
#include <rex/logging.h>
#include <rex/system/user_module.h>
#include <rex/system/xthread.h>

#define REX_EXPORT_RAW(name)                                                \
  REX_HOOK_RAW(name);                                                       \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);  \
  REX_HOOK_RAW(name)

namespace {

constexpr uint32_t kXbdmNoError = 0x02DA0000;
constexpr uint32_t kXbdmEndOfList = 0x82DA0104;
constexpr uint32_t kXbdmInvalidArgument = 0x82DA0004;

constexpr uint32_t kDmModuleInfoSize = 0x120;
constexpr uint32_t kDmModuleInfoNameSize = 0x100;
constexpr uint32_t kDmModuleInfoTypeOffset = 0x100;
constexpr uint32_t kDmModuleInfoBaseOffset = 0x104;
constexpr uint32_t kDmModuleInfoSizeOffset = 0x108;

void StoreGuestU32(uint8_t* data, uint32_t offset, uint32_t value) {
  *reinterpret_cast<rex::be<uint32_t>*>(data + offset) = value;
}

void FillModuleInfo(uint32_t info_guest, rex::system::UserModule* module) {
  auto* memory = REX_KERNEL_MEMORY();
  auto* info = memory->TranslateVirtual<uint8_t*>(info_guest);
  std::memset(info, 0, kDmModuleInfoSize);

  const auto& name = module->name();
  const size_t name_len = std::min<size_t>(name.size(), kDmModuleInfoNameSize - 1);
  std::memcpy(info, name.data(), name_len);

  StoreGuestU32(info, kDmModuleInfoTypeOffset, 1);
  StoreGuestU32(info, kDmModuleInfoBaseOffset, module->xex_module()->base_address());
  StoreGuestU32(info, kDmModuleInfoSizeOffset, module->xex_module()->image_size());
}

void DmWalkLoadedModulesImpl(PPCContext& ctx) {
  auto* memory = REX_KERNEL_MEMORY();
  const uint32_t walk_guest = ctx.r3.u32;
  const uint32_t info_guest = ctx.r4.u32;
  if (!walk_guest || !info_guest) {
    ctx.r3.u64 = kXbdmInvalidArgument;
    return;
  }

  auto* walk = memory->TranslateVirtual<rex::be<uint32_t>*>(walk_guest);
  if (static_cast<uint32_t>(*walk) != 0) {
    ctx.r3.u64 = kXbdmEndOfList;
    return;
  }

  auto module = REX_KERNEL_STATE()->GetExecutableModule();
  if (!module) {
    ctx.r3.u64 = kXbdmEndOfList;
    return;
  }

  *walk = 1;
  FillModuleInfo(info_guest, module.get());
  ctx.r3.u64 = kXbdmNoError;
}

}  // namespace

REX_EXPORT_RAW(__imp__DmCloseLoadedModules) {
  (void)base;
  ctx.r3.u64 = kXbdmNoError;
}

REX_EXPORT_RAW(__imp__DmIsDebuggerPresent) {
  (void)base;
  ctx.r3.u64 = 0;
}

REX_EXPORT_RAW(__imp__DmWalkLoadedModules) {
  (void)base;
  DmWalkLoadedModulesImpl(ctx);
}

REX_EXPORT_RAW(__imp__DmCaptureStackBackTrace) {
  (void)base;
  const uint32_t frame_count = ctx.r3.u32;
  const uint32_t frames_guest = ctx.r4.u32;
  if (frames_guest && frame_count) {
    auto* frames = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(frames_guest);
    std::memset(frames, 0, frame_count * sizeof(uint32_t));
  }
  ctx.r3.u64 = kXbdmNoError;
}

REX_EXPORT_RAW(__imp__DmGetThreadInfoEx) {
  (void)base;
  const uint32_t thread_id = ctx.r3.u32;
  const uint32_t info_guest = ctx.r4.u32;
  if (!info_guest) {
    ctx.r3.u64 = kXbdmInvalidArgument;
    return;
  }

  auto* memory = REX_KERNEL_MEMORY();
  auto* info = memory->TranslateVirtual<uint8_t*>(info_guest);
  const uint32_t requested_size = *reinterpret_cast<rex::be<uint32_t>*>(info);
  const uint32_t info_size = requested_size ? std::min<uint32_t>(requested_size, 56) : 56;
  std::memset(info, 0, info_size);

  auto thread = REX_KERNEL_STATE()->GetThreadByID(thread_id);
  if (!thread) {
    thread = rex::system::retain_object(rex::system::XThread::GetCurrentThread());
  }

  auto* kthread = thread->guest_object<rex::system::X_KTHREAD>();
  StoreGuestU32(info, 0, info_size);
  StoreGuestU32(info, 4, thread->thread_id());
  StoreGuestU32(info, 8, kthread->suspend_count);
  StoreGuestU32(info, 12, static_cast<uint32_t>(thread->priority()));
  StoreGuestU32(info, 16, kthread->start_address);
  StoreGuestU32(info, 20, kthread->stack_base);
  StoreGuestU32(info, 24, kthread->stack_limit);
  StoreGuestU32(info, 28, kthread->last_error);
  StoreGuestU32(info, 32, thread->guest_object());
  StoreGuestU32(info, 36, kthread->tls_address);
  if (info_size > 48) {
    StoreGuestU32(info, 40, info_guest + 48);
    const std::string name = fmt::format("Thread{}", thread->thread_id());
    const size_t copy_len = std::min<size_t>(name.size(), info_size - 49);
    std::memcpy(info + 48, name.data(), copy_len);
  }

  ctx.r3.u64 = kXbdmNoError;
}

REX_EXPORT_RAW(__imp__DmWalkLoadedModulesEx) {
  (void)base;
  DmWalkLoadedModulesImpl(ctx);
}

REX_EXPORT_STUB(__imp__DmAllocatePool);
REX_EXPORT_STUB(__imp__DmAllocatePoolWithTag);
REX_EXPORT_STUB(__imp__DmCloseCounters);
REX_EXPORT_STUB(__imp__DmCloseModuleSections_);
REX_EXPORT_STUB(__imp__DmCloseNotificationSession);
REX_EXPORT_STUB(__imp__DmClosePerformanceCounter);
REX_EXPORT_STUB(__imp__DmContinueThread);
REX_EXPORT_STUB(__imp__DmFreePool);
REX_EXPORT_STUB(__imp__DmGetMemory);
REX_EXPORT_STUB(__imp__DmGetModuleLongName);
REX_EXPORT_STUB(__imp__DmGetProcAddress);
REX_EXPORT_STUB(__imp__DmGetThreadContext);
REX_EXPORT_STUB(__imp__DmGetThreadList);
REX_EXPORT_STUB(__imp__DmGetXbeInfo);
REX_EXPORT_RAW(__imp__DmGetXboxName) {
  (void)base;
  const uint32_t name_guest = ctx.r3.u32;
  const uint32_t length_guest = ctx.r4.u32;
  if (!name_guest || !length_guest) {
    ctx.r3.u64 = kXbdmInvalidArgument;
    return;
  }

  constexpr char kXboxName[] = "REXGLUE";
  constexpr uint32_t kXboxNameSize = sizeof(kXboxName);
  auto* memory = REX_KERNEL_MEMORY();
  auto* length = memory->TranslateVirtual<rex::be<uint32_t>*>(length_guest);
  const uint32_t capacity = *length;
  *length = kXboxNameSize;
  if (capacity < kXboxNameSize) {
    ctx.r3.u64 = kXbdmInvalidArgument;
    return;
  }

  auto* name = memory->TranslateVirtual<char*>(name_guest);
  std::memset(name, 0, capacity);
  std::memcpy(name, kXboxName, kXboxNameSize);
  ctx.r3.u64 = kXbdmNoError;
}
REX_EXPORT_STUB(__imp__DmGo);
REX_EXPORT_STUB(__imp__DmHaltThread);
REX_EXPORT_STUB(__imp__DmIsThreadStopped);
REX_EXPORT_STUB(__imp__DmLoadExtension);
REX_EXPORT_STUB(__imp__DmNotify);
REX_EXPORT_STUB(__imp__DmOpenNotificationSession);
REX_EXPORT_STUB(__imp__DmOpenPerformanceCounter);
REX_EXPORT_STUB(__imp__DmQueryPerformanceCounterHandle);
REX_EXPORT_STUB(__imp__DmReboot);
REX_EXPORT_STUB(__imp__DmRegisterCommandProcessor);
REX_EXPORT_STUB(__imp__DmRegisterNotificationProcessor);
REX_EXPORT_STUB(__imp__DmRegisterPerformanceCounter);
REX_EXPORT_STUB(__imp__DmRemoveBreakpoint);
REX_EXPORT_STUB(__imp__DmResumeThread);
REX_EXPORT_STUB(__imp__DmSendNotificationString);
REX_EXPORT_STUB(__imp__DmSetBreakpoint);
REX_EXPORT_STUB(__imp__DmSetDataBreakpoint);
REX_EXPORT_STUB(__imp__DmSetInitialBreakpoint);
REX_EXPORT_STUB(__imp__DmSetMemory);
REX_EXPORT_STUB(__imp__DmSetThreadContext);
REX_EXPORT_STUB(__imp__DmSetTitle);
REX_EXPORT_RAW(__imp__DmSetXboxName) {
  (void)base;
  ctx.r3.u64 = kXbdmNoError;
}
REX_EXPORT_STUB(__imp__DmStop);
REX_EXPORT_STUB(__imp__DmStopOn);
REX_EXPORT_STUB(__imp__DmSuspendThread);
REX_EXPORT_STUB(__imp__DmThreadUserData);
REX_EXPORT_STUB(__imp__DmUnloadExtension);
REX_EXPORT_STUB(__imp__DmWalkModuleSections);
REX_EXPORT_STUB(__imp__DmWalkPerformanceCounters);
REX_EXPORT_STUB(__imp__DmCloseCounters_0);
REX_EXPORT_STUB(__imp__DmIsBreakpoint);
REX_EXPORT_STUB(__imp__DmCloseCounters_1);
REX_EXPORT_STUB(__imp__DmSetUserAccess);
REX_EXPORT_STUB(__imp__DmGetUserAccess);
REX_EXPORT_STUB(__imp__DmWalkUserList);
REX_EXPORT_STUB(__imp__DmAddUser);
REX_EXPORT_STUB(__imp__DmEnableSecurity);
REX_EXPORT_STUB(__imp__DmIsSecurityEnabled);
REX_EXPORT_STUB(__imp__DmRemoveUser);
REX_EXPORT_STUB(__imp____CAP_Start_Profiling);
REX_EXPORT_STUB(__imp____CAP_End_Profiling);
REX_EXPORT_STUB(__imp____CAP_Enter_Function);
REX_EXPORT_STUB(__imp____CAP_Exit_Function);
REX_EXPORT_STUB_RETURN(__imp__DmRegisterCommandProcessorEx, 0);
REX_EXPORT_STUB(__imp__DmStartProfiling);
REX_EXPORT_STUB(__imp__DmStopProfiling);
REX_EXPORT_STUB(__imp__DmQueryMemoryStatistics);
REX_EXPORT_STUB(__imp__DmEnableStackTrace);
REX_EXPORT_STUB(__imp__DmQueryAllocationTypeName);
REX_EXPORT_STUB(__imp__DmRegisterAllocationType);
REX_EXPORT_STUB(__imp__DmInsertAllocationEntry);
REX_EXPORT_STUB(__imp__DmRemoveAllocationEntry);
REX_EXPORT_STUB(__imp__DmSetTitleEx);
REX_EXPORT_STUB(__imp__DmCrashDump);
REX_EXPORT_STUB(__imp__DmIsFastCAPEnabled);
REX_EXPORT_STUB(__imp__DmGetFileAccessCount);
REX_EXPORT_STUB(__imp__DmGetUtilityDriveInfo);
REX_EXPORT_STUB(__imp__DmSetProfilingOptions);
REX_EXPORT_STUB(__imp__DmQuerySystemSettings);
REX_EXPORT_STUB(__imp__DmSaveSystemSettings);
REX_EXPORT_STUB(__imp__DmpGetPgoModuleHandleForBaseAddress);
REX_EXPORT_STUB(__imp__DmpOnPgoModuleLoad);
REX_EXPORT_STUB(__imp__DmPgoStartDataCollection);
REX_EXPORT_STUB(__imp__DmPgoStopDataCollection);
REX_EXPORT_STUB(__imp__DmPgoSaveSnapshot);
REX_EXPORT_STUB(__imp__IrtClientAbort);
REX_EXPORT_STUB(__imp__IrtPogoInit);
REX_EXPORT_STUB(__imp__IrtSetStaticInfo);
REX_EXPORT_STUB(__imp__IrtAutoSweepW);
REX_EXPORT_STUB(__imp__IrtAutoSweepA);
REX_EXPORT_STUB(__imp__DmGetDumpMode);
REX_EXPORT_STUB(__imp__DmSetDumpMode);
REX_EXPORT_STUB(__imp__DmGetDumpSettings);
REX_EXPORT_STUB(__imp__DmSetDumpSettings);
REX_EXPORT_STUB(__imp__DmGetEventDeferFlags);
REX_EXPORT_STUB(__imp__DmSetEventDeferFlags);
REX_EXPORT_STUB(__imp__DmWalkCommittedMemory);
REX_EXPORT_STUB(__imp__DmCloseCounters_2);
REX_EXPORT_STUB(__imp__DmRebootEx);
REX_EXPORT_STUB(__imp__DmMountFdfxVolume);
REX_EXPORT_STUB(__imp__DmCapGetFileHeader);
REX_EXPORT_STUB(__imp__DmCapFreeFileHeader);
REX_EXPORT_STUB(__imp__DmTraceStartRecording);
REX_EXPORT_STUB(__imp__DmTraceStartRecordingFunction);
REX_EXPORT_STUB(__imp__DmTraceSetBufferSize);
REX_EXPORT_STUB(__imp__DmTraceStopRecording);
REX_EXPORT_STUB(__imp__DmTraceSaveBuffer);
REX_EXPORT_STUB(__imp__DmGetConsoleType);
REX_EXPORT_STUB(__imp__DmMapDevkitDrive);
REX_EXPORT_STUB(__imp__DmGetXexHeaderField);
REX_EXPORT_STUB(__imp__DmGetMouseChanges);
REX_EXPORT_STUB(__imp__DmFindPdbSignature);
REX_EXPORT_STUB(__imp__DmGetProfilingStatus);
REX_EXPORT_STUB(__imp__DmOpticalDiscLogStart);
REX_EXPORT_STUB(__imp__DmOpticalDiscLogStop);
REX_EXPORT_STUB(__imp__DmStartSamplingProfiler);
REX_EXPORT_STUB(__imp__DmStopSamplingProfiler);
REX_EXPORT_STUB(__imp__DmGetSamplingProfilerInfo);
REX_EXPORT_STUB(__imp__DmStartFileEventCapture);
REX_EXPORT_STUB(__imp__DmStopFileEventCapture);
REX_EXPORT_STUB(__imp__DmSetFileEventMarker);
REX_EXPORT_STUB(__imp__DmMarkPseudoCreateBegin);
REX_EXPORT_STUB(__imp__DmMarkPseudoCreateEnd);
REX_EXPORT_STUB(__imp__DmMarkPseudoEventBegin);
REX_EXPORT_STUB(__imp__DmMarkPseudoEventEnd);
REX_EXPORT_STUB(__imp__DmMarkFileEventWorkerThreadBegin);
REX_EXPORT_STUB(__imp__DmMarkFileEventWorkerThreadEnd);
REX_EXPORT_STUB(__imp__DmGetSystemInfo);
REX_EXPORT_STUB(__imp__DmAbortProfiling);
REX_EXPORT_STUB(__imp__DmNetCaptureStart);
REX_EXPORT_STUB(__imp__DmNetCaptureStop);
REX_EXPORT_STUB(__imp__DmQueryTitleMemoryStatistics);
REX_EXPORT_STUB(__imp__DmAutomationGetInputProcess);
REX_EXPORT_STUB(__imp__DmAutomationBindController);
REX_EXPORT_STUB(__imp__DmAutomationUnbindController);
REX_EXPORT_STUB(__imp__DmAutomationConnectController);
REX_EXPORT_STUB(__imp__DmAutomationDisconnectController);
REX_EXPORT_STUB(__imp__DmAutomationSetGamepadState);
REX_EXPORT_STUB(__imp__DmAutomationQueueGamepadState);
REX_EXPORT_STUB(__imp__DmAutomationClearGamepadQueue);
REX_EXPORT_STUB(__imp__DmAutomationQueryGamepadQueue);
REX_EXPORT_STUB(__imp__DmAutomationGetUserDefaultProfile);
REX_EXPORT_STUB(__imp__DmAutomationSetUserDefaultProfile);
REX_EXPORT_STUB(__imp__DmTraceIsRecording);
REX_EXPORT_STUB(__imp__DmLoadDebuggerExtension);
REX_EXPORT_STUB(__imp__DmUnloadDebuggerExtension);
REX_EXPORT_STUB(__imp__DmCreateSystemThread);
REX_EXPORT_STUB(__imp__PEPLELookup);
REX_EXPORT_STUB(__imp__PEPLELookupCompact);
REX_EXPORT_STUB(__imp__PVPLELookup);
REX_EXPORT_STUB(__imp__PVPLELookupCompact);
REX_EXPORT_STUB(__imp__PVPLELookupCompactMDS);
REX_EXPORT_STUB(__imp__PVPLEFilteredLookup);
REX_EXPORT_STUB(__imp__PVPLEFilteredLookupCompact);
REX_EXPORT_STUB(__imp__PVPLEFilteredLookupCompactMDS);
REX_EXPORT_STUB(__imp__PVPLETemplatedLookup);
REX_EXPORT_STUB(__imp__PVPLETemplatedLookupCompact);
REX_EXPORT_STUB(__imp__PVPLETemplatedLookupCompactMDS);
REX_EXPORT_STUB(__imp__DmpPgoCounterOverflow);
REX_EXPORT_STUB(__imp__UpdateMDSProbeState);
REX_EXPORT_STUB(__imp__DmGetHttpRegistration);
REX_EXPORT_STUB(__imp__DmTraceSetIOThread);
REX_EXPORT_STUB(__imp__DmPMCInstallAndStart);
REX_EXPORT_STUB(__imp__DmPMCStopAndReport);
REX_EXPORT_STUB(__imp__DmPMCInstallSetup);
REX_EXPORT_STUB(__imp__DmPMCUnInstallSetup);
REX_EXPORT_STUB(__imp__DmPMCResetCounters);
REX_EXPORT_STUB(__imp__DmPMCSetTriggerProcessor);
REX_EXPORT_STUB(__imp__DmPMCStart);
REX_EXPORT_STUB(__imp__DmPMCStop);
REX_EXPORT_STUB(__imp__DmPMCGetCounter);
REX_EXPORT_STUB(__imp__DmPMCGetCounters);
REX_EXPORT_STUB(__imp__DmPMCGetCounterName);
REX_EXPORT_STUB(__imp__DmPMCDumpCounters);
REX_EXPORT_STUB(__imp__DmPMCDumpCountersVerbose);
REX_EXPORT_STUB(__imp__DmPMCGetCounterCostEstimate);
REX_EXPORT_STUB(__imp__DmPMCGetCounterSource);
REX_EXPORT_STUB(__imp__DmPMCComputeFrequency);
REX_EXPORT_STUB(__imp__DmGetAdditionalTitleMemorySetting);
REX_EXPORT_STUB(__imp__DmGetDebugMemorySize);
REX_EXPORT_STUB(__imp__DmGetConsoleDebugMemoryStatus);
REX_EXPORT_STUB(__imp__DmNetSimSetLinkStatusHidden);
REX_EXPORT_STUB(__imp__DmNetSimInsertQueue);
REX_EXPORT_STUB(__imp__DmNetSimRemoveQueue);
REX_EXPORT_STUB(__imp__DmNetSimRemoveAllQueues);
REX_EXPORT_STUB(__imp__DmNetSimModifyQueueSettings);
REX_EXPORT_STUB(__imp__DmGetConsoleFeatures);
REX_EXPORT_STUB(__imp__DmNetSimGetQueueStats);
REX_EXPORT_STUB(__imp__DmNetSimGetQueueSettings);
REX_EXPORT_STUB(__imp__DmNetSimGetNumQueues);
REX_EXPORT_STUB(__imp__DmNetSimInsertIpv4Redirect);
REX_EXPORT_STUB(__imp__DmNetSimRemoveIpv4Redirect);
REX_EXPORT_STUB(__imp__DmNetSimGetNumIpv4Redirects);
REX_EXPORT_STUB(__imp__XLFAllocate);
REX_EXPORT_STUB(__imp__XLFFree);
REX_EXPORT_STUB(__imp__XLFQueueAdd);
REX_EXPORT_STUB(__imp__XLFQueueAllocated);
REX_EXPORT_STUB(__imp__XLFQueueCreate);
REX_EXPORT_STUB(__imp__XLFQueueDestroy);
REX_EXPORT_STUB(__imp__XLFQueueGetEntryCount);
REX_EXPORT_STUB(__imp__XLFQueueIsEmpty);
REX_EXPORT_STUB(__imp__XLFQueueRemove);
REX_EXPORT_STUB(__imp__XLFQueueUnsafeDump);
REX_EXPORT_STUB(__imp__XLFStackAllocated);
REX_EXPORT_STUB(__imp__XLFStackCreate);
REX_EXPORT_STUB(__imp__XLFStackDestroy);
REX_EXPORT_STUB(__imp__XLFStackGetEntryCount);
REX_EXPORT_STUB(__imp__XLFStackIsEmpty);
REX_EXPORT_STUB(__imp__XLFStackPop);
REX_EXPORT_STUB(__imp__XLFStackPush);
REX_EXPORT_STUB(__imp__XLFStackUnsafeDump);
REX_EXPORT_STUB(__imp__XLFPriorityQueueAdd);
REX_EXPORT_STUB(__imp__XLFPriorityQueueAllocated);
REX_EXPORT_STUB(__imp__XLFPriorityQueueCreate);
REX_EXPORT_STUB(__imp__XLFPriorityQueueDestroy);
REX_EXPORT_STUB(__imp__XLFPriorityQueueGetEntryCount);
REX_EXPORT_STUB(__imp__XLFPriorityQueueIsEmpty);
REX_EXPORT_STUB(__imp__XLFPriorityQueueRemoveFirst);
REX_EXPORT_STUB(__imp__XLFPriorityQueueRemove);
REX_EXPORT_STUB(__imp__XLFPriorityQueueUnsafeDump);
REX_EXPORT_STUB(__imp__XLFHashTableAdd);
REX_EXPORT_STUB(__imp__XLFHashTableAllocated);
REX_EXPORT_STUB(__imp__XLFHashTableCreate);
REX_EXPORT_STUB(__imp__XLFHashTableDestroy);
REX_EXPORT_STUB(__imp__XLFHashTableGetEntryCount);
REX_EXPORT_STUB(__imp__XLFHashTableIsEmpty);
REX_EXPORT_STUB(__imp__XLFHashTableRemoveFirst);
REX_EXPORT_STUB(__imp__XLFHashTableRemove);
REX_EXPORT_STUB(__imp__XLFHashTableUnsafeDump);
REX_EXPORT_STUB(__imp__XLFPoolAcquireLock);
REX_EXPORT_STUB(__imp__XLFPoolAllocated);
REX_EXPORT_STUB(__imp__XLFPoolCreate);
REX_EXPORT_STUB(__imp__XLFPoolCreateLock);
REX_EXPORT_STUB(__imp__XLFPoolDestroy);
REX_EXPORT_STUB(__imp__XLFPoolDestroyLock);
REX_EXPORT_STUB(__imp__XLFPoolIncreaseEvents);
REX_EXPORT_STUB(__imp__XLFPoolIncreaseLocks);
REX_EXPORT_STUB(__imp__XLFPoolInitializeLock);
REX_EXPORT_STUB(__imp__XLFPoolNumberOfEvents);
REX_EXPORT_STUB(__imp__XLFPoolNumberOfLocks);
REX_EXPORT_STUB(__imp__XLFPoolReleaseLock);
REX_EXPORT_STUB(__imp__XLFStartLog);
REX_EXPORT_STUB(__imp__XLFStartUserLog);
REX_EXPORT_STUB(__imp__XLFInitializeLog);
REX_EXPORT_STUB(__imp__XLFInitializeUserLog);
REX_EXPORT_STUB(__imp__XLFEndLog);
REX_EXPORT_STUB(__imp__XLFLogPrint);
REX_EXPORT_STUB(__imp__XLFLogPrintV);
REX_EXPORT_STUB(__imp__XLFLogBuffer);
REX_EXPORT_STUB(__imp__XLFLogMessageStats);
REX_EXPORT_STUB(__imp__XLockFreeGetErrorHandler);
REX_EXPORT_STUB(__imp__XLockFreeSetErrorHandler);
REX_EXPORT_STUB(__imp__DmExecuteThreadRPC);
REX_EXPORT_STUB(__imp__DmGetDebuggerConnection);

#undef REX_EXPORT_RAW
