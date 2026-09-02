#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file
 * @brief Pure address-to-region logic for extension crash reports.
 *
 * Dependency-free by design (no Zephyr headers) so the native_sim suite
 * `extensions.fault_pc` can compile and exercise it on the host — same seam
 * as extension_tick_budget.h.
 *
 * A shipped `.llext` carries no DWARF (the SDK strips it and publishes the
 * unstripped partial link as `<name>.llext.debug` beside the release asset).
 * That sidecar is an `ld -r` relocatable, so its addresses are SECTION
 * RELATIVE: `arm-none-eabi-addr2line -j .text` wants "bytes from the start of
 * .text", not the runtime PC. The host knows the runtime base of every llext
 * region (`llext::mem[]`), so the conversion is one subtraction — but only
 * when the address actually lies inside one of the extension's regions. A
 * PC outside all of them is a firmware export (memcpy, sinf, printk), a
 * null-ish function pointer, or a return into the sandbox entry, and has to
 * be resolved against zephyr.elf instead. This header decides which case
 * applies and produces the offset; the host does the printing.
 */

namespace extension_host {

/** @brief Which extension memory region an address fell in.
 *
 *  The first four values mirror llext's `LLEXT_MEM_TEXT..LLEXT_MEM_BSS`
 *  enumerator order (extension_host.cpp static_asserts the equality), so a
 *  host can index `llext::mem[]` / `mem_size[]` with them directly. */
enum class FaultRegion : uint8_t {
    Text = 0,
    Data = 1,
    Rodata = 2,
    Bss = 3,
    /** Not inside any of the extension's regions. */
    Outside = 4,
};

/** @brief Number of region slots a `FaultRegionSpan` array carries. */
inline constexpr size_t kFaultRegionCount = 4;

/** @brief Runtime placement of one extension region. `size == 0` marks an
 *  absent region (an extension with no .bss, say) and never matches. */
struct FaultRegionSpan {
    uintptr_t base = 0;
    size_t size = 0;
};

/** @brief One resolved register value from a sandbox fault. */
struct FaultAddress {
    /** The raw register value. */
    uintptr_t addr = 0;
    /** Region containing `addr`, or Outside. */
    FaultRegion region = FaultRegion::Outside;
    /** `addr - base` of `region`; the number addr2line wants. Only meaningful
     *  when `region != Outside` (zero otherwise). */
    uintptr_t offset = 0;
};

/** @brief Resolves `addr` against the extension's regions.
 *
 *  Regions are checked in enumerator order; they are disjoint allocations so
 *  order cannot change the answer, it only fixes it. A half-open range is
 *  used (`base <= addr < base + size`): one past the end of .text is not in
 *  .text, exactly as `ext_region_remaining()` treats data pointers.
 *
 *  @param addr  Register value to resolve (a PC should already be even; a
 *               Thumb return address in LR should have bit 0 cleared by the
 *               caller, see `strip_thumb_bit()`).
 *  @param spans Region bases and sizes, indexed by `FaultRegion`.
 *  @return The classification and section-relative offset. */
constexpr FaultAddress locate_fault_address(uintptr_t addr,
                                            const FaultRegionSpan (&spans)[kFaultRegionCount]) {
    FaultAddress out;
    out.addr = addr;
    for (size_t r = 0; r < kFaultRegionCount; r++) {
        const FaultRegionSpan &s = spans[r];
        if (s.size != 0 && addr >= s.base && addr - s.base < s.size) {
            out.region = static_cast<FaultRegion>(r);
            out.offset = addr - s.base;
            return out;
        }
    }
    return out;
}

/** @brief Clears the Thumb interworking bit a Cortex-M leaves set in LR.
 *
 *  A return address in LR is `<next instruction> | 1`; the ELF symbol tables
 *  and DWARF line tables address the even instruction. The PC in the
 *  exception frame is already even, so this is only needed for LR.
 *
 *  @param lr Raw link-register value.
 *  @return `lr` with bit 0 cleared. */
constexpr uintptr_t strip_thumb_bit(uintptr_t lr) {
    return lr & ~static_cast<uintptr_t>(1);
}

/** @brief ELF section name for `addr2line -j`, e.g. ".text".
 *
 *  @param region A region value.
 *  @return The section name, or "?" for Outside. */
constexpr const char *fault_region_section(FaultRegion region) {
    switch (region) {
        case FaultRegion::Text:
            return ".text";
        case FaultRegion::Data:
            return ".data";
        case FaultRegion::Rodata:
            return ".rodata";
        case FaultRegion::Bss:
            return ".bss";
        case FaultRegion::Outside:
            break;
    }
    return "?";
}

/*
 * Zephyr fatal-error reason codes, restated numerically so this header stays
 * dependency-free. The generic ones are `enum k_fatal_error_reason`
 * (zephyr/fatal_types.h); the Cortex-M ones are `enum k_fatal_error_reason_arch`
 * (zephyr/arch/arm/arch.h), a plain sequential enum starting at
 * K_ERR_ARCH_START. Both are pinned to the SDK's definitions by static_asserts
 * in extension_host.cpp, so a renumbering upstream fails the proto0 build
 * rather than mislabelling a fault.
 */
inline constexpr unsigned int kReasonCpuException = 0;
inline constexpr unsigned int kReasonSpuriousIrq = 1;
inline constexpr unsigned int kReasonStackChkFail = 2;
inline constexpr unsigned int kReasonKernelOops = 3;
inline constexpr unsigned int kReasonKernelPanic = 4;
inline constexpr unsigned int kReasonArchStart = 16;
inline constexpr unsigned int kReasonArmMemGeneric = kReasonArchStart + 0;
inline constexpr unsigned int kReasonArmMemStacking = kReasonArchStart + 1;
inline constexpr unsigned int kReasonArmMemUnstacking = kReasonArchStart + 2;
inline constexpr unsigned int kReasonArmMemDataAccess = kReasonArchStart + 3;
inline constexpr unsigned int kReasonArmMemInstructionAccess = kReasonArchStart + 4;
inline constexpr unsigned int kReasonArmBusGeneric = kReasonArchStart + 6;
inline constexpr unsigned int kReasonArmBusPreciseDataBus = kReasonArchStart + 9;
inline constexpr unsigned int kReasonArmBusImpreciseDataBus = kReasonArchStart + 10;
inline constexpr unsigned int kReasonArmBusInstructionBus = kReasonArchStart + 11;
inline constexpr unsigned int kReasonArmUsageGeneric = kReasonArchStart + 13;
inline constexpr unsigned int kReasonArmUsageDiv0 = kReasonArchStart + 14;
inline constexpr unsigned int kReasonArmUsageUnalignedAccess = kReasonArchStart + 15;
inline constexpr unsigned int kReasonArmUsageStackOverflow = kReasonArchStart + 16;
inline constexpr unsigned int kReasonArmUsageNoCoprocessor = kReasonArchStart + 17;
inline constexpr unsigned int kReasonArmUsageUndefinedInstruction = kReasonArchStart + 20;

/** @brief Human-readable name for a Zephyr fatal-error reason.
 *
 *  Covers the generic kernel reasons plus the Cortex-M MemManage / BusFault /
 *  UsageFault sub-reasons an extension can plausibly trigger. Anything else
 *  reads as "arch-specific"; the numeric code is always printed beside the
 *  name so nothing is lost.
 *
 *  @param reason The `reason` argument Zephyr passed to
 *                `k_sys_fatal_error_handler()`.
 *  @return A string with static storage duration. */
constexpr const char *fault_reason_describe(unsigned int reason) {
    switch (reason) {
        case kReasonCpuException:
            return "CPU exception";
        case kReasonSpuriousIrq:
            return "unhandled interrupt";
        case kReasonStackChkFail:
            return "stack overflow (canary)";
        case kReasonKernelOops:
            return "kernel oops";
        case kReasonKernelPanic:
            return "kernel panic";
        case kReasonArmMemGeneric:
            return "MemManage fault";
        case kReasonArmMemStacking:
            return "MemManage fault: exception stacking";
        case kReasonArmMemUnstacking:
            return "MemManage fault: exception unstacking";
        case kReasonArmMemDataAccess:
            return "MemManage fault: data access outside the sandbox";
        case kReasonArmMemInstructionAccess:
            return "MemManage fault: instruction fetch outside the sandbox";
        case kReasonArmBusGeneric:
            return "BusFault";
        case kReasonArmBusPreciseDataBus:
            return "BusFault: precise data access";
        case kReasonArmBusImpreciseDataBus:
            return "BusFault: imprecise data access";
        case kReasonArmBusInstructionBus:
            return "BusFault: instruction fetch";
        case kReasonArmUsageGeneric:
            return "UsageFault";
        case kReasonArmUsageDiv0:
            return "UsageFault: divide by zero";
        case kReasonArmUsageUnalignedAccess:
            return "UsageFault: unaligned access";
        case kReasonArmUsageStackOverflow:
            return "UsageFault: stack overflow";
        case kReasonArmUsageNoCoprocessor:
            return "UsageFault: no coprocessor (FPU access)";
        case kReasonArmUsageUndefinedInstruction:
            return "UsageFault: undefined instruction";
        default:
            break;
    }
    return reason >= kReasonArchStart ? "arch-specific fault" : "unknown reason";
}

}  // namespace extension_host
