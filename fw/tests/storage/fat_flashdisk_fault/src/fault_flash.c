/*
 * Test-only RAM-backed flash driver with injectable failures.
 *
 * Backs the fat_storage partition in this suite's native_sim overlay. Same
 * app-source-driver style the repo's emulator suites use — no Kconfig of its
 * own, capabilities are selected by the suite-local Kconfig (TEST_FAULT_FLASH).
 */

#define DT_DRV_COMPAT test_fault_flash

#include "fault_flash.h"

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>

#define BACKING_SIZE DT_INST_PROP(0, size)
#define ERASE_BLOCK  DT_INST_PROP(0, erase_block_size)
#define WRITE_BLOCK  DT_INST_PROP(0, write_block_size)

BUILD_ASSERT(BACKING_SIZE % ERASE_BLOCK == 0, "size must be a whole number of erase blocks");

static uint8_t backing[BACKING_SIZE];

static struct {
	unsigned int op_mask;
	unsigned int remaining;
	off_t win_start;
	size_t win_len;
	unsigned int injected;
} fault;

void fault_flash_arm(unsigned int op_mask, unsigned int count, off_t win_start, size_t win_len)
{
	fault.op_mask = op_mask;
	fault.remaining = count;
	fault.win_start = win_start;
	fault.win_len = win_len;
	fault.injected = 0;
}

void fault_flash_disarm(void)
{
	fault_flash_arm(0, 0, 0, 0);
}

unsigned int fault_flash_injected(void)
{
	return fault.injected;
}

static bool fault_hits(unsigned int op, off_t addr, size_t len)
{
	if ((fault.op_mask & op) == 0 || fault.remaining == 0) {
		return false;
	}
	if (fault.win_len != 0 &&
	    ((addr + (off_t)len <= fault.win_start) ||
	     (addr >= fault.win_start + (off_t)fault.win_len))) {
		return false;
	}
	fault.remaining--;
	fault.injected++;
	return true;
}

static bool in_bounds(off_t offset, size_t len)
{
	return offset >= 0 && len <= (size_t)BACKING_SIZE && (size_t)offset <= BACKING_SIZE - len;
}

static int fault_flash_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	ARG_UNUSED(dev);
	if (!in_bounds(offset, len)) {
		return -EINVAL;
	}
	if (fault_hits(FAULT_FLASH_OP_READ, offset, len)) {
		return -EIO;
	}
	memcpy(data, &backing[offset], len);
	return 0;
}

static int fault_flash_write(const struct device *dev, off_t offset, const void *data, size_t len)
{
	ARG_UNUSED(dev);
	if (!in_bounds(offset, len) || (offset % WRITE_BLOCK) != 0 || (len % WRITE_BLOCK) != 0) {
		return -EINVAL;
	}
	if (fault_hits(FAULT_FLASH_OP_WRITE, offset, len)) {
		return -EIO;
	}
	/* NOR programming can only clear bits: refuse to program un-erased
	 * media instead of silently overwriting like RAM would. This is what
	 * lets the suite catch a flashdisk_cache_commit that ever skips its
	 * erase (review finding on the first revision) — on real hardware that
	 * bug produces bitwise-AND garbage in FAT sectors, i.e. issue #380's
	 * corruption class, and a plain memcpy double could never see it. */
	for (size_t i = 0; i < len; i++) {
		const uint8_t want = ((const uint8_t *)data)[i];

		if ((backing[offset + i] & want) != want) {
			return -EIO;
		}
	}
	memcpy(&backing[offset], data, len);
	return 0;
}

static int fault_flash_erase(const struct device *dev, off_t offset, size_t size)
{
	ARG_UNUSED(dev);
	if (!in_bounds(offset, size) || (offset % ERASE_BLOCK) != 0 || (size % ERASE_BLOCK) != 0) {
		return -EINVAL;
	}
	if (fault_hits(FAULT_FLASH_OP_ERASE, offset, size)) {
		return -EIO;
	}
	/* Erase really blanks the page BEFORE a potential write fault is
	 * consumed by flashdisk_cache_commit's flash_write — reproducing the
	 * erase-OK/program-fail "page is 0xFF on media" hazard. */
	memset(&backing[offset], 0xFF, size);
	return 0;
}

static const struct flash_parameters fault_flash_parameters = {
	.write_block_size = WRITE_BLOCK,
	.erase_value = 0xFF,
};

static const struct flash_parameters *fault_flash_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);
	return &fault_flash_parameters;
}

static const struct flash_pages_layout fault_flash_pages_layout = {
	.pages_count = BACKING_SIZE / ERASE_BLOCK,
	.pages_size = ERASE_BLOCK,
};

static void fault_flash_page_layout(const struct device *dev,
				    const struct flash_pages_layout **layout, size_t *layout_size)
{
	ARG_UNUSED(dev);
	*layout = &fault_flash_pages_layout;
	*layout_size = 1;
}

static DEVICE_API(flash, fault_flash_api) = {
	.read = fault_flash_read,
	.write = fault_flash_write,
	.erase = fault_flash_erase,
	.get_parameters = fault_flash_get_parameters,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = fault_flash_page_layout,
#endif
};

static int fault_flash_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	memset(backing, 0xFF, sizeof(backing));
	return 0;
}

DEVICE_DT_INST_DEFINE(0, fault_flash_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_FLASH_INIT_PRIORITY, &fault_flash_api);
