/*
 * Copyright (c) 2024 Nordic Semiconductor
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_USBD_MSC_CLASS)
#include <zephyr/usb/class/usbd_msc.h>
#endif

LOG_MODULE_REGISTER(usb_init, LOG_LEVEL_INF);

/*
 * Placeholder VID/PID — replace with a properly registered VID/PID before
 * shipping production firmware. 0x2fe3 is the Zephyr project VID and must
 * not be used outside of Zephyr samples.
 */
#define RGB_SUNGLASSES_USB_VID  0x2fe3
#define RGB_SUNGLASSES_USB_PID  0x0001

USBD_DEVICE_DEFINE(rgb_sunglasses_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   RGB_SUNGLASSES_USB_VID, RGB_SUNGLASSES_USB_PID);

USBD_DESC_LANG_DEFINE(usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(usb_mfr, "Stuart Alldritt");
USBD_DESC_PRODUCT_DEFINE(usb_product, "RGB Sunglasses");

#if IS_ENABLED(CONFIG_HWINFO)
USBD_DESC_SERIAL_NUMBER_DEFINE(usb_sn);
#endif

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");

/* Bus-powered; bMaxPower = 250 × 2 mA = 500 mA (FS USB maximum). */
USBD_CONFIGURATION_DEFINE(rgb_fs_config, 0, 250, &fs_cfg_desc);

#if IS_ENABLED(CONFIG_USBD_MSC_CLASS)
/* Expose the "NAND" flash-disk (MX25R6435F fat_partition) as MSC LUN 0.
 * The disk name must match the msc_disk0 DTS node's disk-name property. */
USBD_DEFINE_MSC_LUN(nand, "NAND", "RGB-SG", "FlashDisk", "0.00");
#endif

static int usb_init(void)
{
	int err;

	err = usbd_add_descriptor(&rgb_sunglasses_usbd, &usb_lang);
	if (err) {
		LOG_ERR("Failed to add language descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&rgb_sunglasses_usbd, &usb_mfr);
	if (err) {
		LOG_ERR("Failed to add manufacturer descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&rgb_sunglasses_usbd, &usb_product);
	if (err) {
		LOG_ERR("Failed to add product descriptor (%d)", err);
		return err;
	}

#if IS_ENABLED(CONFIG_HWINFO)
	err = usbd_add_descriptor(&rgb_sunglasses_usbd, &usb_sn);
	if (err) {
		LOG_ERR("Failed to add serial number descriptor (%d)", err);
		return err;
	}
#endif

	err = usbd_add_configuration(&rgb_sunglasses_usbd, USBD_SPEED_FS, &rgb_fs_config);
	if (err) {
		LOG_ERR("Failed to add FS configuration (%d)", err);
		return err;
	}

	/* cdc_acm_0: console and shell backend (zephyr,console / zephyr,shell-uart in DTS) */
	err = usbd_register_class(&rgb_sunglasses_usbd, "cdc_acm_0", USBD_SPEED_FS, 1);
	if (err) {
		LOG_ERR("Failed to register cdc_acm_0 (%d)", err);
		return err;
	}

	/* cdc_acm_1: MCUmgr UART transport (zephyr,uart-mcumgr in DTS) */
	err = usbd_register_class(&rgb_sunglasses_usbd, "cdc_acm_1", USBD_SPEED_FS, 1);
	if (err) {
		LOG_ERR("Failed to register cdc_acm_1 (%d)", err);
		return err;
	}

#if IS_ENABLED(CONFIG_USBD_MSC_CLASS)
	/* msc_0: NAND flash disk exposed as USB Mass Storage */
	err = usbd_register_class(&rgb_sunglasses_usbd, "msc_0", USBD_SPEED_FS, 1);
	if (err) {
		LOG_ERR("Failed to register msc_0 (%d)", err);
		return err;
	}
#endif

	/* Multiple CDC ACM interfaces require an Interface Association Descriptor;
	 * set device class to Miscellaneous / IAD to signal this to the host. */
	err = usbd_device_set_code_triple(&rgb_sunglasses_usbd, USBD_SPEED_FS,
					  USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	if (err) {
		LOG_ERR("Failed to set device class triple (%d)", err);
		return err;
	}

	err = usbd_init(&rgb_sunglasses_usbd);
	if (err) {
		LOG_ERR("Failed to initialize USB device support (%d)", err);
		return err;
	}

	err = usbd_enable(&rgb_sunglasses_usbd);
	if (err) {
		LOG_ERR("Failed to enable USB device (%d)", err);
		return err;
	}

	LOG_INF("USB device enabled");
	return 0;
}

/* Run after the FAT storage mount at APPLICATION priority 90 so the MSC disk
 * is valid before the host can access it. Must be a literal per SYS_INIT rules. */
#define USB_INIT_PRIORITY 91
SYS_INIT(usb_init, APPLICATION, USB_INIT_PRIORITY);
