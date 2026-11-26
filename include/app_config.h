/**
 * @file app_config.h
 * @brief Smart Label configuration field IDs, SRS 0.3 §5.1.2 bounds, defaults,
 *        Flash partition bounds, and shared persistent/runtime data structures.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* NVM Header Magic & Schema Versioning                                       */
/* -------------------------------------------------------------------------- */

#define APP_CFG_MAGIC                          0x5A494E45UL /* "ZINE" */
#define APP_CFG_SCHEMA_VERSION                 1U

/* -------------------------------------------------------------------------- */
/* Array & History Dimensions (Ticket 2 & Ticket 3)                           */
/* -------------------------------------------------------------------------- */

#define APP_CFG_USER_DATA_SIZE                 10U          /* Single 10-byte opaque field */
#define APP_CFG_CUSTOM_DESC_SIZE               255U         /* 255-byte customer description */
#define APP_CFG_CHARGE_HISTORY_LEN             256U         /* Ring buffer entry capacity */
#define APP_CFG_CHARGE_HIST_CHUNK_ENTRIES      16U          /* Entries per NVM3 chunk */
#define APP_CFG_CHARGE_HIST_CHUNK_COUNT        (APP_CFG_CHARGE_HISTORY_LEN / APP_CFG_CHARGE_HIST_CHUNK_ENTRIES)

/* -------------------------------------------------------------------------- */
/* EFR32BG27 Flash Map & Non-Overlap Bounds (Ticket 2)                        */
/* -------------------------------------------------------------------------- */
/* Main Flash: 0x08000000 .. 0x080C0000 (768 KiB = 96 x 8 KiB pages)
 * Page 95 (0x080BE000 .. 0x080C0000): Manufacturing Tokens / Lock Bits (8 KiB)
 * Pages 92..94 (0x080B8000 .. 0x080BE000): NVM3 Default Instance (24 KiB)
 * Log Region MUST reside in page-aligned Main Flash strictly below NVM3.     */

#define APP_FLASH_PAGE_SIZE                    8192UL
#define APP_MAIN_FLASH_BASE                    0x08000000UL
#define APP_MAIN_FLASH_END                     0x080BE000UL
#define APP_NVM3_FLASH_SIZE                    24576UL      /* 3 x 8 KiB = 24 KiB per SRS */
#define APP_NVM3_EXPECTED_BASE                 (APP_MAIN_FLASH_END - APP_NVM3_FLASH_SIZE)

#ifndef APP_LOG_FLASH_BASE
#define APP_LOG_FLASH_BASE                     0x08080000UL
#endif

#ifndef APP_LOG_FLASH_SIZE
#define APP_LOG_FLASH_SIZE                     0x00038000UL /* 224 KiB (ends at 0x080B8000) */
#endif

/* -------------------------------------------------------------------------- */
/* SRS Field Identifiers (0x01 .. 0x12)                                       */
/* -------------------------------------------------------------------------- */

typedef enum {
  APP_CFG_STAGE_RESET_ID               = 0x00, /* Special selector to clear staging */
  APP_CFG_FIELD_SAMPLE_INTERVAL        = 0x01, /* Core: uint32_t (seconds) */
  APP_CFG_FIELD_OT_THRESHOLD           = 0x02, /* Core: uint16_t (centi-Kelvin) */
  APP_CFG_FIELD_UT_THRESHOLD           = 0x03, /* Core: uint16_t (centi-Kelvin) */
  APP_CFG_FIELD_QUALIFICATION_COUNT    = 0x04, /* Core: uint8_t  (samples) */
  APP_CFG_FIELD_RTC                    = 0x05, /* Core: uint64_t (Unix epoch s) */
  APP_CFG_FIELD_NORM_TO_LOW_THRESHOLD  = 0x06, /* Core: uint16_t (uAh) */
  APP_CFG_FIELD_ADV_INTERVAL           = 0x07, /* Core: uint16_t (ms) */
  APP_CFG_FIELD_TRAIN_DURATION         = 0x08, /* Core: uint16_t (seconds) */
  APP_CFG_FIELD_TRAIN_PERIOD           = 0x09, /* Core: uint32_t (seconds) */
  APP_CFG_FIELD_LP_INTERVAL_FACTOR     = 0x0A, /* Core: uint16_t (multiplier) */
  APP_CFG_FIELD_LP_DURATION_FACTOR     = 0x0B, /* Core: uint16_t (multiplier) */
  APP_CFG_FIELD_LP_PERIOD_FACTOR       = 0x0C, /* Core: uint16_t (multiplier) */
  APP_CFG_FIELD_MAX_TX_POWER           = 0x0D, /* Core: int16_t  (0.1 dBm) */
  APP_CFG_FIELD_LP_POWER_REDUCTION     = 0x0E, /* Core: uint16_t (0.1 dB) */
  APP_CFG_FIELD_CRIT_POWER_REDUCTION   = 0x0F, /* Core: uint16_t (0.1 dB) */
  APP_CFG_FIELD_CUSTOMER_DESC          = 0x10, /* Optional: uint8_t[255] */
  APP_CFG_FIELD_RESERVED_11            = 0x11, /* Reserved: always rejected */
  APP_CFG_FIELD_USER_DATA              = 0x12, /* Optional: uint8_t[10] opaque */
  APP_CFG_FIELD_ID_MAX                 = 0x12
} app_cfg_field_id_t;

#define APP_CFG_FIELD_BIT(id)                  (1UL << (uint32_t)(id))
#define APP_CFG_CORE_FIELDS_MASK               0x0000FFFEUL /* Bits 0x01..0x0F */
#define APP_CFG_OPTIONAL_FIELDS_MASK           (APP_CFG_FIELD_BIT(APP_CFG_FIELD_CUSTOMER_DESC) \
                                                | APP_CFG_FIELD_BIT(APP_CFG_FIELD_USER_DATA))

/* -------------------------------------------------------------------------- */
/* SRS 0.3 §5.1.2 Validation Bounds & Compile-Time Defaults                   */
/* -------------------------------------------------------------------------- */

/* 0x01: Sampling Interval (seconds) */
#define APP_CFG_SAMPLE_INTERVAL_MIN_S          1UL
#define APP_CFG_SAMPLE_INTERVAL_MAX_S          86400UL
#define APP_CFG_SAMPLE_INTERVAL_DEFAULT_S      300UL

/* 0x02 & 0x03: OT / UT Thresholds (1..65,534 cK, UT < OT) */
#define APP_CFG_TEMP_MIN_CK                    1U
#define APP_CFG_TEMP_MAX_CK                    65534U
#define APP_CFG_UT_DEFAULT_CK                  25315U       /* -20.00 C */
#define APP_CFG_OT_DEFAULT_CK                  31815U       /* +45.00 C */

/* 0x04: Qualification Count (1..255 consecutive excursions) */
#define APP_CFG_QUAL_COUNT_MIN                 1U
#define APP_CFG_QUAL_COUNT_MAX                 255U
#define APP_CFG_QUAL_COUNT_DEFAULT             3U

/* 0x05: RTC Unix Timestamp (seconds) */
#define APP_CFG_RTC_MIN_UNIX_S                 1704067200ULL /* 2024-01-01T00:00:00Z */
#define APP_CFG_RTC_MAX_UNIX_S                 4102444800ULL /* 2100-01-01T00:00:00Z */
#define APP_CFG_RTC_DEFAULT_UNIX_S             1735689600ULL /* 2025-01-01T00:00:00Z */

/* 0x06: Normal-to-Low Charge Threshold (uAh) */
#define APP_CFG_CHARGE_THR_MIN_UAH             100U
#define APP_CFG_CHARGE_THR_MAX_UAH             60000U
#define APP_CFG_CHARGE_THR_DEFAULT_UAH         5000U

/* 0x07: Normal Advertising Interval (ms, BLE legacy adv bounds) */
#define APP_CFG_ADV_INTERVAL_MIN_MS            20U
#define APP_CFG_ADV_INTERVAL_MAX_MS            10240U
#define APP_CFG_ADV_INTERVAL_DEFAULT_MS        1000U

/* 0x08 & 0x09: Advertising Train Duration & Period (s, period >= duration) */
#define APP_CFG_TRAIN_DURATION_MIN_S           1U
#define APP_CFG_TRAIN_DURATION_MAX_S           3600U
#define APP_CFG_TRAIN_DURATION_DEFAULT_S       30U

#define APP_CFG_TRAIN_PERIOD_MIN_S             1UL
#define APP_CFG_TRAIN_PERIOD_MAX_S             86400UL
#define APP_CFG_TRAIN_PERIOD_DEFAULT_S         300UL

/* 0x0A, 0x0B, 0x0C: Low-Power Scaling Factors */
#define APP_CFG_LP_FACTOR_MIN                  1U
#define APP_CFG_LP_FACTOR_MAX                  100U
#define APP_CFG_LP_INTERVAL_FACTOR_DEFAULT     2U
#define APP_CFG_LP_DURATION_FACTOR_DEFAULT     1U
#define APP_CFG_LP_PERIOD_FACTOR_DEFAULT       2U

/* 0x0D: Maximum TX Power (0.1 dBm units; <= +4.0 dBm -> +40) */
#define APP_CFG_MAX_TX_POWER_MIN_DBM10         (-200)
#define APP_CFG_MAX_TX_POWER_MAX_DBM10         40
#define APP_CFG_MAX_TX_POWER_DEFAULT_DBM10     0

/* 0x0E & 0x0F: Low-Power & Critical TX Power Reductions (0.1 dB; crit >= lp) */
#define APP_CFG_POWER_RED_MIN_DBM10            0U
#define APP_CFG_POWER_RED_MAX_DBM10            240U
#define APP_CFG_LP_POWER_RED_DEFAULT_DBM10     20U
#define APP_CFG_CRIT_POWER_RED_DEFAULT_DBM10   60U

/* 0x10 & 0x12: Optional Default Customer Description */
#define APP_CFG_CUSTOM_DESC_DEFAULT            "Zinergy Smart Label"

/* -------------------------------------------------------------------------- */
/* Shared Data Structures                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
  uint32_t magic;
  uint16_t version;
  bool     configured;
  uint8_t  reserved;
} label_nvm_header_t;

typedef struct {
  uint32_t sample_interval_s;
  uint16_t ot_threshold_ck;
  uint16_t ut_threshold_ck;
  uint8_t  qualification_count;

  uint64_t rtc_unix_s;

  uint16_t adv_interval_ms;
  uint16_t adv_train_duration_s;
  uint32_t adv_train_period_s;

  uint16_t lp_interval_factor;
  uint16_t lp_duration_factor;
  uint16_t lp_period_factor;

  uint16_t normal_to_low_threshold_uah;

  int16_t  max_tx_power_dbm10;
  uint16_t low_power_reduction_dbm10;
  uint16_t critical_power_reduction_dbm10;

  uint8_t  user_data[APP_CFG_USER_DATA_SIZE];
  uint8_t  customer_description[APP_CFG_CUSTOM_DESC_SIZE];
} zinergy_nvm3_config_t;

typedef struct {
  bool     valid;
  uint16_t max_temp_ck;
  uint16_t min_temp_ck;
} label_extrema_t;

typedef struct {
  uint32_t coulomb_estimate_uah;
  uint32_t voltage_estimate_uah;
  uint32_t selected_remaining_uah;
  int32_t  discharge_rate_uah_per_hour;
  uint8_t  active_profile;
} label_battery_state_t;

typedef struct {
  uint64_t rtc_unix_s;
  uint32_t charge_estimate_uah;
} charge_history_entry_t;

typedef struct {
  uint16_t               write_index;
  uint16_t               count;
  charge_history_entry_t entries[APP_CFG_CHARGE_HISTORY_LEN];
} label_charge_history_t;

typedef struct {
  bool     ot_seen;
  bool     ut_seen;
  uint64_t last_ot_timestamp_s;
  uint64_t last_ut_timestamp_s;
} label_otut_state_t;

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
