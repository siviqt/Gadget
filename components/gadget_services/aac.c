// SPDX-License-Identifier: MIT
#include "gadget_services.h"

#include "esp_aac_dec.h"

static gadget_aac_decoder_t *open_decoder(void)
{
    esp_aac_dec_cfg_t config = ESP_AAC_DEC_CONFIG_DEFAULT();
    config.aac_plus_enable = true;
    void *decoder = NULL;
    return esp_aac_dec_open(&config, sizeof(config), &decoder) == ESP_AUDIO_ERR_OK ? decoder : NULL;
}

static int decode_frame(gadget_aac_decoder_t *decoder, uint8_t *frame, size_t length, int16_t *pcm,
                        size_t capacity_samples, gadget_pcm_format_t *format)
{
    esp_audio_dec_in_raw_t input = {.buffer = frame, .len = length};
    esp_audio_dec_out_frame_t output = {.buffer = (uint8_t *)pcm, .len = capacity_samples * sizeof(*pcm)};
    esp_audio_dec_info_t info = {0};
    if (esp_aac_dec_decode(decoder, &input, &output, &info) != ESP_AUDIO_ERR_OK ||
        input.consumed != length || output.decoded_size > output.len) {
        return -1;
    }
    if (output.decoded_size == 0) {
        return 0;
    }
    if (info.bits_per_sample != 16 || info.channel < 1 || info.channel > 2 ||
        output.decoded_size % (info.channel * sizeof(*pcm)) != 0) {
        return -1;
    }
    *format = (gadget_pcm_format_t){.sample_rate = info.sample_rate, .channels = info.channel};
    return output.decoded_size / (info.channel * sizeof(*pcm));
}

static void close_decoder(gadget_aac_decoder_t *decoder)
{
    esp_aac_dec_close(decoder);
}

const gadget_aac_api_t gadget_aac_api = {
    .open = open_decoder,
    .decode = decode_frame,
    .close = close_decoder,
};
