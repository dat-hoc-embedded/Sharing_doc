/**
 * @file label_nvm.c
 * @brief Deep-module NVM3 persistence layer for Smart Label (Ticket 2).
 */
#include "label_nvm.h"

#include <string.h>
#include "label_config.h"
#include "nvm3.h"
#include "nvm3_default.h"
#include "nvm3_default_config.h"

/* Persistent deterministic zero-padded record formats. */
typedef struct {
  label_nvm_header_t hdr;
  zinergy_nvm3_config_t payload;
} nvm_config_blob_t;
typedef struct {
  label_nvm_header_t hdr;
  uint8_t valid, reserved;
  uint16_t max_temp_ck, min_temp_ck, pad;
} nvm_extrema_blob_t;
typedef struct {
  label_nvm_header_t hdr;
  uint32_t coulomb_estimate_uah, voltage_estimate_uah, selected_remaining_uah;
  int32_t discharge_rate_uah_per_hour;
  uint8_t active_profile, pad[3];
} nvm_battery_blob_t;
typedef struct {
  label_nvm_header_t hdr;
  uint64_t last_ot_timestamp_s, last_ut_timestamp_s;
  uint8_t ot_seen, ut_seen, pad[6];
} nvm_otut_blob_t;
typedef struct {
  label_nvm_header_t hdr;
  uint16_t write_index, count;
} nvm_hist_meta_blob_t;
typedef struct {
  uint32_t rtc_low_s, rtc_high_s, charge_estimate_uah;
} nvm_packed_hist_entry_t;
typedef struct {
  label_nvm_header_t hdr;
  nvm_packed_hist_entry_t entries[APP_CFG_CHARGE_HIST_CHUNK_ENTRIES];
} nvm_hist_chunk_blob_t;

_Static_assert(NVM3_DEFAULT_NVM_SIZE == APP_NVM3_FLASH_SIZE, "NVM3 size must be 24 KiB");
_Static_assert((APP_NVM3_FLASH_SIZE % APP_FLASH_PAGE_SIZE) == 0UL, "NVM3 page align");
_Static_assert(APP_NVM3_FLASH_SIZE >= (3UL * APP_FLASH_PAGE_SIZE), "NVM3 min 3 pages");
_Static_assert((APP_LOG_FLASH_BASE % APP_FLASH_PAGE_SIZE) == 0UL, "Log base page align");
_Static_assert((APP_LOG_FLASH_SIZE % APP_FLASH_PAGE_SIZE) == 0UL, "Log size page align");
_Static_assert((APP_LOG_FLASH_BASE + APP_LOG_FLASH_SIZE) <= APP_NVM3_EXPECTED_BASE,
               "Default log region overlaps NVM3");
_Static_assert(sizeof(nvm_config_blob_t) <= NVM3_DEFAULT_MAX_OBJECT_SIZE,
               "Config blob exceeds NVM3_DEFAULT_MAX_OBJECT_SIZE");
_Static_assert(sizeof(nvm_hist_chunk_blob_t) <= NVM3_DEFAULT_MAX_OBJECT_SIZE,
               "History chunk exceeds NVM3_DEFAULT_MAX_OBJECT_SIZE");

static struct {
  bool                  initialized;
  uintptr_t             nvm_base;
  size_t                nvm_size;
  uintptr_t             log_base;
  size_t                log_size;
  bool                  has_cfg_shadow;
  nvm_config_blob_t     cfg_shadow;
  bool                  has_extrema_shadow;
  nvm_extrema_blob_t    extrema_shadow;
  bool                  has_battery_shadow;
  nvm_battery_blob_t    battery_shadow;
  bool                  has_otut_shadow;
  nvm_otut_blob_t       otut_shadow;
  bool                  has_hist_meta_shadow;
  nvm_hist_meta_blob_t  hist_meta_shadow;
  uint16_t              hist_chunk_valid_mask;
  nvm_hist_chunk_blob_t hist_chunk_shadow[APP_CFG_CHARGE_HIST_CHUNK_COUNT];
} s_nvm_ctx;

static void _fill_header(label_nvm_header_t *hdr, bool configured)
{
  memset(hdr, 0, sizeof(*hdr));
  hdr->magic = APP_CFG_MAGIC;
  hdr->version = APP_CFG_SCHEMA_VERSION;
  hdr->configured = configured;
}

static bool _valid_header(const label_nvm_header_t *hdr)
{
  return (hdr->magic == APP_CFG_MAGIC)
         && (hdr->version == APP_CFG_SCHEMA_VERSION)
         && (hdr->reserved == 0U);
}

static void _normalize_config(zinergy_nvm3_config_t *dst, const zinergy_nvm3_config_t *src)
{
  memset(dst, 0, sizeof(*dst));
  dst->sample_interval_s = src->sample_interval_s;
  dst->ot_threshold_ck = src->ot_threshold_ck;
  dst->ut_threshold_ck = src->ut_threshold_ck;
  dst->qualification_count = src->qualification_count;
  dst->rtc_unix_s = src->rtc_unix_s;
  dst->adv_interval_ms = src->adv_interval_ms;
  dst->adv_train_duration_s = src->adv_train_duration_s;
  dst->adv_train_period_s = src->adv_train_period_s;
  dst->lp_interval_factor = src->lp_interval_factor;
  dst->lp_duration_factor = src->lp_duration_factor;
  dst->lp_period_factor = src->lp_period_factor;
  dst->normal_to_low_threshold_uah = src->normal_to_low_threshold_uah;
  dst->max_tx_power_dbm10 = src->max_tx_power_dbm10;
  dst->low_power_reduction_dbm10 = src->low_power_reduction_dbm10;
  dst->critical_power_reduction_dbm10 = src->critical_power_reduction_dbm10;
  memcpy(dst->user_data, src->user_data, APP_CFG_USER_DATA_SIZE);
  memcpy(dst->customer_description, src->customer_description, APP_CFG_CUSTOM_DESC_SIZE);
}

static void _apply_default_config(zinergy_nvm3_config_t *cfg)
{
  static const char default_desc[] = APP_CFG_CUSTOM_DESC_DEFAULT;
  memset(cfg, 0, sizeof(*cfg));
  cfg->sample_interval_s = APP_CFG_SAMPLE_INTERVAL_DEFAULT_S;
  cfg->ot_threshold_ck = APP_CFG_OT_DEFAULT_CK;
  cfg->ut_threshold_ck = APP_CFG_UT_DEFAULT_CK;
  cfg->qualification_count = APP_CFG_QUAL_COUNT_DEFAULT;
  cfg->rtc_unix_s = APP_CFG_RTC_DEFAULT_UNIX_S;
  cfg->adv_interval_ms = APP_CFG_ADV_INTERVAL_DEFAULT_MS;
  cfg->adv_train_duration_s = APP_CFG_TRAIN_DURATION_DEFAULT_S;
  cfg->adv_train_period_s = APP_CFG_TRAIN_PERIOD_DEFAULT_S;
  cfg->lp_interval_factor = APP_CFG_LP_INTERVAL_FACTOR_DEFAULT;
  cfg->lp_duration_factor = APP_CFG_LP_DURATION_FACTOR_DEFAULT;
  cfg->lp_period_factor = APP_CFG_LP_PERIOD_FACTOR_DEFAULT;
  cfg->normal_to_low_threshold_uah = APP_CFG_CHARGE_THR_DEFAULT_UAH;
  cfg->max_tx_power_dbm10 = APP_CFG_MAX_TX_POWER_DEFAULT_DBM10;
  cfg->low_power_reduction_dbm10 = APP_CFG_LP_POWER_RED_DEFAULT_DBM10;
  cfg->critical_power_reduction_dbm10 = APP_CFG_CRIT_POWER_RED_DEFAULT_DBM10;
  memcpy(cfg->customer_description, default_desc, sizeof(default_desc) - 1U);
}

static size_t _active_hist_chunks(uint16_t count)
{
  if (count == 0U) { return 0U; }
  if (count >= APP_CFG_CHARGE_HISTORY_LEN) { return APP_CFG_CHARGE_HIST_CHUNK_COUNT; }
  return (((size_t)count - 1U) / APP_CFG_CHARGE_HIST_CHUNK_ENTRIES) + 1U;
}

static sl_status_t _read_exact_object(nvm3_ObjectKey_t key, void *buf, size_t expected_len)
{
  uint32_t obj_type = 0U;
  size_t obj_len = 0U;
  sl_status_t st = nvm3_getObjectInfo(nvm3_defaultHandle, key, &obj_type, &obj_len);
  if (st != SL_STATUS_OK) { return st; }
  if ((obj_type != NVM3_OBJECTTYPE_DATA) || (obj_len != expected_len)) {
    return SL_STATUS_INVALID_TYPE;
  }
  return nvm3_readData(nvm3_defaultHandle, key, buf, expected_len);
}

static sl_status_t _write_if_changed(nvm3_ObjectKey_t key, const void *blob, void *shadow,
                                     bool *has_shadow, size_t len, bool *out_written)
{
  if (*has_shadow && (memcmp(shadow, blob, len) == 0)) {
    if (out_written != NULL) { *out_written = false; }
    return SL_STATUS_OK;
  }
  sl_status_t st = nvm3_writeData(nvm3_defaultHandle, key, blob, len);
  if (st == SL_STATUS_OK) {
    memcpy(shadow, blob, len);
    *has_shadow = true;
    if (out_written != NULL) { *out_written = true; }
  }
  return st;
}

static void _pack_hist_chunk(nvm_hist_chunk_blob_t *dst, const charge_history_entry_t *entries)
{
  _fill_header(&dst->hdr, true);
  for (size_t i = 0U; i < APP_CFG_CHARGE_HIST_CHUNK_ENTRIES; i++) {
    dst->entries[i].rtc_low_s = (uint32_t)(entries[i].rtc_unix_s & 0xFFFFFFFFULL);
    dst->entries[i].rtc_high_s = (uint32_t)((entries[i].rtc_unix_s >> 32U) & 0xFFFFFFFFULL);
    dst->entries[i].charge_estimate_uah = entries[i].charge_estimate_uah;
  }
}

static void _unpack_hist_chunk(charge_history_entry_t *dst, const nvm_hist_chunk_blob_t *src)
{
  for (size_t i = 0U; i < APP_CFG_CHARGE_HIST_CHUNK_ENTRIES; i++) {
    dst[i].rtc_unix_s = ((uint64_t)src->entries[i].rtc_high_s << 32U)
                        | (uint64_t)src->entries[i].rtc_low_s;
    dst[i].charge_estimate_uah = src->entries[i].charge_estimate_uah;
  }
}

sl_status_t label_nvm_init(const label_nvm_region_cfg_t *log_region)
{
  uintptr_t log_base = (log_region != NULL) ? log_region->log_flash_base : (uintptr_t)APP_LOG_FLASH_BASE;
  size_t log_size = (log_region != NULL) ? log_region->log_flash_size : (size_t)APP_LOG_FLASH_SIZE;

  if ((log_size == 0U) || ((log_base % APP_FLASH_PAGE_SIZE) != 0U)
      || ((log_size % APP_FLASH_PAGE_SIZE) != 0U)
      || (log_base < (uintptr_t)APP_MAIN_FLASH_BASE)
      || (log_size > (size_t)(APP_MAIN_FLASH_END - APP_MAIN_FLASH_BASE))
      || ((log_base + log_size) > (uintptr_t)APP_MAIN_FLASH_END)) {
    return SL_STATUS_INVALID_CONFIGURATION;
  }

  sl_status_t st = nvm3_initDefault();
  if (st != SL_STATUS_OK) {
    return st;
  }
  if ((nvm3_defaultHandle == NULL) || (nvm3_defaultInit == NULL)) {
    return SL_STATUS_INVALID_STATE;
  }

  uintptr_t nvm_base = (uintptr_t)nvm3_defaultInit->nvmAdr;
  size_t nvm_size = nvm3_defaultInit->nvmSize;
  if ((nvm_size != APP_NVM3_FLASH_SIZE) || ((nvm_size % APP_FLASH_PAGE_SIZE) != 0U)) {
    return SL_STATUS_INVALID_CONFIGURATION;
  }
  if ((log_base < (nvm_base + nvm_size)) && (nvm_base < (log_base + log_size))) {
    return SL_STATUS_INVALID_CONFIGURATION;
  }

  memset(&s_nvm_ctx, 0, sizeof(s_nvm_ctx));
  s_nvm_ctx.nvm_base = nvm_base;
  s_nvm_ctx.nvm_size = nvm_size;
  s_nvm_ctx.log_base = log_base;
  s_nvm_ctx.log_size = log_size;
  s_nvm_ctx.initialized = true;
  return SL_STATUS_OK;
}

sl_status_t label_nvm_load_config(zinergy_nvm3_config_t *out_cfg,
                                  bool *out_configured, bool *out_used_defaults)
{
  if (out_cfg == NULL) {
    return SL_STATUS_NULL_POINTER;
  }
  if (!s_nvm_ctx.initialized) {
    return SL_STATUS_NOT_INITIALIZED;
  }

  nvm_config_blob_t blob;
  memset(&blob, 0, sizeof(blob));
  if ((_read_exact_object(LABEL_NVM_KEY_CONFIG, &blob, sizeof(blob)) ==
       SL_STATUS_OK) &&
      _valid_header(&blob.hdr) &&
      (label_config_validate(&blob.payload) == SL_STATUS_OK))
  {
    nvm_config_blob_t clean_blob;
    _fill_header(&clean_blob.hdr, blob.hdr.configured);
    _normalize_config(&clean_blob.payload, &blob.payload);
    s_nvm_ctx.has_cfg_shadow = (memcmp(&clean_blob, &blob, sizeof(blob)) == 0); // 
    if (s_nvm_ctx.has_cfg_shadow) {
      s_nvm_ctx.cfg_shadow = clean_blob;
    }
    *out_cfg = clean_blob.payload;
    if (out_configured != NULL) { *out_configured = blob.hdr.configured; }
    if (out_used_defaults != NULL) { *out_used_defaults = false; }
    return SL_STATUS_OK;
  }

  _apply_default_config(out_cfg);
  s_nvm_ctx.has_cfg_shadow = false;
  if (out_configured != NULL) { *out_configured = false; }
  if (out_used_defaults != NULL) { *out_used_defaults = true; }
  return SL_STATUS_OK;
}

sl_status_t label_nvm_save_config(const zinergy_nvm3_config_t *cfg,
                                  bool configured, bool *out_written)
{
  if (cfg == NULL) {
    return SL_STATUS_NULL_POINTER;
  }
  if (!s_nvm_ctx.initialized) {
    return SL_STATUS_NOT_INITIALIZED;
  }
  sl_status_t st = label_config_validate(cfg);
  if (st != SL_STATUS_OK) {
    return st;
  }

  nvm_config_blob_t blob;
  _fill_header(&blob.hdr, configured);
  _normalize_config(&blob.payload, cfg);
  return _write_if_changed(LABEL_NVM_KEY_CONFIG, &blob, &s_nvm_ctx.cfg_shadow,
                           &s_nvm_ctx.has_cfg_shadow, sizeof(blob), out_written);
}

sl_status_t label_nvm_load_state(label_nvm_state_id_t state_id,
                                 void *out_state, size_t state_size, bool *out_used_defaults)
{
  if (out_state == NULL) {
    return SL_STATUS_NULL_POINTER;
  }
  if (!s_nvm_ctx.initialized) {
    return SL_STATUS_NOT_INITIALIZED;
  }

  bool fallback = true;
  switch (state_id) {
    case LABEL_NVM_STATE_EXTREMA: {
      if (state_size != sizeof(label_extrema_t)) { return SL_STATUS_INVALID_PARAMETER; }
      label_extrema_t *dst = (label_extrema_t *)out_state;
      memset(dst, 0, sizeof(*dst));
      nvm_extrema_blob_t b;
      if ((_read_exact_object(LABEL_NVM_KEY_EXTREMA, &b, sizeof(b)) == SL_STATUS_OK)
          && _valid_header(&b.hdr) && (b.valid <= 1U) && (b.reserved == 0U) && (b.pad == 0U)
          && ((b.valid == 0U) || ((b.min_temp_ck >= APP_CFG_TEMP_MIN_CK)
              && (b.max_temp_ck <= APP_CFG_TEMP_MAX_CK) && (b.min_temp_ck <= b.max_temp_ck)))) {
        dst->valid = (b.valid != 0U);
        dst->max_temp_ck = b.max_temp_ck;
        dst->min_temp_ck = b.min_temp_ck;
        s_nvm_ctx.extrema_shadow = b;
        s_nvm_ctx.has_extrema_shadow = true;
        fallback = false;
      } else {
        s_nvm_ctx.has_extrema_shadow = false;
      }
      break;
    }
    case LABEL_NVM_STATE_BATTERY: {
      if (state_size != sizeof(label_battery_state_t)) { return SL_STATUS_INVALID_PARAMETER; }
      label_battery_state_t *dst = (label_battery_state_t *)out_state;
      memset(dst, 0, sizeof(*dst));
      nvm_battery_blob_t b;
      if ((_read_exact_object(LABEL_NVM_KEY_BATTERY, &b, sizeof(b)) == SL_STATUS_OK)
          && _valid_header(&b.hdr) && (b.pad[0] == 0U) && (b.pad[1] == 0U) && (b.pad[2] == 0U)) {
        dst->coulomb_estimate_uah = b.coulomb_estimate_uah;
        dst->voltage_estimate_uah = b.voltage_estimate_uah;
        dst->selected_remaining_uah = b.selected_remaining_uah;
        dst->discharge_rate_uah_per_hour = b.discharge_rate_uah_per_hour;
        dst->active_profile = b.active_profile;
        s_nvm_ctx.battery_shadow = b;
        s_nvm_ctx.has_battery_shadow = true;
        fallback = false;
      } else {
        s_nvm_ctx.has_battery_shadow = false;
      }
      break;
    }
    case LABEL_NVM_STATE_OTUT: {
      if (state_size != sizeof(label_otut_state_t)) { return SL_STATUS_INVALID_PARAMETER; }
      label_otut_state_t *dst = (label_otut_state_t *)out_state;
      memset(dst, 0, sizeof(*dst));
      nvm_otut_blob_t b;
      if ((_read_exact_object(LABEL_NVM_KEY_OTUT, &b, sizeof(b)) == SL_STATUS_OK)
          && _valid_header(&b.hdr) && (b.ot_seen <= 1U) && (b.ut_seen <= 1U)) {
        dst->ot_seen = (b.ot_seen != 0U);
        dst->ut_seen = (b.ut_seen != 0U);
        dst->last_ot_timestamp_s = b.last_ot_timestamp_s;
        dst->last_ut_timestamp_s = b.last_ut_timestamp_s;
        s_nvm_ctx.otut_shadow = b;
        s_nvm_ctx.has_otut_shadow = true;
        fallback = false;
      } else {
        s_nvm_ctx.has_otut_shadow = false;
      }
      break;
    }
    case LABEL_NVM_STATE_CHARGE_HISTORY: {
      if (state_size != sizeof(label_charge_history_t)) { return SL_STATUS_INVALID_PARAMETER; }
      label_charge_history_t *dst = (label_charge_history_t *)out_state;
      memset(dst, 0, sizeof(*dst));
      nvm_hist_meta_blob_t meta;
      if ((_read_exact_object(LABEL_NVM_KEY_CHARGE_HIST_META, &meta, sizeof(meta)) == SL_STATUS_OK)
          && _valid_header(&meta.hdr)
          && (meta.write_index < APP_CFG_CHARGE_HISTORY_LEN)
          && (meta.count <= APP_CFG_CHARGE_HISTORY_LEN)) {
        bool chunks_ok = true;
        size_t needed = _active_hist_chunks(meta.count);
        s_nvm_ctx.hist_chunk_valid_mask = 0U;
        for (size_t c = 0U; c < APP_CFG_CHARGE_HIST_CHUNK_COUNT; c++) {
          nvm_hist_chunk_blob_t chunk;
          sl_status_t cst = _read_exact_object(LABEL_NVM_KEY_CHARGE_HIST_BASE + (nvm3_ObjectKey_t)c,
                                               &chunk, sizeof(chunk));
          if ((cst == SL_STATUS_OK) && _valid_header(&chunk.hdr)) {
            _unpack_hist_chunk(&dst->entries[c * APP_CFG_CHARGE_HIST_CHUNK_ENTRIES], &chunk);
            s_nvm_ctx.hist_chunk_shadow[c] = chunk;
            s_nvm_ctx.hist_chunk_valid_mask |= (uint16_t)(1U << c);
          } else if (c < needed) {
            chunks_ok = false;
            break;
          }
        }
        if (chunks_ok) {
          dst->write_index = meta.write_index;
          dst->count = meta.count;
          s_nvm_ctx.hist_meta_shadow = meta;
          s_nvm_ctx.has_hist_meta_shadow = true;
          fallback = false;
        } else {
          memset(dst, 0, sizeof(*dst));
          s_nvm_ctx.has_hist_meta_shadow = false;
          s_nvm_ctx.hist_chunk_valid_mask = 0U;
        }
      }
      break;
    }
    default:
      return SL_STATUS_INVALID_PARAMETER;
  }

  if (out_used_defaults != NULL) { *out_used_defaults = fallback; }
  return SL_STATUS_OK;
}

sl_status_t label_nvm_save_state(label_nvm_state_id_t state_id,
                                 const void *state, size_t state_size, bool *out_written)
{
  if (state == NULL) { return SL_STATUS_NULL_POINTER; }
  if (!s_nvm_ctx.initialized) { return SL_STATUS_NOT_INITIALIZED; }

  switch (state_id) {
    case LABEL_NVM_STATE_EXTREMA: {
      if (state_size != sizeof(label_extrema_t)) { return SL_STATUS_INVALID_PARAMETER; }
      const label_extrema_t *src = (const label_extrema_t *)state;
      if (src->valid && ((src->min_temp_ck < APP_CFG_TEMP_MIN_CK)
          || (src->max_temp_ck > APP_CFG_TEMP_MAX_CK) || (src->min_temp_ck > src->max_temp_ck))) {
        return SL_STATUS_INVALID_PARAMETER;
      }
      nvm_extrema_blob_t b;
      memset(&b, 0, sizeof(b));
      _fill_header(&b.hdr, true);
      b.valid = src->valid ? 1U : 0U;
      b.max_temp_ck = src->max_temp_ck;
      b.min_temp_ck = src->min_temp_ck;
      return _write_if_changed(LABEL_NVM_KEY_EXTREMA, &b, &s_nvm_ctx.extrema_shadow,
                               &s_nvm_ctx.has_extrema_shadow, sizeof(b), out_written);
    }
    case LABEL_NVM_STATE_BATTERY: {
      if (state_size != sizeof(label_battery_state_t)) { return SL_STATUS_INVALID_PARAMETER; }
      const label_battery_state_t *src = (const label_battery_state_t *)state;
      nvm_battery_blob_t b;
      memset(&b, 0, sizeof(b));
      _fill_header(&b.hdr, true);
      b.coulomb_estimate_uah = src->coulomb_estimate_uah;
      b.voltage_estimate_uah = src->voltage_estimate_uah;
      b.selected_remaining_uah = src->selected_remaining_uah;
      b.discharge_rate_uah_per_hour = src->discharge_rate_uah_per_hour;
      b.active_profile = src->active_profile;
      return _write_if_changed(LABEL_NVM_KEY_BATTERY, &b, &s_nvm_ctx.battery_shadow,
                               &s_nvm_ctx.has_battery_shadow, sizeof(b), out_written);
    }
    case LABEL_NVM_STATE_OTUT: {
      if (state_size != sizeof(label_otut_state_t)) { return SL_STATUS_INVALID_PARAMETER; }
      const label_otut_state_t *src = (const label_otut_state_t *)state;
      nvm_otut_blob_t b;
      memset(&b, 0, sizeof(b));
      _fill_header(&b.hdr, true);
      b.ot_seen = src->ot_seen ? 1U : 0U;
      b.ut_seen = src->ut_seen ? 1U : 0U;
      b.last_ot_timestamp_s = src->last_ot_timestamp_s;
      b.last_ut_timestamp_s = src->last_ut_timestamp_s;
      return _write_if_changed(LABEL_NVM_KEY_OTUT, &b, &s_nvm_ctx.otut_shadow,
                               &s_nvm_ctx.has_otut_shadow, sizeof(b), out_written);
    }
    case LABEL_NVM_STATE_CHARGE_HISTORY: {
      if (state_size != sizeof(label_charge_history_t)) { return SL_STATUS_INVALID_PARAMETER; }
      const label_charge_history_t *src = (const label_charge_history_t *)state;
      if ((src->write_index >= APP_CFG_CHARGE_HISTORY_LEN)
          || (src->count > APP_CFG_CHARGE_HISTORY_LEN)) {
        return SL_STATUS_INVALID_PARAMETER;
      }
      bool any_written = false;
      size_t active_chunks = _active_hist_chunks(src->count);
      for (size_t c = 0U; c < active_chunks; c++) {
        nvm_hist_chunk_blob_t chunk;
        _pack_hist_chunk(&chunk, &src->entries[c * APP_CFG_CHARGE_HIST_CHUNK_ENTRIES]);
        bool has_sh = ((s_nvm_ctx.hist_chunk_valid_mask & (uint16_t)(1U << c)) != 0U);
        bool cw = false;
        sl_status_t st = _write_if_changed(LABEL_NVM_KEY_CHARGE_HIST_BASE + (nvm3_ObjectKey_t)c,
                                           &chunk, &s_nvm_ctx.hist_chunk_shadow[c],
                                           &has_sh, sizeof(chunk), &cw);
        if (st != SL_STATUS_OK) { return st; }
        if (has_sh) { s_nvm_ctx.hist_chunk_valid_mask |= (uint16_t)(1U << c); }
        any_written = any_written || cw;
      }
      nvm_hist_meta_blob_t meta;
      _fill_header(&meta.hdr, true);
      meta.write_index = src->write_index;
      meta.count = src->count;
      bool mw = false;
      sl_status_t mst = _write_if_changed(LABEL_NVM_KEY_CHARGE_HIST_META, &meta,
                                          &s_nvm_ctx.hist_meta_shadow,
                                          &s_nvm_ctx.has_hist_meta_shadow, sizeof(meta), &mw);
      if (mst != SL_STATUS_OK) { return mst; }
      if (out_written != NULL) { *out_written = (any_written || mw); }
      return SL_STATUS_OK;
    }
    default:
      return SL_STATUS_INVALID_PARAMETER;
  }
}

sl_status_t label_nvm_append_charge_history(label_charge_history_t *history,
                                            const charge_history_entry_t *entry,
                                            bool *out_written)
{
  if ((history == NULL) || (entry == NULL)) { return SL_STATUS_NULL_POINTER; }
  if ((history->write_index >= APP_CFG_CHARGE_HISTORY_LEN)
      || (history->count > APP_CFG_CHARGE_HISTORY_LEN)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  history->entries[history->write_index] = *entry;
  history->write_index = (uint16_t)((history->write_index + 1U) % APP_CFG_CHARGE_HISTORY_LEN);
  if (history->count < APP_CFG_CHARGE_HISTORY_LEN) {
    history->count++;
  }
  return label_nvm_save_state(LABEL_NVM_STATE_CHARGE_HISTORY, history, sizeof(*history), out_written);
}

sl_status_t label_nvm_service(label_nvm_status_t *out_status)
{
  if (!s_nvm_ctx.initialized) { return SL_STATUS_NOT_INITIALIZED; }
  if (nvm3_repackNeeded(nvm3_defaultHandle)) {
    sl_status_t rst = nvm3_repack(nvm3_defaultHandle);
    if (rst != SL_STATUS_OK) { return rst; }
  }

  if (out_status != NULL) {
    nvm3_MemInfo_t mem_info;
    memset(&mem_info, 0, sizeof(mem_info));
    (void)nvm3_getMemInfo(nvm3_defaultHandle, &mem_info);
    uint32_t erase_cnt = 0U;
    (void)nvm3_getEraseCount(nvm3_defaultHandle, &erase_cnt);

    memset(out_status, 0, sizeof(*out_status));
    out_status->initialized = true;
    out_status->nvm_flash_base = s_nvm_ctx.nvm_base;
    out_status->nvm_flash_size = s_nvm_ctx.nvm_size;
    out_status->log_flash_base = s_nvm_ctx.log_base;
    out_status->log_flash_size = s_nvm_ctx.log_size;
    out_status->live_objects = nvm3_countObjects(nvm3_defaultHandle);
    out_status->deleted_objects = nvm3_countDeletedObjects(nvm3_defaultHandle);
    out_status->available_memory = mem_info.availableMemory;
    out_status->erase_count = erase_cnt;
    out_status->is_memory_low = mem_info.isMemoryLow;
    out_status->is_cache_low = mem_info.isCacheLow;
  }
  return SL_STATUS_OK;
}
