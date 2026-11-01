#pragma once

#include <iostream>
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "driver/jpeg_decode.h"
#include "sys/mman.h"
#include "parser.hpp"
#include <stdint.h>
#include "semaphore.h"

extern SemaphoreHandle_t dma2d_mutex;

class rgb_unit{
public:
    uint8_t* buffer;
};


class Decoder{
public:
    void decoder_init(){
        // esp_video 组件内部把 JPEG 解码超时写死为 40ms；加入 LVGL 后 640x480 解码偶发超过 40ms，
        // 超时会走 dma2d_force_end 错误路径，与 DMA2D RX 中断竞态，触发 dma2d.c:309 的 assert 崩溃。
        // 因此这里手动创建解码引擎，把超时调大，再通过 dec_handle 交给 esp_video 复用。
        jpeg_decode_engine_cfg_t dec_eng_cfg = {};
        dec_eng_cfg.intr_priority = 0;
        dec_eng_cfg.timeout_ms = -1;   // -1 = 永久等待，彻底规避超时路径
        if(jpeg_new_decoder_engine(&dec_eng_cfg, &dec_handle) != ESP_OK){
            std::cout <<"解码器初始化:JPEG解码引擎创建失败"<<std::endl;
        }

        esp_video_init_jpeg_dec_config_t dec_config = {};
        dec_config.dec_handle = dec_handle;
        esp_video_init_config_t config = {};
        config.jpeg_dec = &dec_config;
        esp_err_t rst = esp_video_init(&config);
        if(rst == ESP_OK){
            std::cout <<"解码器初始化:JPEG解码器创建完成"<<std::endl;
        }

        jpeg_fd = open("/dev/video12",O_RDWR);
        if(jpeg_fd < 0){
            std::cout <<"解码器初始化:jpeg解码器打开失败"<<std::endl;
        }


        v4l2_format jpeg_in_fmt = {};
        jpeg_in_fmt.fmt.pix.width = 640;
        jpeg_in_fmt.fmt.pix.height = 480;
        jpeg_in_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
        jpeg_in_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    
        if(ioctl(jpeg_fd,VIDIOC_S_FMT,&jpeg_in_fmt) < 0){
            std::cout <<"解码器初始化:JPEG输入端格式设置错误"<<std::endl;
            close(jpeg_fd);
        }

        v4l2_format jpeg_out_fmt = {};
        jpeg_out_fmt.fmt.pix.width = 640;
        jpeg_out_fmt.fmt.pix.height = 480;
        jpeg_out_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        jpeg_out_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
        if(ioctl(jpeg_fd,VIDIOC_S_FMT,&jpeg_out_fmt) < 0){
            std::cout <<"解码器初始化:JPEG输出端格式设置错误"<<std::endl;
            close(jpeg_fd);
        }

        v4l2_requestbuffers jpeg_in_req = {};
        jpeg_in_req.count = 3;
        jpeg_in_req.memory = V4L2_MEMORY_USERPTR;
        jpeg_in_req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        if(ioctl(jpeg_fd,VIDIOC_REQBUFS,&jpeg_in_req) < 0){
            std::cout <<"解码器初始化:JPEG输入端 requestbuffer失败"<<std::endl;
            close(jpeg_fd);
        }

        v4l2_requestbuffers jpeg_out_req = {};
        jpeg_out_req.count = 3;
        jpeg_out_req.memory = V4L2_MEMORY_USERPTR;
        jpeg_out_req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(ioctl(jpeg_fd,VIDIOC_REQBUFS,&jpeg_out_req) < 0){
            std::cout <<"解码器初始化:JPEG输出端 requestbuffer失败"<<std::endl;
            close(jpeg_fd);
        }

        
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        if(ioctl(jpeg_fd,VIDIOC_STREAMON,&type) < 0){
            std::cout <<"解码器初始化:启动jpeg解码器输入端错误"<<std::endl;
            close(jpeg_fd);
        }
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(ioctl(jpeg_fd,VIDIOC_STREAMON,&type) < 0){
            std::cout <<"解码器初始化:启动jpeg解码器输出端错误"<<std::endl;
            close(jpeg_fd);
        }
    }

    static void decode_task(void* arg){
        Decoder d = *((Decoder*)arg);
        while(1){
            jpeg_unit jpeg = {};
            xQueueReceive(d.jpeg_full,&jpeg,portMAX_DELAY);
            v4l2_buffer in_buffer = {};
            in_buffer.memory = V4L2_MEMORY_USERPTR;
            in_buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            in_buffer.m.userptr = (unsigned long)jpeg.buffer;
            in_buffer.bytesused = jpeg.len;
            in_buffer.length = jpeg.len;
            in_buffer.index = 0;
            
            if(ioctl(d.jpeg_fd,VIDIOC_QBUF,&in_buffer) < 0){
                std::cout <<"解码任务:把数据送入jpeg时错误"<<std::endl;
                std::cout <<strerror(errno)<<std::endl;
            }
            rgb_unit rgb = {};
            xQueueReceive(d.rgb_free,&rgb,portMAX_DELAY);
            v4l2_buffer out_buffer = {};
            out_buffer.memory = V4L2_MEMORY_USERPTR;
            out_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            out_buffer.m.userptr = (unsigned long)rgb.buffer;
            out_buffer.index = 0;
            out_buffer.length = 640*480*2;
            // std::cout <<"jpeg address:"<<std::hex<<(void*)jpeg.buffer<<std::dec<<" jpeg.len"<<jpeg.len<<std::endl;
            // std::cout <<"rgb address:"<<std::hex<<(void*)rgb.buffer<<std::dec<<std::endl;
            if(ioctl(d.jpeg_fd,VIDIOC_QBUF,&out_buffer) < 0){
                std::cout <<"解码任务:把RGB BUFFER送入JPEG输出端时错误"<<std::endl;
                std::cout <<strerror(errno)<<std::endl;
            } 

            xSemaphoreTake(dma2d_mutex,portMAX_DELAY);
            if(ioctl(d.jpeg_fd,VIDIOC_DQBUF,&out_buffer) < 0){
                std::cout <<strerror(errno)<<std::endl;
            }
            xSemaphoreGive(dma2d_mutex);

            
            if(ioctl(d.jpeg_fd,VIDIOC_DQBUF,&in_buffer) < 0){
                std::cout <<"解码任务:把数据buffer从jpeg取出时错误"<<std::endl;
            }


            xQueueSend(d.jpeg_free,&jpeg,portMAX_DELAY);
            xQueueSend(d.rgb_full,&rgb,portMAX_DELAY);
        }
    }

    int jpeg_fd;
    jpeg_decoder_handle_t dec_handle = nullptr;
    QueueHandle_t jpeg_free;
    QueueHandle_t jpeg_full;
    QueueHandle_t rgb_free;
    QueueHandle_t rgb_full;
};