/*********************
 *      INCLUDES
 *********************/
#include "esp_log.h"
#include "esp_system.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "app_peripherals.h"
#include "crc32.h"
#include "string.h"
#include "littlefs_utils.h"

/* LCD drivers */
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_panel_nt35510.h"

/* Touch drivers */
#include "esp_lcd_touch_cst816s.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lcd_touch_gt1151.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_tt21100.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "app_peripherals"

#define OEM_PARTITION_LABEL "oem"
#define OEM_CONFIG_VERSION_FILE "version.txt"
#define OEM_CONFIG_PERIPHERALS_FILE "peripherals.bin"

/**********************
 *  STATIC VARIABLES
 **********************/

/* LCD IO and panel */
static esp_lcd_panel_io_handle_t lcd_io = NULL;
static esp_lcd_panel_handle_t lcd_panel = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;

/* LVGL display and touch */
static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;

/* Cached peripherals config */
static peripherals_config_t cached_peripherals_config = {0};
static int cached_version = 0;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static esp_err_t lcd_init(void);
static esp_err_t touch_init(void);
static esp_err_t lvgl_init(void);
static uint32_t checksum(peripherals_config_t *peripherals_config);

/**********************
 * GLOBAL PROTOTYPES
 **********************/
esp_err_t app_camera_init(void);
esp_err_t app_lvgl_init(void);
peripherals_config_t *app_peripherals_read();

/**********************
 *   STATIC FUNCTIONS
 **********************/
static esp_err_t lcd_init(void)
{
    peripherals_config_t *config = app_peripherals_read();
    esp_err_t ret = ESP_OK;

    /* LCD backlight */
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << config->lcd_module_pin_config.pin_bl};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    /* LCD initialization */
    ESP_LOGD(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = {
        .sclk_io_num = config->lcd_module_pin_config.pin_sclk,
        .mosi_io_num = config->lcd_module_pin_config.pin_mosi,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz =
            config->lcd_module_config.h_res *
            config->lcd_module_config.draw_buff_height *
            sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(
            config->lcd_module_config.spi_num,
            &buscfg,
            SPI_DMA_CH_AUTO),
        TAG,
        "SPI init failed");

    ESP_LOGD(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = config->lcd_module_pin_config.pin_dc,
        .cs_gpio_num = config->lcd_module_pin_config.pin_cs,
        .pclk_hz = config->lcd_module_config.pixel_clk_hz,
        .lcd_cmd_bits = config->lcd_module_config.cmd_bits,
        .lcd_param_bits = config->lcd_module_config.param_bits,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi(
                          (esp_lcd_spi_bus_handle_t)config->lcd_module_config.spi_num,
                          &io_config, &lcd_io),
                      err,
                      TAG,
                      "New panel IO failed");

    ESP_LOGD(TAG, "Install LCD driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = config->lcd_module_pin_config.pin_rst,
        .color_space = config->lcd_module_config.color_space,
        .bits_per_pixel = config->lcd_module_config.bits_per_pixel,
    };
    if (config->lcd_module == LCD_MODULE_ST7789)
    {
        ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io, &panel_config, &lcd_panel), err, TAG, "New panel failed");
    }
    else if (config->lcd_module == LCD_MODULE_SSD1306)
    {
        ESP_GOTO_ON_ERROR(esp_lcd_new_panel_ssd1306(lcd_io, &panel_config, &lcd_panel), err, TAG, "New panel failed");
    }
    else if (config->lcd_module == LCD_MODULE_NT35510)
    {
        ESP_GOTO_ON_ERROR(esp_lcd_new_panel_nt35510(lcd_io, &panel_config, &lcd_panel), err, TAG, "New panel failed");
    }
    else
    {
        ESP_LOGE(TAG, "Unsupported LCD module: %d", config->lcd_module);
        return ESP_FAIL;
    }

    esp_lcd_panel_reset(lcd_panel);
    esp_lcd_panel_init(lcd_panel);
    esp_lcd_panel_mirror(lcd_panel, false, false);
    esp_lcd_panel_disp_on_off(lcd_panel, true);

    /* LCD backlight on */
    ESP_ERROR_CHECK(gpio_set_level(config->lcd_module_pin_config.pin_bl, config->lcd_module_config.bl_on_level));

    return ret;

err:
    if (lcd_panel)
    {
        esp_lcd_panel_del(lcd_panel);
    }
    if (lcd_io)
    {
        esp_lcd_panel_io_del(lcd_io);
    }
    spi_bus_free(config->lcd_module_config.spi_num);
    return ret;
}
static esp_err_t touch_init(void)
{
    peripherals_config_t *config = app_peripherals_read();
    /* Initilize I2C */
    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = config->touch_module_config.i2c_sda,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_io_num = config->touch_module_config.i2c_scl,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = config->touch_module_config.i2c_clk_hz};
    ESP_RETURN_ON_ERROR(i2c_param_config(config->touch_module_config.i2c_num, &i2c_conf), TAG, "I2C configuration failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(config->touch_module_config.i2c_num, i2c_conf.mode, 0, 0, 0), TAG, "I2C initialization failed");

    /* Initialize touch HW */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = config->lcd_module_config.h_res,
        .y_max = config->lcd_module_config.v_res,
        .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
        .int_gpio_num = config->touch_module_config.gpio_int,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = config->touch_module_config.swap_xy,
            .mirror_x = config->touch_module_config.mirror_x,
            .mirror_y = config->touch_module_config.mirror_y,
        },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    if (config->touch_module == TOUCH_MODULE_CST816S)
    {
        const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)config->touch_module_config.i2c_num, &tp_io_config, &tp_io_handle), TAG, "");
        return esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &touch_handle);
    }
    else if (config->touch_module == TOUCH_MODULE_FT5X06)
    {
        const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)config->touch_module_config.i2c_num, &tp_io_config, &tp_io_handle), TAG, "");
        return esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &touch_handle);
    }
    else if (config->touch_module == TOUCH_MODULE_FT6X36)
    {
        const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)config->touch_module_config.i2c_num, &tp_io_config, &tp_io_handle), TAG, "");
        return esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &touch_handle);
    }
    else if (config->touch_module == TOUCH_MODULE_GT1151)
    {
        const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT1151_CONFIG();
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)config->touch_module_config.i2c_num, &tp_io_config, &tp_io_handle), TAG, "");
        return esp_lcd_touch_new_i2c_gt1151(tp_io_handle, &tp_cfg, &touch_handle);
    }
    else if (config->touch_module == TOUCH_MODULE_GT911)
    {
        const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)config->touch_module_config.i2c_num, &tp_io_config, &tp_io_handle), TAG, "");
        return esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &touch_handle);
    }
    else if (config->touch_module == TOUCH_MODULE_TT21100)
    {
        const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_TT21100_CONFIG();
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)config->touch_module_config.i2c_num, &tp_io_config, &tp_io_handle), TAG, "");
        return esp_lcd_touch_new_i2c_tt21100(tp_io_handle, &tp_cfg, &touch_handle);
    }
    else
    {
        ESP_LOGE(TAG, "Unsupported touch module: %d", config->touch_module);
        return ESP_FAIL;
    }
}
static esp_err_t lvgl_init(void)
{
    peripherals_config_t *config = app_peripherals_read();
    /* Initialize LVGL */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,       /* LVGL task priority */
        .task_stack = 1024 * 12,  /* LVGL task stack size, 12KB to avoid `stack overflow in task LVGL task` */
        .task_affinity = -1,      /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500, /* Maximum sleep in LVGL task */
        .timer_period_ms = 5      /* LVGL timer tick period in ms */
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port initialization failed");

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = config->lcd_module_config.h_res * config->lcd_module_config.draw_buff_height,
        .double_buffer = config->lcd_module_config.draw_buff_double,
        .hres = config->lcd_module_config.h_res,
        .vres = config->lcd_module_config.v_res,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = config->lcd_module_config.swap_xy,
            .mirror_x = config->lcd_module_config.mirror_x,
            .mirror_y = config->lcd_module_config.mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .swap_bytes = true,
        }};
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = touch_handle,
    };
    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);

    return ESP_OK;
}
static uint32_t checksum(peripherals_config_t *peripherals_config)
{
    size_t size = sizeof(peripherals_config_t) - sizeof(uint32_t);
    unsigned char *m = (unsigned char *)malloc(size);
    memcpy(m, peripherals_config, size);
    uint32_t c = crc32(0, m, size);
    free(m);
    return c;
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t app_camera_init(void)
{
    peripherals_config_t *config = app_peripherals_read();
    if (config == NULL)
    {
        ESP_LOGE(TAG, "failed to read peripherals config");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Camera module is %d", config->camera_module);

    if (config->camera_module == CAMERA_MODULE_ESP_EYE || config->camera_module == CAMERA_MODULE_ESP32_CAM_BOARD)
    {
        /* IO13, IO14 is designed for JTAG by default,
         * to use it as generalized input,
         * firstly declair it as pullup input */
        gpio_config_t conf;
        conf.mode = GPIO_MODE_INPUT;
        conf.pull_up_en = GPIO_PULLUP_ENABLE;
        conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        conf.intr_type = GPIO_INTR_DISABLE;
        conf.pin_bit_mask = 1LL << 13;
        gpio_config(&conf);
        conf.pin_bit_mask = 1LL << 14;
        gpio_config(&conf);
    }

    int CAMERA_PIN_PWDN = -1;
    int CAMERA_PIN_RESET = -1;
    int CAMERA_PIN_XCLK = -1;
    int CAMERA_PIN_SIOD = -1;
    int CAMERA_PIN_SIOC = -1;
    int CAMERA_PIN_D7 = -1;
    int CAMERA_PIN_D6 = -1;
    int CAMERA_PIN_D5 = -1;
    int CAMERA_PIN_D4 = -1;
    int CAMERA_PIN_D3 = -1;
    int CAMERA_PIN_D2 = -1;
    int CAMERA_PIN_D1 = -1;
    int CAMERA_PIN_D0 = -1;
    int CAMERA_PIN_VSYNC = -1;
    int CAMERA_PIN_HREF = -1;
    int CAMERA_PIN_PCLK = -1;

    if (config->camera_module == CAMERA_MODULE_WROVER_KIT)
    {
        CAMERA_PIN_PWDN = -1;
        CAMERA_PIN_RESET = -1;
        CAMERA_PIN_XCLK = 21;
        CAMERA_PIN_SIOD = 26;
        CAMERA_PIN_SIOC = 27;
        CAMERA_PIN_D7 = 35;
        CAMERA_PIN_D6 = 34;
        CAMERA_PIN_D5 = 39;
        CAMERA_PIN_D4 = 36;
        CAMERA_PIN_D3 = 19;
        CAMERA_PIN_D2 = 18;
        CAMERA_PIN_D1 = 5;
        CAMERA_PIN_D0 = 4;
        CAMERA_PIN_VSYNC = 25;
        CAMERA_PIN_HREF = 23;
        CAMERA_PIN_PCLK = 22;
    }
    else if (config->camera_module == CAMERA_MODULE_ESP_EYE)
    {
        CAMERA_PIN_PWDN = -1;
        CAMERA_PIN_RESET = -1;
        CAMERA_PIN_XCLK = 4;
        CAMERA_PIN_SIOD = 18;
        CAMERA_PIN_SIOC = 23;
        CAMERA_PIN_D7 = 36;
        CAMERA_PIN_D6 = 37;
        CAMERA_PIN_D5 = 38;
        CAMERA_PIN_D4 = 39;
        CAMERA_PIN_D3 = 35;
        CAMERA_PIN_D2 = 14;
        CAMERA_PIN_D1 = 13;
        CAMERA_PIN_D0 = 34;
        CAMERA_PIN_VSYNC = 5;
        CAMERA_PIN_HREF = 27;
        CAMERA_PIN_PCLK = 25;
    }
    else if (config->camera_module == CAMERA_MODULE_ESP_S2_KALUGA)
    {
        CAMERA_PIN_PWDN = -1;
        CAMERA_PIN_RESET = -1;
        CAMERA_PIN_XCLK = 1;
        CAMERA_PIN_SIOD = 8;
        CAMERA_PIN_SIOC = 7;
        CAMERA_PIN_D7 = 38;
        CAMERA_PIN_D6 = 21;
        CAMERA_PIN_D5 = 40;
        CAMERA_PIN_D4 = 39;
        CAMERA_PIN_D3 = 42;
        CAMERA_PIN_D2 = 41;
        CAMERA_PIN_D1 = 37;
        CAMERA_PIN_D0 = 36;
        CAMERA_PIN_VSYNC = 2;
        CAMERA_PIN_HREF = 3;
        CAMERA_PIN_PCLK = 33;
    }
    else if (config->camera_module == CAMERA_MODULE_ESP_S3_EYE)
    {
        CAMERA_PIN_PWDN = -1;
        CAMERA_PIN_RESET = -1;
        CAMERA_PIN_VSYNC = 6;
        CAMERA_PIN_HREF = 7;
        CAMERA_PIN_PCLK = 13;
        CAMERA_PIN_XCLK = 15;
        CAMERA_PIN_SIOD = 4;
        CAMERA_PIN_SIOC = 5;
        CAMERA_PIN_D0 = 11;
        CAMERA_PIN_D1 = 9;
        CAMERA_PIN_D2 = 8;
        CAMERA_PIN_D3 = 10;
        CAMERA_PIN_D4 = 12;
        CAMERA_PIN_D5 = 18;
        CAMERA_PIN_D6 = 17;
        CAMERA_PIN_D7 = 16;
    }
    else if (config->camera_module == CAMERA_MODULE_ESP32_CAM_BOARD)
    {
        CAMERA_PIN_PWDN = 32;
        CAMERA_PIN_RESET = 33;
        CAMERA_PIN_XCLK = 4;
        CAMERA_PIN_SIOD = 18;
        CAMERA_PIN_SIOC = 23;
        CAMERA_PIN_D7 = 36;
        CAMERA_PIN_D6 = 19;
        CAMERA_PIN_D5 = 21;
        CAMERA_PIN_D4 = 39;
        CAMERA_PIN_D3 = 35;
        CAMERA_PIN_D2 = 14;
        CAMERA_PIN_D1 = 13;
        CAMERA_PIN_D0 = 34;
        CAMERA_PIN_VSYNC = 5;
        CAMERA_PIN_HREF = 27;
        CAMERA_PIN_PCLK = 25;
    }
    else if (config->camera_module == CAMERA_MODULE_M5STACK_PSRAM)
    {
        CAMERA_PIN_PWDN = -1;
        CAMERA_PIN_RESET = 15;
        CAMERA_PIN_XCLK = 27;
        CAMERA_PIN_SIOD = 25;
        CAMERA_PIN_SIOC = 23;
        CAMERA_PIN_D7 = 19;
        CAMERA_PIN_D6 = 36;
        CAMERA_PIN_D5 = 18;
        CAMERA_PIN_D4 = 39;
        CAMERA_PIN_D3 = 5;
        CAMERA_PIN_D2 = 34;
        CAMERA_PIN_D1 = 35;
        CAMERA_PIN_D0 = 32;
        CAMERA_PIN_VSYNC = 22;
        CAMERA_PIN_HREF = 26;
        CAMERA_PIN_PCLK = 21;
    }
    else if (config->camera_module == CAMERA_MODULE_M5STACK_WIDE)
    {
        CAMERA_PIN_PWDN = -1;
        CAMERA_PIN_RESET = 15;
        CAMERA_PIN_XCLK = 27;
        CAMERA_PIN_SIOD = 22;
        CAMERA_PIN_SIOC = 23;
        CAMERA_PIN_D7 = 19;
        CAMERA_PIN_D6 = 36;
        CAMERA_PIN_D5 = 18;
        CAMERA_PIN_D4 = 39;
        CAMERA_PIN_D3 = 5;
        CAMERA_PIN_D2 = 34;
        CAMERA_PIN_D1 = 35;
        CAMERA_PIN_D0 = 32;
        CAMERA_PIN_VSYNC = 25;
        CAMERA_PIN_HREF = 26;
        CAMERA_PIN_PCLK = 21;
    }
    else if (config->camera_module == CAMERA_MODULE_AI_THINKER)
    {
        CAMERA_PIN_PWDN = 32;
        CAMERA_PIN_RESET = -1;
        CAMERA_PIN_XCLK = 0;
        CAMERA_PIN_SIOD = 26;
        CAMERA_PIN_SIOC = 27;
        CAMERA_PIN_D7 = 35;
        CAMERA_PIN_D6 = 34;
        CAMERA_PIN_D5 = 39;
        CAMERA_PIN_D4 = 36;
        CAMERA_PIN_D3 = 21;
        CAMERA_PIN_D2 = 19;
        CAMERA_PIN_D1 = 18;
        CAMERA_PIN_D0 = 5;
        CAMERA_PIN_VSYNC = 25;
        CAMERA_PIN_HREF = 23;
        CAMERA_PIN_PCLK = 22;
    }
    else if (config->camera_module == CAMERA_MODULE_CUSTOM)
    {
        CAMERA_PIN_PWDN = config->camera_module_custom_config.pin_pwdn;
        CAMERA_PIN_RESET = config->camera_module_custom_config.pin_reset;
        CAMERA_PIN_XCLK = config->camera_module_custom_config.pin_xclk;
        CAMERA_PIN_SIOD = config->camera_module_custom_config.pin_siod;
        CAMERA_PIN_SIOC = config->camera_module_custom_config.pin_sioc;
        CAMERA_PIN_D7 = config->camera_module_custom_config.pin_d7;
        CAMERA_PIN_D6 = config->camera_module_custom_config.pin_d6;
        CAMERA_PIN_D5 = config->camera_module_custom_config.pin_d5;
        CAMERA_PIN_D4 = config->camera_module_custom_config.pin_d4;
        CAMERA_PIN_D3 = config->camera_module_custom_config.pin_d3;
        CAMERA_PIN_D2 = config->camera_module_custom_config.pin_d2;
        CAMERA_PIN_D1 = config->camera_module_custom_config.pin_d1;
        CAMERA_PIN_D0 = config->camera_module_custom_config.pin_d0;
        CAMERA_PIN_VSYNC = config->camera_module_custom_config.pin_vsync;
        CAMERA_PIN_HREF = config->camera_module_custom_config.pin_href;
        CAMERA_PIN_PCLK = config->camera_module_custom_config.pin_pclk;
    }
    else
    {
        ESP_LOGE(TAG, "Unsupported camera module");
        return ESP_FAIL;
    }

    camera_config_t camera_config;
    camera_config.ledc_channel = LEDC_CHANNEL_0;
    camera_config.ledc_timer = LEDC_TIMER_0;
    camera_config.pin_d0 = CAMERA_PIN_D0;
    camera_config.pin_d1 = CAMERA_PIN_D1;
    camera_config.pin_d2 = CAMERA_PIN_D2;
    camera_config.pin_d3 = CAMERA_PIN_D3;
    camera_config.pin_d4 = CAMERA_PIN_D4;
    camera_config.pin_d5 = CAMERA_PIN_D5;
    camera_config.pin_d6 = CAMERA_PIN_D6;
    camera_config.pin_d7 = CAMERA_PIN_D7;
    camera_config.pin_xclk = CAMERA_PIN_XCLK;
    camera_config.pin_pclk = CAMERA_PIN_PCLK;
    camera_config.pin_vsync = CAMERA_PIN_VSYNC;
    camera_config.pin_href = CAMERA_PIN_HREF;
    camera_config.pin_sccb_sda = CAMERA_PIN_SIOD;
    camera_config.pin_sccb_scl = CAMERA_PIN_SIOC;
    camera_config.pin_pwdn = CAMERA_PIN_PWDN;
    camera_config.pin_reset = CAMERA_PIN_RESET;
    camera_config.xclk_freq_hz = config->camera_module_config.xclk_freq_hz;
    camera_config.pixel_format = config->camera_module_config.pixelformat;
    camera_config.frame_size = config->camera_module_config.framesize;
    camera_config.jpeg_quality = 10;
    camera_config.fb_count = config->camera_module_config.fb_count;
    camera_config.fb_location = CAMERA_FB_IN_PSRAM;
    camera_config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    // camera init
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return ESP_FAIL;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID || s->id.PID == OV2640_PID)
        s->set_vflip(s, 1); // flip it back
    else if (s->id.PID == GC0308_PID)
    {
        s->set_hmirror(s, 0);
    }
    else if (s->id.PID == GC032A_PID)
    {
        s->set_vflip(s, 1);
        // s->set_hmirror(s, 0); //something wrong
    }

    if (s->id.PID == OV3660_PID)
    {
        s->set_brightness(s, 2);
        s->set_contrast(s, 3);
    }

    return ESP_OK;
}
esp_err_t app_lvgl_init(void)
{
    peripherals_config_t *config = app_peripherals_read();
    if (config == NULL)
    {
        ESP_LOGE(TAG, "failed to read peripherals config");
        return ESP_FAIL;
    }

    /* LCD HW initialization */
    ESP_ERROR_CHECK(lcd_init());

    /* Touch initialization */
    ESP_ERROR_CHECK(touch_init());

    /* LVGL initialization */
    ESP_ERROR_CHECK(lvgl_init());

    return ESP_OK;
}
peripherals_config_t *app_peripherals_read()
{
    if (cached_peripherals_config.check_sum == 0)
    {
        // read version
        uint8_t *version_buffer = NULL;
        size_t version_size = 0;
        bool ret = littlefs_read_file(OEM_PARTITION_LABEL, OEM_CONFIG_VERSION_FILE, &version_buffer, &version_size);
        if (!ret)
        {
            return NULL;
        }
        if (version_size != 10)
        {
            free(version_buffer);
            version_buffer = NULL;
            return NULL;
        }
        cached_version = atoi((char *)version_buffer);
        free(version_buffer);
        version_buffer = NULL;

        // read peripherals
        uint8_t *config_buffer = NULL;
        size_t size = 0;
        ret = littlefs_read_file(OEM_PARTITION_LABEL, OEM_CONFIG_PERIPHERALS_FILE, &config_buffer, &size);
        if (!ret)
        {
            return NULL;
        }
        if (size != sizeof(peripherals_config_t))
        {
            free(config_buffer);
            config_buffer = NULL;
            return NULL;
        }
        memcpy(&cached_peripherals_config, config_buffer, size);
        free(config_buffer);
        config_buffer = NULL;
    }
    if (cached_peripherals_config.check_sum == 0)
    {
        return NULL;
    }
    return &cached_peripherals_config;
}
