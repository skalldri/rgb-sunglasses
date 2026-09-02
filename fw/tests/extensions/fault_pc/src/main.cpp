/*
 * Unit tests for the extension crash-site resolution logic behind `ext faults`.
 *
 * A shipped .llext carries no DWARF; the release publishes `<name>.llext.debug`
 * (an `ld -r` relocatable) beside it, so the only address addr2line can use is
 * "bytes from the start of the section". These tests pin the conversion from a
 * runtime register value to that section-relative offset — and, just as
 * importantly, the refusal to invent an offset for an address that is not in
 * the extension at all.
 */

#include <extensions/extension_fault_pc.h>
#include <zephyr/ztest.h>

using extension_host::fault_reason_describe;
using extension_host::fault_region_section;
using extension_host::FaultAddress;
using extension_host::FaultRegion;
using extension_host::FaultRegionSpan;
using extension_host::kFaultRegionCount;
using extension_host::locate_fault_address;
using extension_host::strip_thumb_bit;

namespace {

/* A plausible llext heap layout: four disjoint regions, .bss last. Sizes are
 * deliberately unequal and not powers of two so an off-by-one can't hide. */
constexpr uintptr_t kTextBase = 0x2003'0000;
constexpr size_t kTextSize = 0x1234;
constexpr uintptr_t kDataBase = 0x2003'2000;
constexpr size_t kDataSize = 0x100;
constexpr uintptr_t kRodataBase = 0x2003'2200;
constexpr size_t kRodataSize = 0x3f0;
constexpr uintptr_t kBssBase = 0x2003'2800;
constexpr size_t kBssSize = 0x40;

constexpr FaultRegionSpan kSpans[kFaultRegionCount] = {
    {kTextBase, kTextSize},
    {kDataBase, kDataSize},
    {kRodataBase, kRodataSize},
    {kBssBase, kBssSize},
};

}  // namespace

ZTEST_SUITE(extension_fault_pc, NULL, NULL, NULL, NULL, NULL);

/* --- locate_fault_address ------------------------------------------------ */

ZTEST(extension_fault_pc, test_pc_inside_text_yields_text_offset) {
    /* The shape of a real report: rgbx_tick faulting partway into .text. */
    const FaultAddress a = locate_fault_address(kTextBase + 0x9d4, kSpans);
    zassert_equal(a.addr, kTextBase + 0x9d4, "raw value must be preserved");
    zassert_equal(a.region, FaultRegion::Text);
    zassert_equal(a.offset, 0x9d4u, "offset must be relative to the .text base");
}

ZTEST(extension_fault_pc, test_each_region_is_classified) {
    zassert_equal(locate_fault_address(kDataBase + 4, kSpans).region, FaultRegion::Data);
    zassert_equal(locate_fault_address(kDataBase + 4, kSpans).offset, 4u);
    zassert_equal(locate_fault_address(kRodataBase + 0x3ef, kSpans).region, FaultRegion::Rodata);
    zassert_equal(locate_fault_address(kRodataBase + 0x3ef, kSpans).offset, 0x3efu);
    zassert_equal(locate_fault_address(kBssBase, kSpans).region, FaultRegion::Bss);
    zassert_equal(locate_fault_address(kBssBase, kSpans).offset, 0u);
}

ZTEST(extension_fault_pc, test_region_start_is_inside) {
    const FaultAddress a = locate_fault_address(kTextBase, kSpans);
    zassert_equal(a.region, FaultRegion::Text, "base address is the first byte of the region");
    zassert_equal(a.offset, 0u);
}

ZTEST(extension_fault_pc, test_region_end_is_half_open) {
    /* One past the end is NOT in the region — the same convention as the host's
     * ext_region_remaining() for data pointers. */
    const FaultAddress last = locate_fault_address(kTextBase + kTextSize - 1, kSpans);
    zassert_equal(last.region, FaultRegion::Text);
    zassert_equal(last.offset, kTextSize - 1);

    const FaultAddress past = locate_fault_address(kTextBase + kTextSize, kSpans);
    zassert_equal(past.region, FaultRegion::Outside,
                  "one past the end of .text must not read as .text");
    zassert_equal(past.offset, 0u, "Outside carries no offset");
}

ZTEST(extension_fault_pc, test_address_below_every_region_is_outside) {
    const FaultAddress a = locate_fault_address(kTextBase - 2, kSpans);
    zassert_equal(a.region, FaultRegion::Outside);
    zassert_equal(a.addr, kTextBase - 2, "raw value is still reported for Outside");
    zassert_equal(a.offset, 0u);
}

ZTEST(extension_fault_pc, test_gap_between_regions_is_outside) {
    /* Between .data's end and .rodata's start. A firmware export living there
     * would be resolved against zephyr.elf, never the sidecar. */
    const FaultAddress a = locate_fault_address(kDataBase + kDataSize + 8, kSpans);
    zassert_equal(a.region, FaultRegion::Outside);
}

ZTEST(extension_fault_pc, test_firmware_address_is_outside) {
    /* A flash address (memcpy in the base image, say): nowhere near the heap. */
    const FaultAddress a = locate_fault_address(0x0002'4680, kSpans);
    zassert_equal(a.region, FaultRegion::Outside);
    zassert_equal(a.addr, 0x0002'4680u);
}

ZTEST(extension_fault_pc, test_zero_sized_region_never_matches) {
    /* An extension with no .bss: llext reports base = whatever, size = 0. The
     * base may even be a real address inside another region's allocation, so a
     * size-0 region must be skipped rather than matched at its base. */
    constexpr FaultRegionSpan spans[kFaultRegionCount] = {
        {kTextBase, kTextSize},
        {kDataBase, kDataSize},
        {kRodataBase, kRodataSize},
        {kTextBase + 0x10, 0},
    };
    const FaultAddress a = locate_fault_address(kTextBase + 0x10, spans);
    zassert_equal(a.region, FaultRegion::Text, "a size-0 .bss must not shadow .text");
    zassert_equal(a.offset, 0x10u);

    const FaultAddress b = locate_fault_address(kBssBase, spans);
    zassert_equal(b.region, FaultRegion::Outside, "no .bss means nothing is in .bss");
}

ZTEST(extension_fault_pc, test_null_address_is_outside) {
    /* A null function pointer called from the extension: PC 0 (or near it). */
    zassert_equal(locate_fault_address(0, kSpans).region, FaultRegion::Outside);
}

/* --- strip_thumb_bit ------------------------------------------------------ */

ZTEST(extension_fault_pc, test_strip_thumb_bit_clears_bit_zero) {
    zassert_equal(strip_thumb_bit(kTextBase + 0x9d5), kTextBase + 0x9d4);
    zassert_equal(strip_thumb_bit(kTextBase + 0x9d4), kTextBase + 0x9d4,
                  "an even address is left alone");
    zassert_equal(strip_thumb_bit(1), 0u);
}

ZTEST(extension_fault_pc, test_lr_resolves_after_stripping) {
    /* The real flow: LR carries the Thumb bit; strip, then locate. The bit
     * alone must not push a last-byte return address out of the region. */
    const uintptr_t lr = (kTextBase + kTextSize - 2) | 1;
    const FaultAddress a = locate_fault_address(strip_thumb_bit(lr), kSpans);
    zassert_equal(a.region, FaultRegion::Text);
    zassert_equal(a.offset, kTextSize - 2);
}

/* --- fault_region_section ------------------------------------------------- */

ZTEST(extension_fault_pc, test_region_section_names_match_addr2line_j) {
    zassert_str_equal(fault_region_section(FaultRegion::Text), ".text");
    zassert_str_equal(fault_region_section(FaultRegion::Data), ".data");
    zassert_str_equal(fault_region_section(FaultRegion::Rodata), ".rodata");
    zassert_str_equal(fault_region_section(FaultRegion::Bss), ".bss");
    zassert_str_equal(fault_region_section(FaultRegion::Outside), "?");
}

/* --- fault_reason_describe ------------------------------------------------ */

ZTEST(extension_fault_pc, test_generic_reasons_are_named) {
    zassert_str_equal(fault_reason_describe(extension_host::kReasonCpuException),
                      "CPU exception");
    zassert_str_equal(fault_reason_describe(extension_host::kReasonSpuriousIrq),
                      "unhandled interrupt");
    zassert_str_equal(fault_reason_describe(extension_host::kReasonStackChkFail),
                      "stack overflow (canary)");
    zassert_str_equal(fault_reason_describe(extension_host::kReasonKernelOops), "kernel oops");
    zassert_str_equal(fault_reason_describe(extension_host::kReasonKernelPanic), "kernel panic");
}

ZTEST(extension_fault_pc, test_cortex_m_reasons_are_named) {
    /* The ones an extension can plausibly trigger. Pinned by value as well as by
     * constant so a renumbering of the header's table shows up here too. */
    zassert_str_equal(fault_reason_describe(16), "MemManage fault");
    zassert_str_equal(fault_reason_describe(17), "MemManage fault: exception stacking");
    zassert_str_equal(fault_reason_describe(18), "MemManage fault: exception unstacking");
    zassert_str_equal(fault_reason_describe(19), "MemManage fault: data access outside the sandbox");
    zassert_str_equal(fault_reason_describe(20),
                      "MemManage fault: instruction fetch outside the sandbox");
    zassert_str_equal(fault_reason_describe(22), "BusFault");
    zassert_str_equal(fault_reason_describe(25), "BusFault: precise data access");
    zassert_str_equal(fault_reason_describe(26), "BusFault: imprecise data access");
    zassert_str_equal(fault_reason_describe(27), "BusFault: instruction fetch");
    zassert_str_equal(fault_reason_describe(29), "UsageFault");
    zassert_str_equal(fault_reason_describe(30), "UsageFault: divide by zero");
    zassert_str_equal(fault_reason_describe(31), "UsageFault: unaligned access");
    zassert_str_equal(fault_reason_describe(32), "UsageFault: stack overflow");
    zassert_str_equal(fault_reason_describe(33), "UsageFault: no coprocessor (FPU access)");
    zassert_str_equal(fault_reason_describe(36), "UsageFault: undefined instruction");
}

ZTEST(extension_fault_pc, test_unlisted_arch_reason_is_generic_but_arch) {
    /* K_ERR_ARM_MEM_FP_LAZY (21) and the illegal-EXC_RETURN / EPSR reasons are
     * real Cortex-M codes an extension cannot meaningfully cause; they still
     * read as arch-specific rather than "unknown". */
    zassert_str_equal(fault_reason_describe(21), "arch-specific fault");
    zassert_str_equal(fault_reason_describe(34), "arch-specific fault");
    zassert_str_equal(fault_reason_describe(35), "arch-specific fault");
    zassert_str_equal(fault_reason_describe(200), "arch-specific fault");
}

ZTEST(extension_fault_pc, test_unlisted_generic_reason_is_unknown) {
    /* 5..15 are unassigned in fatal_types.h. */
    zassert_str_equal(fault_reason_describe(5), "unknown reason");
    zassert_str_equal(fault_reason_describe(15), "unknown reason");
}

ZTEST(extension_fault_pc, test_resolution_is_constexpr) {
    /* The whole seam is usable at compile time — the same guarantee
     * extension_tick_budget.h gives, and what lets the host static_assert the
     * region order against llext's. */
    constexpr FaultAddress a = locate_fault_address(kTextBase + 0x10, kSpans);
    static_assert(a.region == FaultRegion::Text);
    static_assert(a.offset == 0x10);
    static_assert(strip_thumb_bit(0x21) == 0x20);
    zassert_true(true);
}
