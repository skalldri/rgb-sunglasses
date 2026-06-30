#ifndef PM_CONFIG_MOCK_H_
#define PM_CONFIG_MOCK_H_

/*
 * Mock partition-manager values for native_sim tests.
 * PM_MCUBOOT_SIZE must match the 'mcuboot' DTS partition in the overlay
 * and must be an exact multiple of the 4096-byte page size.
 */
#define PM_MCUBOOT_ADDRESS  0x00110000
#define PM_MCUBOOT_SIZE     0x00014000   /* 80 KB — 20 x 4096-byte pages */
#define PM_MCUBOOT_ID       0

#endif /* PM_CONFIG_MOCK_H_ */
