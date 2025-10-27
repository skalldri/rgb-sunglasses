#define DT_DRV_COMPAT other_net_core_sim_flash

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <bootutil/image.h>
#include <pm_config.h>

// COPIED FROM zephyr/subsys/mgmt/mcumgr/grp/img_mgmt/include/mgmt/mcumgr/grp/img_mgmt/img_mgmt_priv.h
#ifdef CONFIG_MCUBOOT_BOOTLOADER_USES_SHA512
#define IMAGE_TLV_SHA IMAGE_TLV_SHA512
#define IMAGE_SHA_LEN 64
#else
#define IMAGE_TLV_SHA IMAGE_TLV_SHA256
#define IMAGE_SHA_LEN 32
#endif

LOG_MODULE_REGISTER(net_core_sim_flash, CONFIG_NET_CORE_SIM_FLASH_LOG_LEVEL);

#define SOC_NV_FLASH_NODE DT_INST_CHILD(0, net_core_flash_sim_0)
#define FLASH_SIMULATOR_BASE_OFFSET DT_REG_ADDR(SOC_NV_FLASH_NODE)
#define FLASH_SIMULATOR_ERASE_UNIT DT_PROP(SOC_NV_FLASH_NODE, erase_block_size)
#define FLASH_SIMULATOR_PROG_UNIT DT_PROP(SOC_NV_FLASH_NODE, write_block_size)
#define FLASH_SIMULATOR_FLASH_SIZE DT_REG_SIZE(SOC_NV_FLASH_NODE)

#define FLASH_SIMULATOR_ERASE_VALUE \
    DT_PROP(DT_PARENT(SOC_NV_FLASH_NODE), erase_value)

#define FLASH_SIMULATOR_PAGE_COUNT (FLASH_SIMULATOR_FLASH_SIZE / \
                                    FLASH_SIMULATOR_ERASE_UNIT)

static DEVICE_API(flash, net_core_sim_flash_api);

static const struct flash_parameters net_core_sim_flash_parameters = {
    .write_block_size = FLASH_SIMULATOR_PROG_UNIT,
    .erase_value = FLASH_SIMULATOR_ERASE_VALUE,
    .caps = {
#if !defined(CONFIG_FLASH_SIMULATOR_EXPLICIT_ERASE)
        .no_explicit_erase = false,
#endif
    },
};

#if defined(CONFIG_MCUBOOT_BOOTLOADER_MODE_SWAP_USING_OFFSET)
#error "Doesn't work with this option enabled"
#endif

static int net_core_sim_flash_read(const struct device *dev, const off_t offset,
                                   void *data,
                                   const size_t len)
{
    ARG_UNUSED(dev);

    // Here we should communicate with the network core to read the image info.
    // For now, return bogus value.

    LOG_DBG("Simulated read at offset 0x%lx of length %zu", (long)offset, len);

    const off_t tlv_info_offset = PM_MCUBOOT_PAD_SIZE + PM_CPUNET_APP_SIZE;
    const off_t tlv_sha_offset = tlv_info_offset + sizeof(struct image_tlv_info);
    const off_t tlv_sha_data_offset = tlv_sha_offset + sizeof(struct image_tlv);

    if (offset == 0 && len == sizeof(struct image_header))
    {
        // They are trying to read the MCUboot image header. Lets give them one!
        struct image_header *hdr = (struct image_header *)data;

        hdr->ih_magic = IMAGE_MAGIC;
        hdr->ih_hdr_size = PM_MCUBOOT_PAD_SIZE;
        hdr->ih_load_addr = PM_MCUBOOT_PRIMARY_1_ADDRESS;
        hdr->ih_img_size = PM_CPUNET_APP_SIZE;
        hdr->ih_flags = 0;

        // TODO: Query the real version from the net core?
        hdr->ih_ver.iv_major = 0;
        hdr->ih_ver.iv_minor = 0;
        hdr->ih_ver.iv_revision = 0;
        hdr->ih_ver.iv_build_num = 0;

        return 0;
    }
    else if (offset == tlv_info_offset && len == sizeof(struct image_tlv_info))
    {
        // There will be 1 TLV, which is composed of an image_tlv header and the data payload, a SHA hash
        struct image_tlv_info *tlv_info = (struct image_tlv_info *)data;
        tlv_info->it_magic = IMAGE_TLV_INFO_MAGIC;
        tlv_info->it_tlv_tot = sizeof(struct image_tlv) + IMAGE_SHA_LEN;
        return 0;
    }
    // It will then try to read the single TLVs. We will only provide a single TLV, the SHA TLV
    else if (offset == tlv_sha_offset && len == sizeof(struct image_tlv))
    {
        struct image_tlv *tlv = (struct image_tlv *)data;
        tlv->it_type = IMAGE_TLV_SHA;
        tlv->it_len = IMAGE_SHA_LEN;
        return 0;
    }
    // Finally, it will try to read the SHA TLV data directly
    else if (offset == tlv_sha_data_offset && len == IMAGE_SHA_LEN)
    {
        memset(data, 0xAB, IMAGE_SHA_LEN); // Fake SHA data
        return 0;
    }
    // Due to a bug in MCUmgr's `img_mgmt_read_info()` function, it will try to interpret the SHA bytes as
    // a `struct image_tlv`. Handle that case by producing a fake TLV that
    else if (offset == tlv_sha_data_offset && len == sizeof(struct image_tlv))
    {
        struct image_tlv *tlv = (struct image_tlv *)data;
        tlv->it_type = 0x03;                                    // Not a real TLV, but also not the invalid 0xFF TLV
        tlv->it_len = IMAGE_SHA_LEN - sizeof(struct image_tlv); // This is how many bytes we need to fast-forward
        return 0;
    }

    return -ENOTSUP;
}

static int net_core_sim_flash_write(const struct device *dev, const off_t offset,
                                    const void *data, const size_t len)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(offset);
    ARG_UNUSED(data);
    ARG_UNUSED(len);

    // Modifying this simulated flash region is not supported
    return -ENOTSUP;
}

static int net_core_sim_flash_erase(const struct device *dev, const off_t offset,
                                    const size_t len)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(offset);
    ARG_UNUSED(len);

    // Modifying this simulated flash region is not supported
    return -ENOTSUP;
}

static int net_core_sim_flash_get_size(const struct device *dev, uint64_t *size)
{
    ARG_UNUSED(dev);

    *size = FLASH_SIMULATOR_FLASH_SIZE;

    return 0;
}

static const struct flash_parameters *
net_core_sim_flash_get_parameters(const struct device *dev)
{
    ARG_UNUSED(dev);

    return &net_core_sim_flash_parameters;
}

#ifdef CONFIG_FLASH_PAGE_LAYOUT
static const struct flash_pages_layout net_core_sim_flash_pages_layout = {
    .pages_count = FLASH_SIMULATOR_PAGE_COUNT,
    .pages_size = FLASH_SIMULATOR_ERASE_UNIT,
};

static void net_core_sim_flash_page_layout(const struct device *dev,
                                           const struct flash_pages_layout **layout,
                                           size_t *layout_size)
{
    *layout = &net_core_sim_flash_pages_layout;
    *layout_size = 1;
}
#endif

static DEVICE_API(flash, net_core_sim_flash_api) = {
    .read = net_core_sim_flash_read,
    .write = net_core_sim_flash_write,
    .erase = net_core_sim_flash_erase,
    .get_parameters = net_core_sim_flash_get_parameters,
    .get_size = net_core_sim_flash_get_size,
#ifdef CONFIG_FLASH_PAGE_LAYOUT
    .page_layout = net_core_sim_flash_page_layout,
#endif
};

static int net_core_sim_flash_init(const struct device *dev)
{
    return 0;
}

DEVICE_DT_INST_DEFINE(0, net_core_sim_flash_init, NULL,
                      NULL, NULL, POST_KERNEL, CONFIG_FLASH_INIT_PRIORITY,
                      &net_core_sim_flash_api);