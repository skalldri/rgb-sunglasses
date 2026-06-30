#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Erase the entire NVS settings partition. Returns 0 on success, negative errno on failure. */
int storage_erase_settings_partition(void);

#ifdef __cplusplus
}
#endif
