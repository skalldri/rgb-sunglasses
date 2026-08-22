#include <extensions/extension_tick_budget.h>
#include <extensions/wasm_mvp_runtime.h>
#include <m3_env.h>
#include <m3_function.h>
#include <wasm3.h>
#include <wasm3_zephyr_port.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/libc-hooks.h>

#include <cstring>

LOG_MODULE_REGISTER(wasm_mvp_runtime, LOG_LEVEL_INF);

K_APPMEM_PARTITION_DEFINE(rgbx_wasm3_partition);

namespace wasm_mvp_runtime {
namespace {

using extension_host::evaluate_tick_budget;
using extension_host::TickBudgetLimits;
using extension_host::TickBudgetSample;
using extension_host::TickVerdict;

constexpr char kImportModule[] = "rgbx_mvp";
constexpr char kFillImport[] = "fill";
constexpr char kTickExport[] = "rgbx_tick";
constexpr uint32_t kMaxFillCallsPerTick = 1;
constexpr uint32_t kMaxFunctions = 8;
constexpr uint16_t kMaxLocalsPerFunction = 32;
constexpr int kPollMs = MAX(1, MIN(CONFIG_APP_WASM3_MVP_CPU_BUDGET_MS, 10));

enum class SharedStatus : uint32_t {
    Empty,
    Ready,
    EnvironmentAllocationFailed,
    RuntimeAllocationFailed,
    ParseFailed,
    PolicyRejected,
    LoadFailed,
    LinkFailed,
    CompileFailed,
    ExportLookupFailed,
    TickCompleted,
    TickTrapped,
    HostImportRejected,
};

enum class RequestKind : uint32_t {
    Tick,
#if defined(CONFIG_ZTEST)
    MemoryFault,
#endif
};

struct SharedMailbox {
    uint8_t module[CONFIG_APP_WASM3_MVP_MODULE_MAX_SIZE];
    size_t moduleSize;
    uint32_t requestElapsedMs;
    uint32_t requestGeneration;
    uint32_t completedGeneration;
    uint32_t pendingColor;
    uint32_t committedColor;
    uint32_t fillCallCount;
    SharedStatus status;
    RequestKind requestKind;
#if defined(CONFIG_ZTEST)
    uintptr_t faultAddress;
#endif
};

K_APP_BMEM(rgbx_wasm3_partition) SharedMailbox sShared;

K_THREAD_STACK_DEFINE(sSandboxStack, CONFIG_APP_WASM3_MVP_THREAD_STACK_SIZE);
struct k_thread sSandboxThread;
struct k_mem_domain sSandboxDomain;
K_SEM_DEFINE(sRequestSem, 0, 1);
K_SEM_DEFINE(sDoneSem, 0, 1);
#if defined(CONFIG_ZTEST)
K_SEM_DEFINE(sRequestStartedForTest, 0, 1);
#endif
K_MUTEX_DEFINE(sRuntimeLock);

bool sSandboxAlive = false;
bool sReady = false;
uint32_t sNextGeneration = 1;
size_t sPeakArenaHighWater = 0;
size_t sPeakThreadStackUsed = 0;
#if defined(CONFIG_ZTEST)
volatile uint32_t sKernelOnlyCanary = 0x13579bdf;
#endif

BUILD_ASSERT(CONFIG_APP_WASM3_MVP_THREAD_PRIORITY > CONFIG_APP_PATTERN_CONTROLLER_THREAD_PRIORITY,
             "the Wasm sandbox must rank below pattern_controller_thread");
BUILD_ASSERT(CONFIG_APP_WASM3_MVP_THREAD_PRIORITY >= 0 &&
                 CONFIG_APP_WASM3_MVP_THREAD_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES,
             "CONFIG_APP_WASM3_MVP_THREAD_PRIORITY must be preemptible");
BUILD_ASSERT(CONFIG_APP_WASM3_MVP_WALL_BACKSTOP_MS > CONFIG_APP_WASM3_MVP_CPU_BUDGET_MS,
             "the Wasm wall backstop must exceed its CPU budget");
#if !defined(CONFIG_SCHED_THREAD_USAGE)
#error "wasm_mvp_runtime requires CONFIG_SCHED_THREAD_USAGE for CPU accounting"
#endif
#if defined(CONFIG_THREAD_RUNTIME_STATS_USE_TIMING_FUNCTIONS)
#error "wasm_mvp_runtime CPU accounting requires k_cycle_get_32() clock-domain stats"
#endif

struct RuntimeLockGuard {
    RuntimeLockGuard() { k_mutex_lock(&sRuntimeLock, K_FOREVER); }
    ~RuntimeLockGuard() { k_mutex_unlock(&sRuntimeLock); }
};

void recordMemoryHighWater() {
    const size_t arena = m3_GetFixedHeapHighWater();
    if (arena > sPeakArenaHighWater) {
        sPeakArenaHighWater = arena;
    }

    size_t unused = 0;
    if (sSandboxAlive && k_thread_stack_space_get(&sSandboxThread, &unused) == 0) {
        const size_t stackSize = K_THREAD_STACK_SIZEOF(sSandboxStack);
        const size_t used = unused < stackSize ? stackSize - unused : 0;
        if (used > sPeakThreadStackUsed) {
            sPeakThreadStackUsed = used;
        }
    }
}

uint64_t sandboxExecutionCycles() {
    k_thread_runtime_stats_t stats;
    if (!sSandboxAlive || k_thread_runtime_stats_get(&sSandboxThread, &stats) != 0) {
        return 0;
    }
    return stats.execution_cycles;
}

bool sandboxThreadTerminated() {
    return sSandboxAlive && k_thread_join(&sSandboxThread, K_NO_WAIT) == 0;
}

enum class HandshakeAction : uint8_t {
    StartThread,
    PostRequest,
};

TickVerdict waitForSandbox(k_timepoint_t deadline, HandshakeAction action, uint32_t& cpuCycles,
                           uint32_t& wallCycles) {
    const TickBudgetLimits limits{k_ms_to_cyc_ceil64(CONFIG_APP_WASM3_MVP_CPU_BUDGET_MS)};
    const uint32_t startCycles = k_cycle_get_32();
    const uint64_t startExecution = sandboxExecutionCycles();

    if (action == HandshakeAction::StartThread) {
        k_thread_start(&sSandboxThread);
    } else {
        k_sem_give(&sRequestSem);
    }

    TickBudgetSample sample;
    TickVerdict verdict;
    do {
        sample.completed = k_sem_take(&sDoneSem, K_MSEC(kPollMs)) == 0;
        const uint64_t executionNow = sandboxExecutionCycles();
        sample.cpuCyc = executionNow > startExecution ? executionNow - startExecution : 0;
        sample.sandboxDied = sandboxThreadTerminated();
        sample.wallDeadlineExpired = sys_timepoint_expired(deadline);
        verdict = evaluate_tick_budget(sample, limits);
    } while (verdict == TickVerdict::Running);

    cpuCycles = static_cast<uint32_t>(sample.cpuCyc);
    wallCycles = k_cycle_get_32() - startCycles;
    return verdict;
}

Result resultFromVerdict(TickVerdict verdict) {
    switch (verdict) {
        case TickVerdict::Completed:
            return Result::Completed;
        case TickVerdict::CpuBudgetExceeded:
            return Result::CpuBudgetExceeded;
        case TickVerdict::SandboxDied:
            return Result::SandboxDied;
        case TickVerdict::WallBackstopExceeded:
            return Result::WallBackstopExceeded;
        default:
            return Result::RuntimeFailure;
    }
}

Result resultFromSharedStatus(SharedStatus status) {
    switch (status) {
        case SharedStatus::Ready:
        case SharedStatus::TickCompleted:
            return Result::Completed;
        case SharedStatus::ParseFailed:
        case SharedStatus::PolicyRejected:
        case SharedStatus::LoadFailed:
        case SharedStatus::LinkFailed:
        case SharedStatus::CompileFailed:
        case SharedStatus::ExportLookupFailed:
            return Result::InvalidModule;
        case SharedStatus::TickTrapped:
        case SharedStatus::HostImportRejected:
            return Result::Trap;
        default:
            return Result::RuntimeFailure;
    }
}

bool modulePolicyAllows(IM3Module module) {
    if (module->numFuncImports != 1 || module->numFunctions > kMaxFunctions ||
        module->startFunction >= 0 || module->hasTable || module->memoryDeclared ||
        module->memoryImported || module->numDataSegments != 0 || module->numElementSegments != 0) {
        return false;
    }

    for (uint32_t i = 0; i < module->numGlobals; ++i) {
        if (module->globals[i].imported) {
            return false;
        }
    }

    const M3ImportInfo& import = module->functions[0].import;
    return import.moduleUtf8 != nullptr && import.fieldUtf8 != nullptr &&
           std::strcmp(import.moduleUtf8, kImportModule) == 0 &&
           std::strcmp(import.fieldUtf8, kFillImport) == 0;
}

bool compiledModulePolicyAllows(IM3Module module) {
    for (uint32_t i = module->numFuncImports; i < module->numFunctions; ++i) {
        if (module->functions[i].numLocals > kMaxLocalsPerFunction) {
            return false;
        }
    }
    return true;
}

void stopLocked() {
    sReady = false;
    if (sSandboxAlive) {
        recordMemoryHighWater();
        if (k_thread_join(&sSandboxThread, K_NO_WAIT) != 0) {
            k_thread_abort(&sSandboxThread);
        }
        (void)k_thread_join(&sSandboxThread, K_FOREVER);
        sSandboxAlive = false;
    }

    // No Wasm3 handle crosses the privilege boundary. After termination the
    // supervisor invalidates the mailbox and bulk-resets the entire arena.
    std::memset(&sShared, 0, sizeof(sShared));
    m3_ResetFixedHeap();
    k_sem_reset(&sRequestSem);
    k_sem_reset(&sDoneSem);
#if defined(CONFIG_ZTEST)
    k_sem_reset(&sRequestStartedForTest);
#endif
}

const void* fillHost(IM3Runtime runtime, IM3ImportContext context, uint64_t* stack, void* memory) {
    uint64_t* _sp = stack;
    void* _mem = memory;
    IM3ImportContext _ctx = context;
    m3ApiGetArg(uint32_t, color);
    (void)runtime;
    (void)_mem;

    auto* shared = static_cast<SharedMailbox*>(const_cast<void*>(_ctx->userdata));
    if (shared == nullptr || shared->requestGeneration == 0 ||
        ++shared->fillCallCount > kMaxFillCallsPerTick) {
        if (shared != nullptr) {
            shared->status = SharedStatus::HostImportRejected;
        }
        return m3Err_trapAbort;
    }

    shared->pendingColor = color;
    m3ApiSuccess();
}

void sandboxEntry(void* p1, void* p2, void* p3) {
    auto* shared = static_cast<SharedMailbox*>(p1);
    (void)p2;
    (void)p3;

    IM3Environment environment = m3_NewEnvironment();
    if (environment == nullptr) {
        shared->status = SharedStatus::EnvironmentAllocationFailed;
        k_sem_give(&sDoneSem);
        return;
    }

    IM3Runtime runtime = m3_NewRuntime(environment, CONFIG_APP_WASM3_MVP_VALUE_STACK_SIZE, shared);
    if (runtime == nullptr) {
        shared->status = SharedStatus::RuntimeAllocationFailed;
        k_sem_give(&sDoneSem);
        return;
    }

    IM3Module module = nullptr;
    M3Result result = m3_ParseModule(environment, &module, shared->module, shared->moduleSize);
    if (result != m3Err_none) {
        shared->status = SharedStatus::ParseFailed;
        k_sem_give(&sDoneSem);
        return;
    }

    // Inspect before m3_LoadModule(), which may execute a start function. The
    // embedded-only profile has one exact import and no memory, table, data,
    // element, or implicit-start surface.
    if (!modulePolicyAllows(module)) {
        shared->status = SharedStatus::PolicyRejected;
        k_sem_give(&sDoneSem);
        return;
    }

    result = m3_LoadModule(runtime, module);
    if (result != m3Err_none) {
        shared->status = SharedStatus::LoadFailed;
        k_sem_give(&sDoneSem);
        return;
    }

    result = m3_LinkRawFunctionEx(module, kImportModule, kFillImport, "v(i)", fillHost, shared);
    if (result != m3Err_none) {
        shared->status = SharedStatus::LinkFailed;
        k_sem_give(&sDoneSem);
        return;
    }

    result = m3_CompileModule(module);
    if (result != m3Err_none) {
        shared->status = SharedStatus::CompileFailed;
        k_sem_give(&sDoneSem);
        return;
    }
    if (!compiledModulePolicyAllows(module)) {
        shared->status = SharedStatus::PolicyRejected;
        k_sem_give(&sDoneSem);
        return;
    }

    IM3Function tickFunction = nullptr;
    result = m3_FindFunction(&tickFunction, runtime, kTickExport);
    if (result != m3Err_none || m3_GetArgCount(tickFunction) != 1 ||
        m3_GetArgType(tickFunction, 0) != c_m3Type_i32 || m3_GetRetCount(tickFunction) != 0) {
        shared->status = SharedStatus::ExportLookupFailed;
        k_sem_give(&sDoneSem);
        return;
    }

    shared->status = SharedStatus::Ready;
    k_sem_give(&sDoneSem);

    while (true) {
        k_sem_take(&sRequestSem, K_FOREVER);
#if defined(CONFIG_ZTEST)
        k_sem_give(&sRequestStartedForTest);
        if (shared->requestKind == RequestKind::MemoryFault) {
            // The supervisor supplies an address in kernel-owned application
            // data, outside the sandbox domain. This read must raise an MPU
            // fault before the done semaphore can be signalled.
            const auto* forbidden =
                reinterpret_cast<volatile const uint32_t*>(shared->faultAddress);
            volatile uint32_t value = *forbidden;
            (void)value;
            k_sem_give(&sDoneSem);
            continue;
        }
#endif
        const uint32_t generation = shared->requestGeneration;
        shared->fillCallCount = 0;
        shared->pendingColor = 0;
        shared->status = SharedStatus::Empty;

        result = m3_CallV(tickFunction, shared->requestElapsedMs);
        if (result == m3Err_none && shared->fillCallCount == 1) {
            shared->committedColor = shared->pendingColor;
            shared->completedGeneration = generation;
            shared->status = SharedStatus::TickCompleted;
        } else if (shared->status != SharedStatus::HostImportRejected) {
            shared->status = SharedStatus::TickTrapped;
        }
        k_sem_give(&sDoneSem);
    }
}

}  // namespace

Result start(const uint8_t* module, size_t moduleSize, k_timepoint_t deadline) {
    RuntimeLockGuard lock;
    stopLocked();

    if (module == nullptr || moduleSize == 0 || moduleSize > sizeof(sShared.module)) {
        return Result::InvalidModule;
    }
    std::memcpy(sShared.module, module, moduleSize);
    sShared.moduleSize = moduleSize;

    struct k_mem_partition* partitions[] = {&rgbx_wasm3_partition, &z_libc_partition};
    int ret = k_mem_domain_init(&sSandboxDomain, ARRAY_SIZE(partitions), partitions);
    if (ret != 0) {
        LOG_ERR("Wasm sandbox domain init failed: %d", ret);
        stopLocked();
        return Result::RuntimeFailure;
    }

    k_tid_t tid = k_thread_create(
        &sSandboxThread, sSandboxStack, K_THREAD_STACK_SIZEOF(sSandboxStack), sandboxEntry,
        &sShared, nullptr, nullptr, CONFIG_APP_WASM3_MVP_THREAD_PRIORITY, K_USER, K_FOREVER);
    k_thread_name_set(tid, "wasm_sandbox");
#if defined(CONFIG_ZTEST)
    k_thread_access_grant(tid, &sRequestSem, &sDoneSem, &sRequestStartedForTest);
#else
    k_thread_access_grant(tid, &sRequestSem, &sDoneSem);
#endif
    ret = k_mem_domain_add_thread(&sSandboxDomain, tid);
    if (ret != 0) {
        LOG_ERR("Wasm sandbox domain attach failed: %d", ret);
        k_thread_abort(tid);
        (void)k_thread_join(tid, K_FOREVER);
        stopLocked();
        return Result::RuntimeFailure;
    }

    // Set before start so an immediate user-mode fault is recognized by the
    // lock-free fatal handler and by the supervisor's death check.
    sSandboxAlive = true;
    uint32_t cpuCycles = 0;
    uint32_t wallCycles = 0;
    const TickVerdict verdict =
        waitForSandbox(deadline, HandshakeAction::StartThread, cpuCycles, wallCycles);
    recordMemoryHighWater();

    Result result = resultFromVerdict(verdict);
    if (result == Result::Completed) {
        result = resultFromSharedStatus(sShared.status);
    }
    if (result != Result::Completed || sShared.status != SharedStatus::Ready) {
        LOG_ERR("Wasm sandbox admission failed: %s (cpu %u us, wall %u us)", describe(result),
                k_cyc_to_us_near32(cpuCycles), k_cyc_to_us_near32(wallCycles));
        stopLocked();
        return result;
    }

    sReady = true;
    static bool readinessReported = false;
    if (!readinessReported) {
        readinessReported = true;
        LOG_INF("Wasm sandbox ready: module %zu B, arena %zu / %u B, stack peak %zu / %zu B",
                moduleSize, m3_GetFixedHeapHighWater(),
                static_cast<unsigned int>(CONFIG_APP_WASM3_MVP_HEAP_SIZE), sPeakThreadStackUsed,
                K_THREAD_STACK_SIZEOF(sSandboxStack));
    }
    return Result::Completed;
}

Result tick(uint32_t elapsedMs, k_timepoint_t deadline, TickOutput& output) {
    RuntimeLockGuard lock;
    if (!sReady || !sSandboxAlive) {
        return Result::NotReady;
    }

    uint32_t generation = sNextGeneration++;
    if (generation == 0) {
        generation = sNextGeneration++;
    }
    sShared.requestElapsedMs = elapsedMs;
    sShared.requestGeneration = generation;
    sShared.completedGeneration = 0;
    sShared.committedColor = 0;
    sShared.status = SharedStatus::Empty;
    sShared.requestKind = RequestKind::Tick;

    uint32_t cpuCycles = 0;
    uint32_t wallCycles = 0;
    const TickVerdict verdict =
        waitForSandbox(deadline, HandshakeAction::PostRequest, cpuCycles, wallCycles);
    recordMemoryHighWater();

    Result result = resultFromVerdict(verdict);
    if (result == Result::Completed) {
        result = resultFromSharedStatus(sShared.status);
    }
    if (result == Result::Completed && sShared.completedGeneration == generation &&
        sShared.status == SharedStatus::TickCompleted) {
        output.color = sShared.committedColor;
        output.arenaHighWater = m3_GetFixedHeapHighWater();
        return Result::Completed;
    }

    if (result == Result::Completed) {
        result = Result::Trap;
    }
    LOG_ERR("Wasm sandbox tick failed: %s (cpu %u us, wall %u us)", describe(result),
            k_cyc_to_us_near32(cpuCycles), k_cyc_to_us_near32(wallCycles));
    stopLocked();
    return result;
}

void stop() {
    RuntimeLockGuard lock;
    stopLocked();
}

bool isCurrentSandboxThread() {
    return k_current_get() == &sSandboxThread;
}

size_t peakArenaHighWater() {
    return sPeakArenaHighWater;
}

const char* describe(Result result) {
    switch (result) {
        case Result::Completed:
            return "completed";
        case Result::NotReady:
            return "not ready";
        case Result::InvalidModule:
            return "module admission failed";
        case Result::RuntimeFailure:
            return "runtime setup failed";
        case Result::Trap:
            return "guest trapped or produced invalid output";
        case Result::CpuBudgetExceeded:
            return "CPU budget exceeded";
        case Result::SandboxDied:
            return "sandbox thread died";
        case Result::WallBackstopExceeded:
            return "wall backstop exceeded";
    }
    return "unknown";
}

#if defined(CONFIG_ZTEST)
Result triggerMemoryFaultForTest(k_timepoint_t deadline) {
    RuntimeLockGuard lock;
    if (!sReady || !sSandboxAlive) {
        return Result::NotReady;
    }

    sShared.requestKind = RequestKind::MemoryFault;
    sShared.faultAddress = reinterpret_cast<uintptr_t>(&sKernelOnlyCanary);
    uint32_t cpuCycles = 0;
    uint32_t wallCycles = 0;
    const TickVerdict verdict =
        waitForSandbox(deadline, HandshakeAction::PostRequest, cpuCycles, wallCycles);
    const Result result = resultFromVerdict(verdict);
    stopLocked();
    return result;
}

bool waitForRequestStartForTest(k_timeout_t timeout) {
    return k_sem_take(&sRequestStartedForTest, timeout) == 0;
}
#endif

}  // namespace wasm_mvp_runtime
