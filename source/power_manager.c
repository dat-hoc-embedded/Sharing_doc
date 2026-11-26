#define CURRENT_MODULE_NAME "label_power_manager"
#include "power_manager.h"
#include "em_cmu.h"
#include "em_emu.h"
#include "sl_core.h"
#include "sl_device_init_dcdc_config.h"
#include <stddef.h>

static label_power_status_t power_status;
static label_shelf_config_t shelf_config;
static uint32_t shelf_wakeup_mask;
static bool boot_captured;
static uint32_t reset_cause;
static label_boot_reason_t boot_reason;
static sl_power_manager_em_transition_event_handle_t transition_handle;

static bool valid_mode(sl_power_manager_em_t mode) {
  return mode == SL_POWER_MANAGER_EM0 || mode == SL_POWER_MANAGER_EM1 ||
         mode == SL_POWER_MANAGER_EM2 || mode == SL_POWER_MANAGER_EM3;
}

/* Caller holds the critical section. Own one SDK requirement even when policy
 * and several independent application operations all require EM1. */
static void update_em1_requirement(void) {
  bool required = power_status.requested_mode == SL_POWER_MANAGER_EM1 ||
                  power_status.em1_lock_count != 0;
  if (required && !power_status.em1_requirement_active) {
    sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);
  } else if (!required && power_status.em1_requirement_active) {
    sl_power_manager_remove_em_requirement(SL_POWER_MANAGER_EM1);
  }
  power_status.em1_requirement_active = required;
}

/* Critical/ISR context: never log, block, or change requirements here. */
static void on_em_transition(sl_power_manager_em_t from,
                             sl_power_manager_em_t to) {
  power_status.last_from = from;
  power_status.last_to = to;
  power_status.transition_count++;
}

static const sl_power_manager_em_transition_event_info_t transition_info = {
    .event_mask = SL_POWER_MANAGER_EVENT_TRANSITION_ENTERING_EM0 |
                  SL_POWER_MANAGER_EVENT_TRANSITION_ENTERING_EM1 |
                  SL_POWER_MANAGER_EVENT_TRANSITION_ENTERING_EM2,
    .on_event = on_em_transition};

sl_status_t label_power_manager_init(const label_power_config_t *config) {
  const label_power_config_t defaults = LABEL_POWER_CONFIG_DEFAULT;
  if (config == NULL) {
    config = &defaults;
  }
  if (!valid_mode(config->initial_mode)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  if (power_status.initialized) {
    CORE_EXIT_CRITICAL();
    return SL_STATUS_ALREADY_INITIALIZED;
  }
  power_status.requested_mode = config->initial_mode;
  power_status.last_from = SL_POWER_MANAGER_EM0;
  power_status.last_to = SL_POWER_MANAGER_EM0;
  sl_power_manager_em23_voltage_scaling_enable_fast_wakeup(config->fast_wakeup);
  sl_power_manager_subscribe_em_transition_event(&transition_handle,
                                                 &transition_info);
  update_em1_requirement();
  power_status.initialized = true;
  CORE_EXIT_CRITICAL();
  return SL_STATUS_OK;
}

sl_status_t label_power_manager_set_mode(sl_power_manager_em_t mode) {
  if (!valid_mode(mode)) {
    return SL_STATUS_INVALID_PARAMETER;
  }
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  if (!power_status.initialized) {
    CORE_EXIT_CRITICAL();
    return SL_STATUS_NOT_INITIALIZED;
  }
  power_status.requested_mode = mode;
  update_em1_requirement();
  CORE_EXIT_CRITICAL();
  return SL_STATUS_OK;
}

sl_status_t label_power_manager_acquire_em1(void) {
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  if (!power_status.initialized) {
    CORE_EXIT_CRITICAL();
    return SL_STATUS_NOT_INITIALIZED;
  }
  if (power_status.em1_lock_count == UINT16_MAX) {
    CORE_EXIT_CRITICAL();
    return SL_STATUS_WOULD_OVERFLOW;
  }
  power_status.em1_lock_count++;
  update_em1_requirement();
  CORE_EXIT_CRITICAL();
  return SL_STATUS_OK;
}

sl_status_t label_power_manager_release_em1(void) {
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  if (!power_status.initialized) {
    CORE_EXIT_CRITICAL();
    return SL_STATUS_NOT_INITIALIZED;
  }
  if (power_status.em1_lock_count == 0) {
    CORE_EXIT_CRITICAL();
    return SL_STATUS_INVALID_STATE;
  }
  power_status.em1_lock_count--;
  update_em1_requirement();
  CORE_EXIT_CRITICAL();
  return SL_STATUS_OK;
}

sl_status_t label_power_manager_get_status(label_power_status_t *status) {
  if (status == NULL) {
    return SL_STATUS_NULL_POINTER;
  }
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  *status = power_status;
  CORE_EXIT_CRITICAL();
  return SL_STATUS_OK;
}

bool label_power_manager_is_ok_to_sleep(void) {
  return power_status.initialized &&
         power_status.requested_mode != SL_POWER_MANAGER_EM0;
}

label_boot_reason_t label_detect_boot_reason(void) {
  if (!boot_captured) {
    /* BG27 uses EMU reset registers. Capture early in SDK init, without
     * requiring the otherwise unused emlib_rmu component for these register
     * accesses. */
    reset_cause = EMU->RSTCAUSE;
    EMU->CMD_SET = EMU_CMD_RSTCAUSECLR;
    if ((reset_cause & EMU_RSTCAUSE_EM4) != 0) {
      boot_reason = LABEL_BOOT_FROM_EM4_WAKEUP;
    } else if ((reset_cause & EMU_RSTCAUSE_POR) != 0) {
      boot_reason = LABEL_BOOT_POWER_ON_RESET;
    } else {
      boot_reason = LABEL_BOOT_OTHER;
    }
    boot_captured = true;
  }
  return boot_reason;
}

uint32_t label_get_reset_cause(void) {
  (void)label_detect_boot_reason();
  return reset_cause;
}

sl_status_t label_configure_shelf_mode(const label_shelf_config_t *config) {
  /* BG27 channels are fixed to these pins. Wake bits start at bit 16, and
   * their positions are unrelated to the GPIO pin number. */
  static const struct {
    GPIO_Port_TypeDef port;
    unsigned int pin;
    unsigned int channel;
  } wake_pins[] = {
#if defined(GPIO_EM4WU0_PORT) && defined(GPIO_EM4WU0_PIN)
      {GPIO_EM4WU0_PORT, GPIO_EM4WU0_PIN, 0},
#endif
#if defined(GPIO_EM4WU3_PORT) && defined(GPIO_EM4WU3_PIN)
      {GPIO_EM4WU3_PORT, GPIO_EM4WU3_PIN, 3},
#endif
#if defined(GPIO_EM4WU4_PORT) && defined(GPIO_EM4WU4_PIN)
      {GPIO_EM4WU4_PORT, GPIO_EM4WU4_PIN, 4},
#endif
#if defined(GPIO_EM4WU6_PORT) && defined(GPIO_EM4WU6_PIN)
      {GPIO_EM4WU6_PORT, GPIO_EM4WU6_PIN, 6},
#endif
#if defined(GPIO_EM4WU7_PORT) && defined(GPIO_EM4WU7_PIN)
      {GPIO_EM4WU7_PORT, GPIO_EM4WU7_PIN, 7},
#endif
#if defined(GPIO_EM4WU8_PORT) && defined(GPIO_EM4WU8_PIN)
      {GPIO_EM4WU8_PORT, GPIO_EM4WU8_PIN, 8},
#endif
#if defined(GPIO_EM4WU9_PORT) && defined(GPIO_EM4WU9_PIN)
      {GPIO_EM4WU9_PORT, GPIO_EM4WU9_PIN, 9}
#endif
  };
  if (config == NULL) {
    return SL_STATUS_NULL_POINTER;
  }
  if (!power_status.initialized) {
    return SL_STATUS_NOT_INITIALIZED;
  }
#if SL_DEVICE_INIT_DCDC_ENABLE &&                                              \
    (SL_DEVICE_INIT_DCDC_TYPE == SL_DEVICE_INIT_DCDC_TYPE_BOOST)
  return SL_STATUS_NOT_SUPPORTED;
#endif
  for (unsigned int i = 0; i < sizeof(wake_pins) / sizeof(wake_pins[0]); i++) {
    if (config->port == wake_pins[i].port && config->pin == wake_pins[i].pin) {
      shelf_config = *config;
      shelf_wakeup_mask =
          1UL << (_GPIO_EM4WUEN_EM4WUEN_SHIFT + wake_pins[i].channel);
      return SL_STATUS_OK;
    }
  }
  return SL_STATUS_INVALID_PARAMETER;
}

sl_status_t label_enter_shelf_mode(void) {
  if (!power_status.initialized) {
    return SL_STATUS_NOT_INITIALIZED;
  }
  if (shelf_wakeup_mask == 0) {
    return SL_STATUS_NOT_READY;
  }
  if (__get_IPSR() != 0 || __get_PRIMASK() != 0 || __get_BASEPRI() != 0) {
    return SL_STATUS_INVALID_STATE;
  }
  if (power_status.em1_lock_count != 0) {
    return SL_STATUS_BUSY;
  }

  CMU_ClockEnable(cmuClock_GPIO, true);
  // EM4 wake requires unfiltered input. Bias it to the inactive level.
  GPIO_PinModeSet(shelf_config.port, shelf_config.pin, gpioModeInputPull,
                  shelf_config.active_high ? 0 : 1);
  if ((GPIO_PinInGet(shelf_config.port, shelf_config.pin) != 0) ==
      shelf_config.active_high) {
    return SL_STATUS_BUSY;
  }

  // Close the window against an ISR acquiring an application lock. A wake
  // input becoming active afterwards is a valid immediate reset/wake request.
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  if (power_status.em1_lock_count != 0) {
    CORE_EXIT_CRITICAL();
    return SL_STATUS_BUSY;
  }
  EMU_EM4Init_TypeDef em4_init = EMU_EM4INIT_DEFAULT;
  em4_init.pinRetentionMode =
      shelf_config.retain_gpio ? emuPinRetentionLatch : emuPinRetentionEm4Exit;
  EMU_EM4Init(&em4_init);
  GPIO_EM4DisablePinWakeup(_GPIO_EM4WUEN_EM4WUEN_MASK);
  GPIO_EM4EnablePinWakeup(shelf_wakeup_mask,
                          shelf_config.active_high ? shelf_wakeup_mask : 0);

  // SDK handles xG27 buck bypass and the EM4 entry sequence.
  sl_power_manager_enter_em4();
  for (;;) {
    // EM4 success never returns; do not resume with interrupts masked.
  }
}
