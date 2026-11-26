/**
 * @file label_nvm.h
 * @brief Persistent NVM3 storage for Smart Label configuration, extrema,
 *        coulomb/battery state, OT/UT state, and chunked charge history.
 *
 * Enforces 24 KiB NVM3 sizing, Flash log region non-overlap, header magic and
 * schema validation, boot fallback to defaults, and write-only-on-change wear
 * suppression. Main context only (never call from ISR).
 */
#ifndef LABEL_NVM_H
#define LABEL_NVM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"
#include "sl_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* User-domain NVM3 object keys (must remain in 0x00000 .. 0x0FFFF). */
#define LABEL_NVM_KEY_CONFIG              0x0001UL
#define LABEL_NVM_KEY_EXTREMA             0x0002UL
#define LABEL_NVM_KEY_BATTERY             0x0003UL
#define LABEL_NVM_KEY_OTUT                0x0004UL
#define LABEL_NVM_KEY_CHARGE_HIST_META    0x0005UL
#define LABEL_NVM_KEY_CHARGE_HIST_BASE    0x0010UL /* 0x0010 .. 0x001F (16 chunks) */

typedef enum {
  LABEL_NVM_STATE_EXTREMA = 0,
  LABEL_NVM_STATE_BATTERY,
  LABEL_NVM_STATE_OTUT,
  LABEL_NVM_STATE_CHARGE_HISTORY,
  LABEL_NVM_STATE_COUNT
} label_nvm_state_id_t;

typedef struct {
  uintptr_t log_flash_base;
  size_t    log_flash_size;
} label_nvm_region_cfg_t;

typedef struct {
  bool      initialized;
  uintptr_t nvm_flash_base;
  size_t    nvm_flash_size;
  uintptr_t log_flash_base;
  size_t    log_flash_size;
  size_t    live_objects;
  size_t    deleted_objects;
  size_t    available_memory;
  uint32_t  erase_count;
  bool      is_memory_low;
  bool      is_cache_low;
} label_nvm_status_t;

/**
 * Initialize default NVM3 storage and verify non-overlap against the Flash log
 * region. Pass NULL to use APP_LOG_FLASH_BASE / APP_LOG_FLASH_SIZE defaults.
 */
sl_status_t label_nvm_init(const label_nvm_region_cfg_t *log_region);

/**
 * Load configuration from NVM3. On missing, corrupt, or invalid stored data,
 * populates out_cfg with app_config.h defaults and sets *out_used_defaults=true.
 */
sl_status_t label_nvm_load_config(zinergy_nvm3_config_t *out_cfg,
                                  bool *out_configured,
                                  bool *out_used_defaults);

/**
 * Validate and persist configuration to NVM3 only if bytes changed vs. stored
 * state. Never writes invalid configurations.
 */
sl_status_t label_nvm_save_config(const zinergy_nvm3_config_t *cfg,
                                  bool configured,
                                  bool *out_written);

/**
 * Load runtime state (extrema, battery/coulomb, OT/UT, or 256-entry charge
 * history) with automatic fallback to safe zero/initial defaults on invalid or
 * absent NVM3 objects.
 */
sl_status_t label_nvm_load_state(label_nvm_state_id_t state_id,
                                 void *out_state,
                                 size_t state_size,
                                 bool *out_used_defaults);

/**
 * Persist runtime state to NVM3 only when changed. For charge history, diffs
 * each 16-entry chunk so unchanged chunks are never rewritten.
 */
sl_status_t label_nvm_save_state(label_nvm_state_id_t state_id,
                                 const void *state,
                                 size_t state_size,
                                 bool *out_written);

/**
 * Append one sample to the 256-entry charge history ring buffer in RAM and
 * persist only the single affected 16-entry NVM3 chunk + metadata header.
 */
sl_status_t label_nvm_append_charge_history(label_charge_history_t *history,
                                            const charge_history_entry_t *entry,
                                            bool *out_written);

/**
 * Run one non-blocking NVM3 user-repack check if needed and optionally return
 * live capacity, wear, and region diagnostics.
 */
sl_status_t label_nvm_service(label_nvm_status_t *out_status);

#ifdef __cplusplus
}
#endif

#endif /* LABEL_NVM_H */
