#include "neopixel.h"

#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"

static const char *TAG = "neopixel";

/* RMT channel & encoder – allocated once by neopixel_init() */
static rmt_channel_handle_t s_led_chan  = NULL;
static rmt_encoder_handle_t s_encoder   = NULL;
static bool s_initialised               = false;
static int  s_gpio                      = -1;

/* Pixel data buffer for a single WS2812 (3 bytes: G, R, B). */
static uint8_t s_pixel_data[3];

#define RMT_RESOLUTION_HZ  10000000  /* 10 MHz → 1 tick = 0.1 µs */

esp_err_t neopixel_init(int gpio)
{
    /* Idempotent: if already running on the same GPIO, skip. */
    if (s_initialised && s_gpio == gpio) {
        return ESP_OK;
    }

    /* If re-initialising with a different GPIO, tear down first. */
    if (s_initialised) {
        rmt_disable(s_led_chan);
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_led_chan);
        s_initialised = false;
        s_led_chan = NULL;
        s_encoder  = NULL;
    }

    /* GPIO -1 means "disabled". */
    if (gpio < 0) {
        s_gpio = gpio;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "initialising neopixel on GPIO %d", gpio);

    /* Create RMT TX channel. */
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src          = RMT_CLK_SRC_DEFAULT,
        .gpio_num         = gpio,
        .mem_block_symbols = 64,
        .resolution_hz    = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &s_led_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Create LED strip encoder. */
    led_strip_encoder_config_t enc_cfg = {
        .resolution = RMT_RESOLUTION_HZ,
    };
    err = rmt_new_led_strip_encoder(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_led_strip_encoder failed: %s", esp_err_to_name(err));
        rmt_del_channel(s_led_chan);
        s_led_chan = NULL;
        return err;
    }

    /* Enable the channel. */
    err = rmt_enable(s_led_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_led_chan);
        s_encoder  = NULL;
        s_led_chan = NULL;
        return err;
    }

    s_gpio        = gpio;
    s_initialised = true;
    return ESP_OK;
}

esp_err_t neopixel_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialised || s_gpio < 0) {
        /* Neopixel not configured — silently ignore. */
        return ESP_OK;
    }

    /* WS2812 byte order in memory: GRB. */
    s_pixel_data[0] = g;
    s_pixel_data[1] = r;
    s_pixel_data[2] = b;

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,  /* single shot */
    };

    ESP_LOGD(TAG, "neopixel -> R=%d G=%d B=%d", r, g, b);
    return rmt_transmit(s_led_chan, s_encoder, s_pixel_data, sizeof(s_pixel_data), &tx_config);
}
