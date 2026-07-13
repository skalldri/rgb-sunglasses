#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

int tps25750_dump(const struct device *dev);

int tps25750_download_patch(const struct device *dev, const char *patch, uint32_t patchSize);

int tps25750_clear_dead_battery(const struct device *dev);

#if defined(CONFIG_TPS25750_INTERNAL_PATCH)
int tps25750_get_patch(const char **patch, size_t *patch_size);
#endif

/**
 * @brief Raw reads of the PD-contract-related host-interface registers.
 *
 * These are plain reads of the TPS25750's own unique-address registers (host
 * interface TRM SLVUC05A ch.2) — NOT bridged I2Cm transactions — so they are
 * cheap and safe to poll. Each returns the register payload with the leading
 * byte-count byte already stripped/validated.
 */

/** Read POWER_STATUS (0x3F, TRM Table 2-33) into @p raw (16-bit LE payload). */
int tps25750_read_power_status(const struct device *dev, uint16_t *raw);

/** Read PD_STATUS (0x40, TRM Table 2-35) into @p raw (32-bit LE payload). */
int tps25750_read_pd_status(const struct device *dev, uint32_t *raw);

/** Read ACTIVE_CONTRACT_PDO (0x34, TRM Table 2-29): bytes 1-4 = the active
 *  PDO as a 32-bit LE value. Reads 0 when there is no explicit PD contract
 *  (cleared on disconnect / Hard Reset / PR_Swap). */
int tps25750_read_active_contract_pdo(const struct device *dev, uint32_t *pdo);

/** Read ACTIVE_CONTRACT_RDO (0x35, TRM Table 2-31) as a 32-bit LE value. */
int tps25750_read_active_contract_rdo(const struct device *dev, uint32_t *rdo);

/**
 * @brief Read TX_SINK_CAPS (0x33, TRM Table 2-25) — the sink-capability PDO
 * list this port advertises (from the Application Customization bundle).
 *
 * @param dev      TPS25750 device pointer.
 * @param pdos     Output array of up to @p max_pdos PDO words (32-bit LE).
 * @param max_pdos Capacity of @p pdos (register holds at most 7).
 * @param num_pdos Output: numValidPDOs from the header byte (may exceed
 *                 @p max_pdos; only min(num, max) entries are written).
 * @return 0 on success, negative errno on failure.
 */
int tps25750_read_tx_sink_caps(const struct device *dev, uint32_t *pdos, size_t max_pdos,
                               uint8_t *num_pdos);

/** How the current input power budget was established. */
enum tps25750_power_source {
    TPS25750_PWR_NONE = 0,      /**< No connection / not sinking power */
    TPS25750_PWR_TYPEC_DEFAULT, /**< Type-C default current (USB 500mA class) */
    TPS25750_PWR_TYPEC_1A5,     /**< Type-C 1.5A advertisement */
    TPS25750_PWR_TYPEC_3A0,     /**< Type-C 3.0A advertisement */
    TPS25750_PWR_PD_CONTRACT,   /**< Explicit PD contract (see available_*) */
    TPS25750_PWR_UNKNOWN,       /**< Contract exists but PDO type not decoded
                                     (e.g. PPS/augmented) — treat budget
                                     conservatively */
};

/** Decoded "what can this input supply" summary. */
struct tps25750_pd_power_info {
    enum tps25750_power_source source;
    bool connected;        /**< POWER_STATUS.PowerConnection */
    bool sinking;          /**< true when the port partner powers us */
    uint32_t available_mv; /**< Contract voltage (fixed PDO bits 19:10 x 50mV)
                                or 5000 for Type-C-only budgets */
    uint32_t available_ma; /**< Contract max current (fixed PDO bits 9:0 x
                                10mA) or the Type-C tier current */
    uint32_t raw_pdo;      /**< ACTIVE_CONTRACT_PDO word, 0 if none */
    uint32_t raw_rdo;      /**< ACTIVE_CONTRACT_RDO word, 0 if none */
    uint16_t raw_power_status; /**< POWER_STATUS word (for debug surfaces) */
};

/**
 * @brief Summarize the negotiated input power budget.
 *
 * Decodes POWER_STATUS.TypeCCurrent; for an explicit PD contract, reads and
 * decodes ACTIVE_CONTRACT_PDO/RDO (fixed-supply PDOs only — a non-fixed
 * active PDO yields TPS25750_PWR_UNKNOWN with a conservative 5V/500mA
 * budget). ChargerDetectStatus is intentionally not decoded: the TRM marks it
 * unsupported on firmware TPS25750_F509.04.02 (Table 2-33 footnotes).
 *
 * @return 0 on success, negative errno on bus failure.
 */
int tps25750_get_pd_power_info(const struct device *dev, struct tps25750_pd_power_info *info);

#ifdef __cplusplus
};
#endif
