/**
 * @file label_config.h
 * @brief Smart Label runtime configuration data model, SRS 0.3 §5.1.2 range and
 *        consistency validation, per-field getter/setter with change detection,
 *        and staged atomic Configuration Set commit (Tickets 1 & 3).
 */
#ifndef LABEL_CONFIG_H
#define LABEL_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"
#include "sl_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t changed_count;
  uint8_t changed_field_ids[APP_CFG_FIELD_ID_MAX]; /* Changed SRS IDs (ZSL-15) */
  bool    nvm_written;
  bool    configured;
} label_config_commit_result_t;

/** Clear all staged fields and reset the "field written" tracking mask. */
#define label_config_stage_reset() \
  label_config_stage_field(APP_CFG_STAGE_RESET_ID, NULL, 0U)

/**
 * Initialize runtime configuration from NVM3 (or app_config.h defaults if NVM3
 * holds no valid configuration) and clear the staging buffer. Requires
 * label_nvm_init() to have completed first.
 */
sl_status_t label_config_init(void);

/**
 * Pure range and cross-field consistency check per SRS 0.3 §5.1.2.
 * Safe to call at any time without mutating module state.
 */
sl_status_t label_config_validate(const zinergy_nvm3_config_t *cfg);

/**
 * Copy the currently active validated configuration snapshot.
 */
sl_status_t label_config_get(zinergy_nvm3_config_t *out_cfg,
                             bool *out_configured);

/**
 * Read a single configuration field by SRS ID (0x01..0x10, 0x12) from active
 * configuration. Reserved ID 0x11 and out-of-range IDs are rejected.
 */
sl_status_t label_config_get_field(app_cfg_field_id_t field_id,
                                   void *out_buf,
                                   size_t buf_size,
                                   size_t *out_len);

/**
 * Update a single field on the active configuration after full SRS §5.1.2
 * validation, persist on change, and report whether the field value changed.
 */
sl_status_t label_config_set_field(app_cfg_field_id_t field_id,
                                   const void *data,
                                   size_t len,
                                   bool *out_changed);

/**
 * Stage an incoming field write in RAM and mark its "field written" bit without
 * altering active or stored configuration. Pass APP_CFG_STAGE_RESET_ID (0) with
 * NULL/0 (or call label_config_stage_reset()) to clear staging state.
 */
sl_status_t label_config_stage_field(app_cfg_field_id_t field_id,
                                     const void *data,
                                     size_t len);

/**
 * Atomic Configuration Set commit:
 * 1. Verifies all core fields (0x01..0x0F) were staged.
 * 2. Preserves previous active values for unstaged optional customer arrays
 *    (0x10 Customer Description, 0x12 User Data).
 * 3. Validates the complete candidate snapshot per SRS 0.3 §5.1.2.
 * 4. Persists to NVM3 BEFORE applying to active configuration.
 * 5. Computes changed SRS field IDs (ZSL-15), applies active snapshot, and
 *    clears staging state. Any failure leaves active config untouched.
 */
sl_status_t label_config_commit(label_config_commit_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif /* LABEL_CONFIG_H */
