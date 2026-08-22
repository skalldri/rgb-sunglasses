#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Project-owned lifecycle for Wasm3's fixed bump arena. The supervisor may
 * call reset only after the sandbox thread has terminated. Forced-abort paths
 * deliberately do not traverse interpreter objects or call Wasm3 destructors.
 */
void m3_ResetFixedHeap(void);
size_t m3_GetFixedHeapHighWater(void);

#ifdef __cplusplus
}
#endif
