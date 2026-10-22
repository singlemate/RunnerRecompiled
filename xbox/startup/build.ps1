param([Parameter(Mandatory = $true)][string]$Workspace)
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $Workspace).Path
$taskNdk = Join-Path $taskRoot 'work\toolchains\android-ndk-r30\toolchains\llvm\prebuilt\windows-x86_64\bin'
$taskGenerated = Join-Path $taskRoot 'work\xbox-game-private\generated-fourth-final'
$taskArchive = Join-Path $taskRoot 'work\xbox-game-private\libmirrorsedge_ppc_mmio.a'
$taskSimde = Join-Path $taskRoot 'work\references\XenonRecomp\thirdparty\simde'
$taskOutput = Join-Path $taskRoot 'work\startup\current'
& (Join-Path $taskRoot 'work/venv/Scripts/python.exe') (Join-Path $PSScriptRoot '../gpu-port/prepare_external_presenter.py') $taskRoot
if ($LASTEXITCODE) { throw 'External presenter preparation failed' }
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$taskMapping = [IO.File]::ReadAllText((Join-Path $taskGenerated 'ppc_func_mapping.cpp'))
$taskImports = [regex]::Matches($taskMapping, '\{ 0x[0-9A-F]+, (__imp__\w+) \}') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$taskImplemented = @('__imp__XMsgStartIORequest', '__imp__XamEnumerate', '__imp__XamShowSigninUI','__imp__XamUserGetSigninInfo', '__imp__VdPersistDisplay','__imp__VdInitializeScalerCommandBuffer', '__imp__RtlFillMemoryUlong', '__imp__XMACreateContext', '__imp__XMAReleaseContext', '__imp__NetDll_XNetGetTitleXnAddr', '__imp__InterlockedPopEntrySList', '__imp__InterlockedFlushSList', '__imp__NetDll_XNetGetEthernetLinkStatus', '__imp__XamInputSetState', '__imp__RtlTimeToTimeFields', '__imp__RtlTimeFieldsToTime', '__imp__KeQuerySystemTime', '__imp__XamUserGetName', '__imp__XamUserReadProfileSettings', '__imp__NetDll_WSAWaitForMultipleEvents', '__imp__XamInputGetCapabilities', '__imp__XamUserCreateAchievementEnumerator', '__imp__XamUserGetXUID', '__imp__XamUserGetSigninState', '__imp__XamNotifyCreateListener', '__imp__XNotifyGetNext', '__imp__NtWaitForMultipleObjectsEx', '__imp__XamInputGetState', '__imp__XMsgStartIORequestEx', '__imp__XMsgInProcessCall', '__imp__NtResumeThread', '__imp__NtCreateMutant', '__imp__NtReleaseMutant', '__imp__NtSetInformationFile', '__imp__NetDll_WSAStartup', '__imp__MmQueryAddressProtect', '__imp__NetDll_XNetStartup', '__imp__XamGetSystemVersion', '__imp__MmQueryStatistics', '__imp__NtQueryInformationFile', '__imp__NtReadFile', '__imp__NtCreateFile', '__imp__RtlUnicodeToMultiByteN', '__imp__XexGetModuleSection', '__imp__XexGetModuleHandle', '__imp__KeEnableFpuExceptions', '__imp__NtQueryFullAttributesFile', '__imp__XAudioSubmitRenderDriverFrame', '__imp__KeWaitForMultipleObjects', '__imp__RtlInitAnsiString', '__imp__XAudioGetVoiceCategoryVolumeChangeMask', '__imp__XAudioRegisterRenderDriverClient', '__imp__KeRaiseIrqlToDpcLevel', '__imp__KfLowerIrql', '__imp__KeResumeThread', '__imp__KeInitializeSemaphore', '__imp__KeReleaseSemaphore', '__imp__KeAcquireSpinLockAtRaisedIrql', '__imp__KeReleaseSpinLockFromRaisedIrql', '__imp__KfAcquireSpinLock', '__imp__KfReleaseSpinLock', '__imp__VdSetDisplayMode', '__imp__VdGetSystemCommandBuffer', '__imp__VdSwap', '__imp__MmSetAddressProtect', '__imp__VdIsHSIOTrainingSucceeded', '__imp__VdRetrainEDRAMWorker', '__imp__VdRetrainEDRAM', '__imp__KeEnterCriticalRegion', '__imp__KeLeaveCriticalRegion', '__imp__KeInitializeEvent', '__imp__KeSetEvent', '__imp__KeResetEvent', '__imp__KeClearEvent', '__imp__KeWaitForSingleObject', '__imp__VdCallGraphicsNotificationRoutines', '__imp__VdQueryVideoFlags', '__imp__VdGetCurrentDisplayGamma', '__imp__VdGetCurrentDisplayInformation', '__imp__KeSetBasePriorityThread', '__imp__KiApcNormalRoutineNop', '__imp__VdSetSystemCommandBufferGpuIdentifierAddress', '__imp__ExGetXConfigSetting', '__imp__ExRegisterTitleTerminateNotification', '__imp__VdInitializeRingBuffer', '__imp__VdSetGraphicsInterruptCallback', '__imp__VdEnableRingBufferRPtrWriteBack', '__imp__VdInitializeEngines', '__imp__MmGetPhysicalAddress', '__imp__MmQueryAllocationSize', '__imp__MmFreePhysicalMemory', '__imp__MmAllocatePhysicalMemoryEx', '__imp__XGetVideoMode', '__imp__VdQueryVideoMode', '__imp__KeDelayExecutionThread', '__imp__ObReferenceObjectByHandle', '__imp__ObDereferenceObject', '__imp__KeSetAffinityThread', '__imp__ExTerminateThread', '__imp__ExCreateThread', '__imp__NtCreateEvent', '__imp__NtSetEvent', '__imp__NtClearEvent', '__imp__NtClose', '__imp__NtWaitForSingleObjectEx', '__imp__RtlNtStatusToDosError', '__imp__FscSetCacheElementCount', '__imp__XamLoaderGetLaunchDataSize', '__imp__KeQueryPerformanceFrequency', '__imp__RtlMultiByteToUnicodeN', '__imp__NtFreeVirtualMemory', '__imp__RtlTryEnterCriticalSection', '__imp__KeTlsAlloc', '__imp__KeTlsFree', '__imp__KeTlsGetValue', '__imp__KeTlsSetValue', '__imp__XexCheckExecutablePrivilege', '__imp__RtlEnterCriticalSection', '__imp__RtlLeaveCriticalSection', '__imp__RtlInitializeCriticalSection', '__imp__KeGetCurrentProcessType', '__imp__RtlImageXexHeaderField', '__imp__NtAllocateVirtualMemory')
$taskStubs = @('#include "ppc_recomp_shared.h"', '[[noreturn]] void me_stop_import(const char*,PPCContext&);')
foreach ($taskImport in $taskImports) {
    if ($taskImport -notin $taskImplemented) {
        $taskStubs += 'PPC_FUNC(' + $taskImport + ') { me_stop_import("' + $taskImport + '",ctx); }'
    }
}
$taskStubsPath = Join-Path $taskOutput 'import-stops.cpp'
[IO.File]::WriteAllLines($taskStubsPath, $taskStubs)
$taskClang = Join-Path $taskNdk 'aarch64-linux-android35-clang++.cmd'
$taskSdk = Join-Path $taskRoot 'work\references\rexglue-sdk'
$taskCommon = @('-std=c++23', '-O1', '-DSIMDE_ENABLE_NATIVE_ALIASES', '-include', (Join-Path $PSScriptRoot 'guest_memory_access.h'), "-I$taskGenerated", "-I$taskSimde", "-I$taskSdk/include")
$taskProbeObject = Join-Path $taskOutput 'entry.o'
$taskStubsObject = Join-Path $taskOutput 'imports.o'
& $taskClang @taskCommon -c (Join-Path $PSScriptRoot 'entry_probe.cpp') -o $taskProbeObject
if ($LASTEXITCODE) { throw 'Entry loader compile failed' }
& $taskClang @taskCommon -c $taskStubsPath -o $taskStubsObject
if ($LASTEXITCODE) { throw 'Import diagnostics compile failed' }
$taskExecutable = Join-Path $taskOutput 'me_memory_probe'
$taskRingObject = Join-Path $taskOutput 'rex-ring-buffer.o'
& $taskClang @taskCommon -c (Join-Path $taskSdk 'src\core\ring_buffer.cpp') -o $taskRingObject
if ($LASTEXITCODE) { throw 'RexGlue ring buffer compile failed' }
& (Join-Path $taskRoot 'work/venv/Scripts/python.exe') (Join-Path $PSScriptRoot 'link_gpu_loader.py') $taskRoot
if ($LASTEXITCODE) { throw 'GPU loader link failed' }
& (Join-Path $taskNdk 'llvm-strip.exe') -o ($taskExecutable + '.stripped') $taskExecutable
if ($LASTEXITCODE) { throw 'Strip failed' }
[PSCustomObject]@{Executable=($taskExecutable + '.stripped');ImplementedImports=$taskImplemented;DiagnosticImports=($taskImports.Count - $taskImplemented.Count)}
