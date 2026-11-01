#pragma once

#include <iostream>
#include <stdint.h>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

class jpeg_unit{
public:
    uint8_t* buffer;
    size_t len;
};

using callback = std::function<void(const uint8_t* buf,size_t len)>;

class MjpegParser{
public:
    MjpegParser(size_t capacity,QueueHandle_t free,QueueHandle_t full):
        index(0),
        cap(capacity),
        in_frame(false),
        prev_ff(false),
        free_buffer(free),
        full_buffer(full){}

    void parser_init(){
        jpeg_unit jpeg = {};
        xQueueReceive(free_buffer,&jpeg,portMAX_DELAY);
        this->buffer = jpeg.buffer;
    }

    void feed(const uint8_t* data,size_t len,callback cb){
        for(int i = 0;i<len;i++){
            uint8_t b = data[i];
            if(this->in_frame){//在帧里，找FFD9结束标
                if(index < this->cap){
                    buffer[index] = b;
                    index ++;
                }
                else{
                    this->in_frame = false;
                    this->prev_ff = false;
                    this->index = 0;
                    continue;
                }
                if(prev_ff && b == 0xD9){
                    //if(cb) cb(buffer,index);
                    jpeg_unit jpeg = {};
                    if(xQueueReceive(free_buffer,&jpeg,0) != pdTRUE){
                        in_frame = false;
                        prev_ff = false;
                        index = 0;
                        continue;
                    }
                    uint8_t* temp = this->buffer;
                    this->buffer = jpeg.buffer;
                    jpeg.buffer = temp;
                    jpeg.len = index;


                    if(xQueueSend(full_buffer,&jpeg,0) != pdTRUE){
                        xQueueSend(free_buffer,&jpeg,0);
                    }
                    this->in_frame = false;
                    this->prev_ff = false;
                }
                else{
                    prev_ff = (b == 0xFF);
                }
            }   
            else{//不在帧里 找FF D8开始标
                if(prev_ff && b == 0xD8){
                    this->in_frame = true;
                    buffer[0] = 0xFF;
                    buffer[1] = 0xD8;
                    index = 2;
                    prev_ff = false;
                }
                else{
                    prev_ff = (b == 0xFF);
                }
            }
        }
    }

    void reset(){
        this->index = 0;    
        this->in_frame = false;
        this->prev_ff = false;
    }

private:
    size_t index;
    size_t cap;
    bool in_frame;
    bool prev_ff;
    uint8_t* buffer;
    QueueHandle_t free_buffer;
    QueueHandle_t full_buffer;
};