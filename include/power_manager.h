/**
 * @file power_manager.h
 * @author embeddat.dev@gmail.com
 * @brief Application power policy for EFR32BG27 and Silicon Labs Power Manager.
 */
#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "em_gpio.h"
#include "sl_power_manager.h"
#include "sl_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LABEL_BOOT_POWER_ON_RESET,
  LABEL_BOOT_FROM_EM4_WAKEUP,
  LABEL_BOOT_OTHER
} label_boot_reason_t;

typedef struct {
  sl_power_manager_em_t initial_mode;
  bool fast_wakeup;
} label_power_config_t;

/** Default: allow deep sleep and use the SDK's low-power voltage scaling. */
#define LABEL_POWER_CONFIG_DEFAULT { SL_POWER_MANAGER_EM2, false }

typedef struct {
  bool initialized;
  sl_power_manager_em_t requested_mode;
  uint16_t em1_lock_count;
  bool em1_requirement_active;
  sl_power_manager_em_t last_from;
  sl_power_manager_em_t last_to;
  uint32_t transition_count;
} label_power_status_t;

typedef struct {
  GPIO_Port_TypeDef port;
  unsigned int pin;
  bool active_high;
  bool retain_gpio; /**< true: latch through reset; false: release on EM4 exit. */
} label_shelf_config_t;

/**
 * Initialize application policy AFTER sl_main_init(); NULL uses defaults.
 * The SDK Power Manager is already initialized by sl_main; do not reinitialize it.
 * Call once from main context. Repeated calls return ALREADY_INITIALIZED.
 */
sl_status_t label_power_manager_init(const label_power_config_t *config);

/**
 * Set idle policy; takes effect at the next main-loop sleep opportunity.
 * EM0: veto sleep. EM1: hold an EM1 requirement. EM2/EM3: allow deep sleep.
 * EM2 and EM3 share the SDK deep-sleep policy on Series 2; EM3 does not disable
 * LF clocks/peripherals. Other SDK requirements can still keep the MCU in EM1.
 * EM4 is rejected; use the explicit shelf-mode functions below.
 * Call from main context, never from an energy-mode transition notification.
 */
sl_status_t label_power_manager_set_mode(sl_power_manager_em_t mode);

/**
 * Acquire BEFORE starting an operation that needs HF clocks; release on completion
 * or startup failure. Nested acquisitions are counted, independent of idle policy.
 * ISR-safe, but never call from an energy-mode transition notification.
 * Unmatched release and counter overflow return an error without changing state.
 * SDK drivers that already own their requirements do not need another lock.
 */
sl_status_t label_power_manager_acquire_em1(void);
sl_status_t label_power_manager_release_em1(void);

/** Atomic snapshot. last_from/last_to describe the latest SDK notification,
 * not instantaneous CPU mode; sleep entry is notified before hardware entry.
 */
sl_status_t label_power_manager_get_status(label_power_status_t *status);

/** Nonblocking policy query for bare-metal sleep hooks; false before init. */
bool label_power_manager_is_ok_to_sleep(void);

/** Cache and clear reset flags once per boot, preferably in app_init_pre_clock().
 * EM4 takes precedence over POR if both flags are set. Repeated reads are stable.
 */
label_boot_reason_t label_detect_boot_reason(void);
uint32_t label_get_reset_cause(void);

/**
 * Store/validate one EM4 GPIO wake source against the selected device's fixed
 * EM4WU mapping. Does not change GPIO until shelf entry. Main context only.
 * Choose a free pin from the board schematic; PA5 is currently VCOM TX.
 */
sl_status_t label_configure_shelf_mode(const label_shelf_config_t *config);

/**
 * Enter EM4 using the configured GPIO. Returns an error if unconfigured, called
 * with interrupts masked/from ISR, an EM1 lock is held, or wake input is active.
 * Success NEVER returns; GPIO wake restarts the MCU through reset.
 * Caller must first stop BLE, drain/stop UART/DMA, save state, shut down external
 * loads, and disable EM4-blocking watchdogs. Bypasses normal sleep requirements.
 * If retain_gpio is true, reinitialize pins on boot then call
 * sl_power_manager_em4_unlatch_pin_retention() (done by this project's app_init).
 */
sl_status_t label_enter_shelf_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* POWER_MANAGER_H */
