#include <stdio.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "custom_certificates.h"
#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "portmacro.h"
#include "wifi_connection.h"
#include "wifi_remote.h"

// Constants
static char const TAG[] = "main";

// Global variables
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb                   = {0};
static QueueHandle_t                input_event_queue    = NULL;

#if defined(CONFIG_BSP_TARGET_KAMI)
// Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
static pax_col_t palette[] = {0xffffffff, 0xff000000, 0xffff0000};  // white, black, red
#endif

void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

#define CAN_RX   4
#define CAN_TX   15
#define CAN_MODE 2

void can_initialize(bool high_speed) {
    // Transceiver mode pin
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << CAN_MODE),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(CAN_MODE, !high_speed);

    // Driver
    twai_general_config_t g_config = {.mode           = TWAI_MODE_NORMAL,
                                      .tx_io          = CAN_TX,
                                      .rx_io          = CAN_RX,
                                      .clkout_io      = TWAI_IO_UNUSED,
                                      .bus_off_io     = TWAI_IO_UNUSED,
                                      .tx_queue_len   = 5,
                                      .rx_queue_len   = 5,
                                      .alerts_enabled = TWAI_ALERT_RX_DATA,
                                      .clkout_divider = 0,
                                      .intr_flags     = ESP_INTR_FLAG_LEVEL1};

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
    ESP_ERROR_CHECK(twai_start());
}

void write_digital_outputs(uint8_t value) {
    // Create and send a message
    twai_message_t message = {.identifier       = 0x101,
                              .data_length_code = 8,
                              .data             = {value, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                              .flags            = TWAI_MSG_FLAG_NONE};

    if (twai_transmit(&message, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ESP_LOGI(TAG, "Message sent");
    } else {
        ESP_LOGE(TAG, "Transmit failed");
    }
}

void request_read_digital_inputs(void) {
    // Create and send a message
    twai_message_t message = {.identifier       = 0x301,
                              .data_length_code = 8,
                              .data             = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                              .flags            = TWAI_MSG_FLAG_NONE};

    if (twai_transmit(&message, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ESP_LOGI(TAG, "Message sent");
    } else {
        ESP_LOGE(TAG, "Transmit failed");
    }
}

uint8_t input_value = 0;

void can_rx_task(void* arg) {
    uint32_t       alerts;
    twai_message_t rx_msg;
    while (1) {
        // Wait for an alert (with timeout)
        if (twai_read_alerts(&alerts, pdMS_TO_TICKS(1000)) == ESP_OK) {
            if (alerts & TWAI_ALERT_RX_DATA) {
                // Message is ready in RX queue
                if (twai_receive(&rx_msg, 0) == ESP_OK) {
                    ESP_LOGI(TAG, "Received ID=0x%03X, DLC=%d", rx_msg.identifier, rx_msg.data_length_code);
                    for (int i = 0; i < rx_msg.data_length_code; i++) {
                        printf("%02X ", rx_msg.data[i]);
                    }
                    printf("\n");

                    if (rx_msg.identifier == 0x301 && rx_msg.data_length_code >= 1) {
                        printf("Digital inputs: 0x%02x\r\n", rx_msg.data[0]);
                        input_value = rx_msg.data[0];
                    }
                }
            }
        }
    }
}

void app_main(void) {
    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage partition
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        res = nvs_flash_erase();
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS flash: %d", res);
            return;
        }
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %d", res);
        return;
    }

    // Initialize the Board Support Package
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %d", res);
        return;
    }

    // Get display parameters and rotation
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get display parameters: %d", res);
        return;
    }

    // Convert ESP-IDF color format into PAX buffer type
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (display_color_format) {
        case LCD_COLOR_PIXEL_FORMAT_RGB565:
            format = PAX_BUF_16_565RGB;
            break;
        case LCD_COLOR_PIXEL_FORMAT_RGB888:
            format = PAX_BUF_24_888RGB;
            break;
        default:
            break;
    }

    // Convert BSP display rotation format into PAX orientation type
    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
    pax_orientation_t      orientation      = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:
            orientation = PAX_O_ROT_CCW;
            break;
        case BSP_DISPLAY_ROTATION_180:
            orientation = PAX_O_ROT_HALF;
            break;
        case BSP_DISPLAY_ROTATION_270:
            orientation = PAX_O_ROT_CW;
            break;
        case BSP_DISPLAY_ROTATION_0:
        default:
            orientation = PAX_O_UPRIGHT;
            break;
    }

        // Initialize graphics stack
#if defined(CONFIG_BSP_TARGET_KAMI)
    // Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
    format = PAX_BUF_2_PAL;
#endif
    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
#if defined(CONFIG_BSP_TARGET_KAMI)
    // Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
    fb.palette      = palette;
    fb.palette_size = sizeof(palette) / sizeof(pax_col_t);
#endif
    pax_buf_set_orientation(&fb, orientation);

#if defined(CONFIG_BSP_TARGET_KAMI)
#define BLACK 0
#define WHITE 1
#define RED   2
#else
#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF
#define RED   0xFFFF0000
#endif

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    can_initialize(true);  // Initialize CAN bus in high speed mode
    xTaskCreate(can_rx_task, "can_rx_task", 4096, NULL, 10, NULL);

    // Main section of the app

    // This example shows how to read from the BSP event queue to read input events

    // If you want to run something at an interval in this same main thread you can replace portMAX_DELAY with an amount
    // of ticks to wait, for example pdMS_TO_TICKS(1000)

    pax_background(&fb, WHITE);
    pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "CAN bus test");
    blit();

    uint8_t value = 0;

    uint8_t prev_input = 0;

    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_KEYBOARD: {
                    switch (event.args_keyboard.ascii) {
                        case '1':
                            value |= 0x01;
                            break;
                        case '2':
                            value |= 0x02;
                            break;
                        case '3':
                            value |= 0x04;
                            break;
                        case '4':
                            value |= 0x08;
                            break;
                        case 'q':
                            value &= ~0x01;
                            break;
                        case 'w':
                            value &= ~0x02;
                            break;
                        case 'e':
                            value &= ~0x04;
                            break;
                        case 'r':
                            value &= ~0x08;
                            break;
                        default:
                            break;
                    }
                    write_digital_outputs(value);
                    pax_simple_rect(&fb, WHITE, 0, 0, pax_buf_get_width(&fb), 72);
                    char text[64];
                    snprintf(text, sizeof(text), "Value: 0x%02x", value);
                    pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 18, text);
                    blit();
                    break;
                }
                default:
                    break;
            }
        } else {
            // Turn on the relay when input one is active, turn it off if input two is active
            if (input_value != prev_input) {
                if (input_value & 1) {
                    value &= ~0x01;
                }
                if (input_value & 2) {
                    value |= 0x01;
                }
                write_digital_outputs(value);
                prev_input = input_value;
            }
            // Poll the input state
            request_read_digital_inputs();
        }
    }
}
