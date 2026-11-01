#pragma once
#include "driver/pulse_cnt.h"

//EC11引脚
#define PIN_EC_SW 20
#define PIN_EC_A 21
#define PIN_EC_B 22

class RotateEncoder{
public:
    RotateEncoder(){
        this->count = 0;
        this->last_count = 0;
    }

    void encoder_init(){
        pcnt_unit_config_t config = {};
        config.high_limit = 1000;
        config.low_limit = -1000;
        ESP_ERROR_CHECK(pcnt_new_unit(&config,&handle));

        pcnt_glitch_filter_config_t filter_config = {};
        filter_config.max_glitch_ns = 1000;
        ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(handle,&filter_config));

        pcnt_channel_handle_t chan_a_handle = nullptr;
        pcnt_chan_config_t chan_a_config = {};
        chan_a_config.edge_gpio_num = PIN_EC_A;
        chan_a_config.level_gpio_num = PIN_EC_B;
        ESP_ERROR_CHECK(pcnt_new_channel(handle,&chan_a_config,&chan_a_handle));
    
        pcnt_channel_set_edge_action(chan_a_handle,PCNT_CHANNEL_EDGE_ACTION_INCREASE,PCNT_CHANNEL_EDGE_ACTION_DECREASE);
        pcnt_channel_set_level_action(chan_a_handle,PCNT_CHANNEL_LEVEL_ACTION_INVERSE,PCNT_CHANNEL_LEVEL_ACTION_KEEP);

        pcnt_unit_enable(handle);
        pcnt_unit_clear_count(handle);
        pcnt_unit_start(handle);

        gpio_config_t sw_config = {};
        sw_config.intr_type = GPIO_INTR_NEGEDGE;
        sw_config.mode = GPIO_MODE_INPUT;
        sw_config.pin_bit_mask = (1ULL) << PIN_EC_SW;
        gpio_config(&sw_config);

    }

    int get_delta(){
        this->last_count = this->count;
        pcnt_unit_get_count(this->handle,&this->count);
        return count - last_count;
    }


    pcnt_unit_handle_t handle;
    int count;
    int last_count;
};