#include "config.h"
#include "app_assert.h"
#include "app_log.h"
#include "sl_bt_api.h"

const char* get_name_adress_type (sl_bt_gap_address_type_t type)
{
    switch (type)
    {
        case sl_bt_gap_public_address:
            return "Public_address";
        case sl_bt_gap_static_address:
            return "static_address";
        case sl_bt_gap_random_resolvable_address:
            return "random_resolvable_address";
        case sl_bt_gap_random_nonresolvable_address:
            return "random_nonresolvable_address";
        default:
            return "Unknown";
    }
}

void get_config_info_ble(void) {
  bd_addr identity_address;
  uint8_t identity_type;

  // Retrieve the device's registered identity address and its type (output parameters)
  sl_status_t status = sl_bt_gap_get_identity_address(&identity_address, &identity_type);
  app_assert_status(status);

  app_log_info("============================================================\r\n");
  app_log_info("               BLE DEVICE ADDRESS INFORMATION               \r\n");
  app_log_info("============================================================\r\n");
  app_log_info("[Active Identity Address]\r\n");
  app_log_info("  Type : %s (0x%02X)\r\n",
               get_name_adress_type((sl_bt_gap_address_type_t)identity_type),
               identity_type);
  // Standard BLE MAC format is printed MSB to LSB (addr[5] down to addr[0])
  app_log_info("  MAC  : %02X:%02X:%02X:%02X:%02X:%02X\r\n",
               identity_address.addr[5], identity_address.addr[4],
               identity_address.addr[3], identity_address.addr[2],
               identity_address.addr[1], identity_address.addr[0]);
  app_log_info("------------------------------------------------------------\r\n");
  app_log_info("[Bluetooth Core Spec: 4 Address Types Overview]\r\n");
  app_log_info("  1. public_address (0x00)              : Fixed IEEE factory MAC [ACTIVE]\r\n");
  app_log_info("  2. static_address (0x01)              : Random device address (MSB 2 bits = 11b)\r\n");
  app_log_info("  3. random_resolvable_address (0x02)   : Private RPA rotated via IRK (MSB 2 bits = 01b)\r\n");
  app_log_info("  4. random_nonresolvable_address (0x03): Ephemeral beacon address (MSB 2 bits = 00b)\r\n");
  app_log_info("============================================================\r\n");
}
