#include <bluetooth/boot_gate.h>
#include <zephyr/kernel.h>

namespace {
K_SEM_DEFINE(sBootReadySem, 0, 1);
}

bool boot_gate_wait_ready(int timeout_ms) {
    return k_sem_take(&sBootReadySem, K_MSEC(timeout_ms)) == 0;
}

void boot_gate_notify_ready() {
    k_sem_give(&sBootReadySem);
}
