/**
 * @file label_config.c
 * @brief Smart Label configuration data model, SRS 0.3 §5.1.2 validation,
 *        per-field change detection, and staged atomic commit (Tickets 1 & 3).
 */
#include "label_config.h"

#include <string.h>
#include "label_nvm.h"

/* -------------------------------------------------------------------------- */
/* Table-Driven Field Metadata                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
  app_cfg_field_id_t id;
  size_t             offset;
  size_t             size;
  bool               allow_short_write; /* True for 255B Customer Description */
} cfg_field_meta_t;

static const cfg_field_meta_t s_field_table[] = {
  { APP_CFG_FIELD_SAMPLE_INTERVAL,
    offsetof(zinergy_nvm3_config_t, sample_interval_s),
    sizeof(uint32_t), false },
  { APP_CFG_FIELD_OT_THRESHOLD,
    offsetof(zinergy_nvm3_config_t, ot_threshold_ck),
    sizeof(uint16_t), false },
  { APP_CFG_FIELD_UT_THRESHOLD,
    offsetof(zinergy_nvm3_config_t, ut_threshold_ck),
    sizeof(uint16_t), false },
  { APP_CFG_FIELD_QUALIFICATION_COUNT,
    offsetof(zinergy_nvm3_config_t, qualification_count),
    sizeof(uint8_t), false },
  { APP_CFG_FIELD_RTC,
    offsetof(zinergy_nvm3_config_t, rtc_unix_s),
    sizeof(uint64_t), false },
  { APP_CFG_FIELD_NORM_TO_LOW_THRESHOLD,
    offsetof(zinergy_nvm3_config_t, normal_to_low_threshold_uah),
    sizeof(uint16_t), false },
  { APP_CFG_FIELD_ADV_INTERVAL,
    offsetof(zinergy_nvm3_config_t, adv_interval_ms),
    sizeof(uint16_t), false },
  { APP_CFG_FIELD_TRAIN_DURATION,
    offsetof(zinergy_nvm3_config_t, adv_train_duration_s),
    sizeof(uint16_t), false },
  { APP_CFG_FIELD_TRAIN_PERIOD,
    offsetof(zinergy_nvm3_config_t, adv_train_period_s),
    sizeof(uint32_t), false },
  { APP_CFG_FIELD_LP_INTERVAL_FACTOR,
    offsetof(zinergy_nvm3_config_t, lp_interval_factor),
    sizeof(uint16_t), false },
  { APP_CFG_FIELD_LP_DURATION_FACTOR,
    offsetof(zinergy_nvm3_config_t, lp_duration_factor),
    sizeof(uint16_t), false },
  { APP_CFG_FIELD_LP_PERIOD_FACTOR,
    offsetof(zinergy_nvm3_config_t, lp_period_factor),
    sizeof(uint16_t), false },
  { APP_CFG_FIELD_MAX_TX_POWER,
    offsetof(zinergy_nvm3_config_t, max_tx_power_dbm10),
    sizeof(int16_t), false },
  { APP_CFG_FIELD_LP_POWER_REDUCTION,
    offsetof(zinergy_nvm3_config_t, low_power_reduction_dbm10),
    sizeof(uint16_t), false },
  { APP_CFG_FIELD_CRIT_POWER_REDUCTION,
    offsetof(zinergy_nvm3_config_t, critical_power_reduction_dbm10),
    sizeof(uint16_t), false },
  { APP_CFG_FIELD_CUSTOMER_DESC,
    offsetof(zinergy_nvm3_config_t, customer_description),
    APP_CFG_CUSTOM_DESC_SIZE, true },
  /* Note: 0x11 is reserved and intentionally absent from valid descriptors. */
  { APP_CFG_FIELD_USER_DATA,
    offsetof(zinergy_nvm3_config_t, user_data),
    APP_CFG_USER_DATA_SIZE, false },
};

#define CFG_VALID_FIELD_COUNT (sizeof(s_field_table) / sizeof(s_field_table[0]))

/* -------------------------------------------------------------------------- */
/* Module-Private State                                                       */
/* -------------------------------------------------------------------------- */

static struct {
  bool                  initialized;
  bool                  configured;
  zinergy_nvm3_config_t active;
  zinergy_nvm3_config_t staged;
  uint32_t              staged_mask;
} s_cfg_ctx;

/* -------------------------------------------------------------------------- */
/* Private Helpers                                                            */
/* -------------------------------------------------------------------------- */

static const cfg_field_meta_t *_find_field(app_cfg_field_id_t field_id)
{
  for (size_t i = 0U; i < CFG_VALID_FIELD_COUNT; i++) {
    if (s_field_table[i].id == field_id) {
      return &s_field_table[i];
    }
  }
  return NULL;
}

static void _copy_clean_snapshot(zinergy_nvm3_config_t *dst,
                                 const zinergy_nvm3_config_t *src)
{
  memset(dst, 0, sizeof(*dst));
  uint8_t *dst_bytes = (uint8_t *)dst;
  const uint8_t *src_bytes = (const uint8_t *)src;
  for (size_t i = 0U; i < CFG_VALID_FIELD_COUNT; i++) {
    memcpy(&dst_bytes[s_field_table[i].offset],
           &src_bytes[s_field_table[i].offset],
           s_field_table[i].size);
  }
}

static sl_status_t _write_field_slice(zinergy_nvm3_config_t *target,
                                      const cfg_field_meta_t *meta,
                                      const void *data,
                                      size_t len)
{
  if (meta->allow_short_write) {
    if (len > meta->size) {
      return SL_STATUS_INVALID_PARAMETER;
    }
    if ((len > 0U) && (data == NULL)) {
      return SL_STATUS_NULL_POINTER;
    }
    uint8_t *dst = (uint8_t *)target + meta->offset;
    memset(dst, 0, meta->size);
    if (len > 0U) {
      memcpy(dst, data, len);
    }
    return SL_STATUS_OK;
  }

  if (data == NULL) {
    return SL_STATUS_NULL_POINTER;
  }
  if (len != meta->size) {
    return SL_STATUS_INVALID_PARAMETER;
  }
  memcpy((uint8_t *)target + meta->offset, data, meta->size);
  return SL_STATUS_OK;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

sl_status_t label_config_init(void)
{
  zinergy_nvm3_config_t loaded;
  bool is_configured = false;
  sl_status_t st = label_nvm_load_config(&loaded, &is_configured, NULL);
  if (st != SL_STATUS_OK) {
    return st;
  }

  memset(&s_cfg_ctx, 0, sizeof(s_cfg_ctx));
  _copy_clean_snapshot(&s_cfg_ctx.active, &loaded);
  s_cfg_ctx.configured = is_configured;
  s_cfg_ctx.initialized = true;
  return SL_STATUS_OK;
}

sl_status_t label_config_validate(const zinergy_nvm3_config_t *cfg)
{
  if (cfg == NULL) {
    return SL_STATUS_NULL_POINTER;
  }

  /* 0x01: Sampling interval */
  if ((cfg->sample_interval_s < APP_CFG_SAMPLE_INTERVAL_MIN_S)
      || (cfg->sample_interval_s > APP_CFG_SAMPLE_INTERVAL_MAX_S)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  /* 0x02 & 0x03: OT and UT thresholds (1..65,534 cK, UT < OT) */
  if ((cfg->ot_threshold_ck < APP_CFG_TEMP_MIN_CK)
      || (cfg->ot_threshold_ck > APP_CFG_TEMP_MAX_CK)
      || (cfg->ut_threshold_ck < APP_CFG_TEMP_MIN_CK)
      || (cfg->ut_threshold_ck > APP_CFG_TEMP_MAX_CK)
      || (cfg->ut_threshold_ck >= cfg->ot_threshold_ck)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  /* 0x04: Qualification count (1..255) */
  if (cfg->qualification_count < APP_CFG_QUAL_COUNT_MIN) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  /* 0x05: RTC Unix seconds */
  if ((cfg->rtc_unix_s < APP_CFG_RTC_MIN_UNIX_S)
      || (cfg->rtc_unix_s > APP_CFG_RTC_MAX_UNIX_S)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  /* 0x06: Charge threshold */
  if ((cfg->normal_to_low_threshold_uah < APP_CFG_CHARGE_THR_MIN_UAH)
      || (cfg->normal_to_low_threshold_uah > APP_CFG_CHARGE_THR_MAX_UAH)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  /* 0x07, 0x08, 0x09: Advertising interval and train (train_period >= duration) */
  if ((cfg->adv_interval_ms < APP_CFG_ADV_INTERVAL_MIN_MS)
      || (cfg->adv_interval_ms > APP_CFG_ADV_INTERVAL_MAX_MS)
      || (cfg->adv_train_duration_s < APP_CFG_TRAIN_DURATION_MIN_S)
      || (cfg->adv_train_duration_s > APP_CFG_TRAIN_DURATION_MAX_S)
      || (cfg->adv_train_period_s < APP_CFG_TRAIN_PERIOD_MIN_S)
      || (cfg->adv_train_period_s > APP_CFG_TRAIN_PERIOD_MAX_S)
      || (cfg->adv_train_period_s < (uint32_t)cfg->adv_train_duration_s)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  /* 0x0A, 0x0B, 0x0C: Low-Power factors + scaled train period >= duration */
  if ((cfg->lp_interval_factor < APP_CFG_LP_FACTOR_MIN)
      || (cfg->lp_interval_factor > APP_CFG_LP_FACTOR_MAX)
      || (cfg->lp_duration_factor < APP_CFG_LP_FACTOR_MIN)
      || (cfg->lp_duration_factor > APP_CFG_LP_FACTOR_MAX)
      || (cfg->lp_period_factor < APP_CFG_LP_FACTOR_MIN)
      || (cfg->lp_period_factor > APP_CFG_LP_FACTOR_MAX)) {
    return SL_STATUS_INVALID_PARAMETER;
  }
  uint64_t lp_period = (uint64_t)cfg->adv_train_period_s * (uint64_t)cfg->lp_period_factor;
  uint64_t lp_duration = (uint64_t)cfg->adv_train_duration_s * (uint64_t)cfg->lp_duration_factor;
  if (lp_period < lp_duration) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  /* 0x0D, 0x0E, 0x0F: TX power <= +4 dBm (+40 dbm10) and power reductions */
  if ((cfg->max_tx_power_dbm10 < APP_CFG_MAX_TX_POWER_MIN_DBM10)
      || (cfg->max_tx_power_dbm10 > APP_CFG_MAX_TX_POWER_MAX_DBM10)
      || (cfg->low_power_reduction_dbm10 > APP_CFG_POWER_RED_MAX_DBM10)
      || (cfg->critical_power_reduction_dbm10 > APP_CFG_POWER_RED_MAX_DBM10)
      || (cfg->critical_power_reduction_dbm10 < cfg->low_power_reduction_dbm10)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  return SL_STATUS_OK;
}

sl_status_t label_config_get(zinergy_nvm3_config_t *out_cfg,
                             bool *out_configured)
{
  if (out_cfg == NULL) {
    return SL_STATUS_NULL_POINTER;
  }
  if (!s_cfg_ctx.initialized) {
    return SL_STATUS_NOT_INITIALIZED;
  }

  _copy_clean_snapshot(out_cfg, &s_cfg_ctx.active);
  if (out_configured != NULL) {
    *out_configured = s_cfg_ctx.configured;
  }
  return SL_STATUS_OK;
}

sl_status_t label_config_get_field(app_cfg_field_id_t field_id,
                                   void *out_buf,
                                   size_t buf_size,
                                   size_t *out_len)
{
  if (out_buf == NULL) {
    return SL_STATUS_NULL_POINTER;
  }
  if (!s_cfg_ctx.initialized) {
    return SL_STATUS_NOT_INITIALIZED;
  }

  const cfg_field_meta_t *meta = _find_field(field_id);
  if (meta == NULL) {
    return SL_STATUS_INVALID_PARAMETER;
  }
  if (buf_size < meta->size) {
    return SL_STATUS_WOULD_OVERFLOW;
  }

  memcpy(out_buf, (const uint8_t *)&s_cfg_ctx.active + meta->offset, meta->size);
  if (out_len != NULL) {
    *out_len = meta->size;
  }
  return SL_STATUS_OK;
}

sl_status_t label_config_set_field(app_cfg_field_id_t field_id,
                                   const void *data,
                                   size_t len,
                                   bool *out_changed)
{
  if (out_changed != NULL) {
    *out_changed = false;
  }
  if (!s_cfg_ctx.initialized) {
    return SL_STATUS_NOT_INITIALIZED;
  }

  const cfg_field_meta_t *meta = _find_field(field_id);
  if (meta == NULL) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  zinergy_nvm3_config_t candidate;
  _copy_clean_snapshot(&candidate, &s_cfg_ctx.active);
  sl_status_t st = _write_field_slice(&candidate, meta, data, len);
  if (st != SL_STATUS_OK) {
    return st;
  }

  st = label_config_validate(&candidate);
  if (st != SL_STATUS_OK) {
    return st;
  }

  const uint8_t *old_ptr = (const uint8_t *)&s_cfg_ctx.active + meta->offset;
  const uint8_t *new_ptr = (const uint8_t *)&candidate + meta->offset;
  bool changed = (memcmp(old_ptr, new_ptr, meta->size) != 0);

  if (changed) {
    st = label_nvm_save_config(&candidate, s_cfg_ctx.configured, NULL);
    if (st != SL_STATUS_OK) {
      return st;
    }
    _copy_clean_snapshot(&s_cfg_ctx.active, &candidate);
  }

  if (out_changed != NULL) {
    *out_changed = changed;
  }
  return SL_STATUS_OK;
}

sl_status_t label_config_stage_field(app_cfg_field_id_t field_id,
                                     const void *data,
                                     size_t len)
{
  if (!s_cfg_ctx.initialized) {
    return SL_STATUS_NOT_INITIALIZED;
  }

  if (field_id == APP_CFG_STAGE_RESET_ID) {
    if ((data != NULL) || (len != 0U)) {
      return SL_STATUS_INVALID_PARAMETER;
    }
    memset(&s_cfg_ctx.staged, 0, sizeof(s_cfg_ctx.staged));
    s_cfg_ctx.staged_mask = 0U;
    return SL_STATUS_OK;
  }

  const cfg_field_meta_t *meta = _find_field(field_id);
  if (meta == NULL) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  sl_status_t st = _write_field_slice(&s_cfg_ctx.staged, meta, data, len);
  if (st != SL_STATUS_OK) {
    return st;
  }

  s_cfg_ctx.staged_mask |= APP_CFG_FIELD_BIT(field_id);
  return SL_STATUS_OK;
}

sl_status_t label_config_commit(label_config_commit_result_t *out_result)
{
  if (out_result != NULL) {
    memset(out_result, 0, sizeof(*out_result));
  }
  if (!s_cfg_ctx.initialized) {
    return SL_STATUS_NOT_INITIALIZED;
  }

  /* 1. Every core field (0x01..0x0F) must have been written in staging. */
  if ((s_cfg_ctx.staged_mask & APP_CFG_CORE_FIELDS_MASK) != APP_CFG_CORE_FIELDS_MASK) {
    return SL_STATUS_NOT_READY;
  }

  /* 2. Build candidate: core from staged; optional customer arrays retain
   *    active value when not written during this staging session. */
  zinergy_nvm3_config_t candidate;
  memset(&candidate, 0, sizeof(candidate));
  uint8_t *cand_bytes = (uint8_t *)&candidate;
  const uint8_t *staged_bytes = (const uint8_t *)&s_cfg_ctx.staged;
  const uint8_t *active_bytes = (const uint8_t *)&s_cfg_ctx.active;

  for (size_t i = 0U; i < CFG_VALID_FIELD_COUNT; i++) {
    uint32_t bit = APP_CFG_FIELD_BIT(s_field_table[i].id);
    const uint8_t *src = ((s_cfg_ctx.staged_mask & bit) != 0U)
                         ? staged_bytes
                         : active_bytes;
    memcpy(&cand_bytes[s_field_table[i].offset],
           &src[s_field_table[i].offset],
           s_field_table[i].size);
  }

  /* 3. Validate complete snapshot per SRS 0.3 §5.1.2. */
  sl_status_t st = label_config_validate(&candidate);
  if (st != SL_STATUS_OK) {
    return st;
  }

  /* 4. Persist to NVM3 BEFORE modifying active configuration or reporting OK. */
  bool nvm_written = false;
  st = label_nvm_save_config(&candidate, true, &nvm_written);
  if (st != SL_STATUS_OK) {
    return st;
  }

  /* 5. Compute list of changed field IDs (ZSL-15) against previous active. */
  uint8_t changed_ids[APP_CFG_FIELD_ID_MAX];
  uint8_t changed_cnt = 0U;
  memset(changed_ids, 0, sizeof(changed_ids));
  for (size_t i = 0U; i < CFG_VALID_FIELD_COUNT; i++) {
    size_t ofs = s_field_table[i].offset;
    size_t sz = s_field_table[i].size;
    if (memcmp(&active_bytes[ofs], &cand_bytes[ofs], sz) != 0) {
      changed_ids[changed_cnt++] = (uint8_t)s_field_table[i].id;
    }
  }

  /* 6. Apply candidate to active configuration and reset staging session. */
  _copy_clean_snapshot(&s_cfg_ctx.active, &candidate);
  s_cfg_ctx.configured = true;
  memset(&s_cfg_ctx.staged, 0, sizeof(s_cfg_ctx.staged));
  s_cfg_ctx.staged_mask = 0U;

  if (out_result != NULL) {
    out_result->changed_count = changed_cnt;
    if (changed_cnt > 0U) {
      memcpy(out_result->changed_field_ids, changed_ids, changed_cnt);
    }
    out_result->nvm_written = nvm_written;
    out_result->configured = true;
  }
  return SL_STATUS_OK;
}
