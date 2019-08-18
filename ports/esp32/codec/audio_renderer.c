/*
 * audio_renderer.c
 *
 *  Created on: 2019.02.03
 *      Author: zhaohuijiang
 */

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "audio_player.h"
#include "audio_renderer.h"
#include "http_client.h"
#define TAG "renderer"

#define I2S_ADC_CHANNEL ADC1_CHANNEL_2

renderer_config_t *renderer_instance = NULL;
static QueueHandle_t i2s_event_queue;
extern HTTP_HEAD_VAL http_head[6];

static void init_i2s(renderer_config_t *config)
{
    i2s_mode_t mode = I2S_MODE_MASTER;
    i2s_comm_format_t comm_fmt = I2S_COMM_FORMAT_I2S; //I2S_COMM_FORMAT_I2S_MSB;

    if ((config->mode == DAC_BUILT_IN) || (config->mode == ADC_DAC_BUILT_IN))
    {
        mode = mode | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN;
    }
    else if ((config->mode == ADC_BUILT_IN) || (config->mode == ADC_DAC_BUILT_IN))
    {
        mode = mode | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN;
    }
    else if ((config->mode == DAC) || (config->mode == ADC_DAC))
    {
        //ESP_LOGE(TAG, "I2S master TX mode.");
        mode = mode | I2S_MODE_TX; 
    }
    else if ((config->mode == ADC) || (config->mode == ADC_DAC))
    {
        //ESP_LOGE(TAG, "I2S master RX mode.");
        mode = mode | I2S_MODE_RX;
    }

    /*
     * Allocate just enough to decode AAC+, which has huge frame sizes.
     *
     * Memory consumption formula:
     * (bits_per_sample / 8) * num_chan * dma_buf_count * dma_buf_len
     *
     * 16 bit: 32 * 256 = 8192 bytes
     * 32 bit: 32 * 256 = 16384 bytes
     */
    i2s_config_t i2s_config = {
        .mode = mode,                                 // I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN,
        .sample_rate = config->sample_rate,           //44100 default
        .bits_per_sample = config->bit_depth,         //16bit default
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // I2S_COMM_FORMAT_I2S_MSB 2-channels
        .communication_format = comm_fmt,
        .dma_buf_count = 16,                     //2,             //32 number of buffers, 128 max.
        .dma_buf_len = 128,                      //1024,          //64 size of each buffer
        .use_apll = config->use_apll,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1 // Interrupt level 1 ESP_INTR_FLAG_LEVEL0?
    };

    i2s_driver_uninstall(config->i2s_num);
    i2s_driver_install(config->i2s_num, &i2s_config, 1, &i2s_event_queue);
    i2s_set_clk(config->i2s_num, config->sample_rate, config->bit_depth, config->i2s_channal_nums);
    //i2s_set_pin(config->i2s_num, NULL);
    if((config->mode == ADC) || (config->mode == DAC) ||(config->mode == ADC_DAC))
    {
        i2s_pin_config_t i2s_pin_cfg;
        i2s_pin_cfg.bck_io_num = GPIO_NUM_18; //5;
        i2s_pin_cfg.ws_io_num = GPIO_NUM_5; //25;
        i2s_pin_cfg.data_out_num = GPIO_NUM_32; //26;
        i2s_pin_cfg.data_in_num = GPIO_NUM_35; //35;
        i2s_set_pin(config->i2s_num, &i2s_pin_cfg);
        // i2s_mclk_gpio_select(config->i2s_num, GPIO_NUM_0);
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0_CLK_OUT1);
        WRITE_PERI_REG(PIN_CTRL, 0xFFF0);
    }
    
    if ((config->mode == DAC_BUILT_IN) || (config->mode == ADC_DAC_BUILT_IN))
    {
        i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
    }
    else if (((config->mode == ADC_BUILT_IN) || (config->mode == ADC_DAC_BUILT_IN)))
    {
        i2s_set_adc_mode(renderer_instance->adc_num, renderer_instance->adc_channel_num );
    }

    i2s_zero_dma_buffer(0);
    i2s_stop(config->i2s_num);
    // ESP_LOGE(TAG, "1.1 Create after i2s driver install: RAM left 1 %d", esp_get_free_heap_size());
}

/**
 * I2S is MSB first (big-endian) two's complement (signed) integer format.
 * The I2S module receives and transmits left-channel data first.
 *
 * ESP32 is little-endian.
 */
/*
void render_samples(char *buf, uint32_t buf_len, pcm_format_t *buf_desc)
{
    //ESP_LOGI(TAG, "buf_desc: bit_depth %d format %d num_chan %d sample_rate %d", buf_desc->bit_depth, buf_desc->buffer_format, buf_desc->num_channels, buf_desc->sample_rate);
    //ESP_LOGI(TAG, "renderer_instance: bit_depth %d, output_mode %d", renderer_instance->bit_depth, renderer_instance->output_mode);

    // handle changed sample rate
    if (renderer_instance->sample_rate != buf_desc->sample_rate)
    {
        ESP_LOGI(TAG, "changing sample rate from %d to %d", renderer_instance->sample_rate, buf_desc->sample_rate);
        uint32_t rate = buf_desc->sample_rate * renderer_instance->sample_rate_modifier;
        i2s_set_sample_rates(renderer_instance->i2s_num, rate);
        renderer_instance->sample_rate = buf_desc->sample_rate;
    }

    uint8_t buf_bytes_per_sample = (buf_desc->bit_depth / 8);
    uint32_t num_samples = buf_len / buf_bytes_per_sample / buf_desc->num_channels;

    // formats match, we can write the whole block
    if (buf_desc->bit_depth == renderer_instance->bit_depth && buf_desc->buffer_format == PCM_INTERLEAVED && buf_desc->num_channels == 2 && renderer_instance->mode != DAC_BUILT_IN)
    {

        // do not wait longer than the duration of the buffer
        TickType_t max_wait = buf_desc->sample_rate / num_samples / 2;

        // don't block, rather retry
        int bytes_left = buf_len;
        int bytes_written = 0;
        while (bytes_left > 0)
        {
            bytes_written = i2s_write_bytes(renderer_instance->i2s_num, buf, bytes_left, 0);
            bytes_left -= bytes_written;
            buf += bytes_written;
        }

        return;
    }

    // support only 16 bit buffers for now
    if (buf_desc->bit_depth != I2S_BITS_PER_SAMPLE_16BIT)
    {
        // ESP_LOGE(TAG, "unsupported decoder bit depth: %d", buf_desc->bit_depth);
        return;
    }

    // pointer to left / right sample position
    char *ptr_l = buf;
    char *ptr_r = buf + buf_bytes_per_sample;
    uint8_t stride = buf_bytes_per_sample * 2;

    // right half of the buffer contains all the right channel samples
    if (buf_desc->buffer_format == PCM_LEFT_RIGHT)
    {
        ptr_r = buf + buf_len / 2;
        stride = buf_bytes_per_sample;
    }

    if (buf_desc->num_channels == 1)
    {
        ptr_r = ptr_l;
    }

    int bytes_pushed = 0;
    for (int i = 0; i < num_samples; i++)
    {

        if (renderer_instance->mode == DAC_BUILT_IN)
        {
            // assume 16 bit src bit_depth
            short left = *(short *)ptr_l;
            short right = *(short *)ptr_r;

            // The built-in DAC wants unsigned samples, so we shift the range
            // from -32768-32767 to 0-65535.
            left = left + 0x8000;
            right = right + 0x8000;

            uint32_t sample = (uint16_t)left;
            sample = (sample << 16 & 0xffff0000) | ((uint16_t)right);

            bytes_pushed = i2s_push_sample(renderer_instance->i2s_num, (const char *)&sample, portMAX_DELAY);
        }
        else
        {

            switch (renderer_instance->bit_depth)
            {
            case I2S_BITS_PER_SAMPLE_16BIT:; // workaround

                //  low - high / low - high 
                const char samp32[4] = {ptr_l[0], ptr_l[1], ptr_r[0], ptr_r[1]};

                bytes_pushed = i2s_push_sample(renderer_instance->i2s_num, (const char *)&samp32, portMAX_DELAY);
                break;

            case I2S_BITS_PER_SAMPLE_32BIT:; // workaround

                const char samp64[8] = {0, 0, ptr_l[0], ptr_l[1], 0, 0, ptr_r[0], ptr_r[1]};
                bytes_pushed = i2s_push_sample(renderer_instance->i2s_num, (const char *)&samp64, portMAX_DELAY);
                break;

            default:
                // ESP_LOGE(TAG, "bit depth unsupported: %d", renderer_instance->bit_depth);
                break;
            }
        }

        // DMA buffer full - retry
        if (bytes_pushed == 0)
        {
            i--;
        }
        else
        {
            ptr_r += stride;
            ptr_l += stride;
        }
    }

    //  takes too long
    // i2s_event_t evt = {0};
    // if(xQueueReceive(i2s_event_queue, &evt, 0)) {
    //     if(evt.type == I2S_EVENT_TX_DONE) {
    //         ESP_LOGE(TAG, "DMA Buffer Underflow");
    //     }
    // }
    
}
*/

void renderer_zero_dma_buffer()
{
    i2s_zero_dma_buffer(renderer_instance->i2s_num);
}

renderer_config_t *renderer_get()
{
    return renderer_instance;
}

/* init renderer sink */
void renderer_init(renderer_config_t *config)
{
    // update global
    renderer_instance = config;

    // ESP_LOGE(TAG, "init I2S , port %d, %d bit, %d Hz",  renderer_instance->i2s_num, renderer_instance->bit_depth, renderer_instance->sample_rate);
    init_i2s(config);
}

void renderer_start()
{
    i2s_start(renderer_instance->i2s_num);
    // buffer might contain noise
    i2s_zero_dma_buffer(renderer_instance->i2s_num);
}

void renderer_stop()
{
    i2s_stop(renderer_instance->i2s_num);
}

void renderer_destroy()
{
    if(renderer_instance != NULL)
    {
        i2s_stop(renderer_instance->i2s_num);
        i2s_driver_uninstall(renderer_instance->i2s_num);
        free(renderer_instance);
        renderer_instance = NULL;
    }
}

void renderer_adc_enable()
{
    i2s_adc_enable(renderer_instance->i2s_num);
}

void renderer_adc_disable()
{
    i2s_adc_disable(renderer_instance->i2s_num);
}

void renderer_read_raw(uint8_t *buff, uint32_t len)
{
    size_t bytes_read;
    i2s_read(renderer_instance->i2s_num, (void *)buff, len, &bytes_read, portMAX_DELAY);
}

void i2s_adc_data_scale(uint8_t * d_buff, uint8_t* s_buff, uint32_t len)
{
    uint32_t j = 0;
    uint16_t adc_val;

    if(renderer_instance->bit_depth == I2S_BITS_PER_SAMPLE_16BIT) 
    { 
        // memcpy(d_buff, s_buff, len);
        for (int i = 0; i < len; i += 2) {
            adc_val = ((((uint16_t) (s_buff[i + 1] & 0xff) << 8) | ((s_buff[i + 0]))));
            adc_val = (adc_val - 2048 - 300) << 4; 
            d_buff[j++] = adc_val & 0xff;
            d_buff[j++] = (adc_val >> 8) & 0xff;
            // printf("%02x", s_buff[i]);
            // printf("%02x", (uint8_t)(adc_val * 256 / 4096));
            // printf("%d\n", (dac_value - 2048 - 300) << 2);
        }
    }
}

void i2s_adc_data_scale1(uint16_t * d_buff, uint8_t* s_buff, uint32_t len)
{
    uint32_t j = 0;
    int16_t adc_val;

    if(renderer_instance->bit_depth == I2S_BITS_PER_SAMPLE_16BIT) 
    {
        // memcpy(d_buff, s_buff, len);
        for (int i = 0; i < len; i += 2) {
            adc_val = (int16_t) (((s_buff[i + 1] & 0xff) << 8) | s_buff[i + 0]);
            // adc_val = (adc_val - 2048 - 300) << 2;
            // adc_val = (adc_val < 0)? 0:adc_val;
            d_buff[j++] = abs(adc_val);
            // printf("%d ", adc_val);
        }
        // printf("\r\n");
    }
}