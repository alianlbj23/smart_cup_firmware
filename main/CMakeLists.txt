idf_component_register(SRCS "main.c" "cup_state.c" "ble_gatt_svr.c"
                    INCLUDE_DIRS "."
                    # ESP-IDF v6 的驅動拆成獨立元件，這裡明確列出用到的：
                    #   esp_driver_i2c  → driver/i2c_master.h（新版 I2C 主機驅動）
                    #   esp_driver_gpio → driver/gpio.h（IRQ / INT 計數）
                    #   esp_adc         → esp_adc/adc_oneshot.h、adc_cali（電池電壓）
                    #   esp_timer       → esp_timer_get_time()（即時顯示的時間欄）
                    #   bt              → NimBLE（ble_gatt_svr.c）
                    #   nvs_flash       → BLE 控制器存 PHY 校正資料
                    #   esp_driver_usb_serial_jtag → usb_serial_jtag_is_connected()（接電腦時不進深度睡眠）
                    #   esp_pm          → 自動淺眠（休眠時 BLE 連線保持）
                    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_adc esp_timer bt nvs_flash esp_driver_usb_serial_jtag esp_pm)
