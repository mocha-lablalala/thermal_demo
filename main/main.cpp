#include <iostream>
#include "esp_http_client.h"
#include "semaphore.h"
#include "esp_video_init.h"
#include "parser.hpp"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_mac.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_ops.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "jpeg_decoder.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "setter.hpp"
#include "rotate.hpp"
#include "esp_lvgl_port.h"
#include "esp_attr.h"
#include "esp_adc/adc_oneshot.h"

extern const lv_font_t thermal_font;

#define CAM_USERNAME "admin"
#define CAM_PASSWORD "root123root"

//W5500引脚
#define PIN_ETH_INT 32
#define PIN_ETH_CS 27
#define PIN_ETH_SCK 26
#define PIN_ETH_MO 23
#define PIN_ETH_RST 30
#define PIN_ETH_MI 25

//屏幕引脚
#define PIN_SCRE_SCLK 28
#define PIN_SCRE_MOSI 29
#define PIN_SCRE_RST 5
#define PIN_SCRE_DC 52
#define PIN_SCRE_CS 51
#define PIN_SCRE_BLK 31

#define LCD_W 240
#define LCD_H 320

#define RGB_SIZE 640*480*2

#define JPEG_BUFFER_CAP 64*1024


//网络相关
esp_netif_t* netif;
SemaphoreHandle_t eth_ready;
esp_http_client_handle_t client;
SemaphoreHandle_t cam_ready;

//设置API
Setter* setter;

//MJPEG解析器相关
MjpegParser* parser;
uint8_t* jpeg_buffer;
QueueHandle_t jpeg_free;
QueueHandle_t jpeg_full;
QueueHandle_t rgb_free;
QueueHandle_t rgb_full;
SemaphoreHandle_t dma2d_mutex;

//屏幕显示相关
esp_lcd_panel_io_handle_t io_handle;
esp_lcd_panel_handle_t panel_handle;
ppa_client_handle_t ppa_handle;
uint8_t* display_buffer;

//lvgl
lv_obj_t* video;
lv_obj_t* zoom_str;
lv_obj_t* vol_str;
lv_obj_t* node_lists;
bool is_menu;
lv_indev_t* indev;
std::vector<lv_obj_t*> functions;
std::vector<lv_obj_t*> fun_names;
lv_group_t* function_group;

//编码器
RotateEncoder* rotate;

//ADC
float battery_vol;



//函数声明
void eth_init();
static void mjpegReceiveTask(void* pvParameters);
void screen_init();
void ppa_init();
void ppa_display_task(void* arg);
void rotate_task(void* arg);
void lvgl_init();
void draw_load_view();
void draw_video_ui();
void battery_task(void* arg);


extern "C" void app_main(void)
{
    dma2d_mutex = xSemaphoreCreateMutex();
    eth_init();    
    xSemaphoreTake(eth_ready,portMAX_DELAY);
    std::cout <<"以太网创建完成"<<std::endl;
    setter = new Setter();    
    rotate = new RotateEncoder();
    rotate->encoder_init();
    display_buffer = (uint8_t*)heap_caps_aligned_alloc(
        128,
        320*240*2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA
    );
    memset(display_buffer,0,320*240*2);

    //显示，渲染在屏幕上
    screen_init();
    is_menu = false;
    lvgl_init();
    draw_load_view();
    // 启动电量监测：
    xTaskCreate(battery_task,"BATTERY",4096,nullptr,5,nullptr);

    cam_ready = xSemaphoreCreateBinary();
    while(setter->login(CAM_USERNAME,CAM_PASSWORD) != ESP_OK){
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    xSemaphoreGive(cam_ready);
    setter->setColor(0);
    setter->setEzoom(0);
    setter->switchPoint(false);
    setter->switchOutline(false);

    draw_video_ui();
    xTaskCreate(rotate_task,"ROTATE",4096,rotate,5,nullptr);
    //解析器
    jpeg_free = xQueueCreate(3,sizeof(jpeg_unit));
    jpeg_full = xQueueCreate(3,sizeof(jpeg_unit));
    for(int i = 0;i<3;i++){
        jpeg_unit unit = {};
        jpeg_buffer = (uint8_t*)heap_caps_aligned_alloc(
            4096,
            JPEG_BUFFER_CAP,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA
        );
        unit.buffer = jpeg_buffer;
        unit.len = 0;
        xQueueSend(jpeg_free,&unit,portMAX_DELAY);
    }
    parser = new MjpegParser(JPEG_BUFFER_CAP,jpeg_free,jpeg_full);
    parser->parser_init();
    //开始接收图像
    xTaskCreate(mjpegReceiveTask,"MJPEG_TASK",4096,nullptr,5,nullptr);

    rgb_free = xQueueCreate(3,sizeof(rgb_unit));
    rgb_full = xQueueCreate(3,sizeof(rgb_unit));
    for(int i = 0;i<3;i++){
        rgb_unit rgb = {};
        uint8_t* buffer = (uint8_t*)heap_caps_aligned_alloc(4096,
            RGB_SIZE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA
        );
        rgb.buffer = buffer;
        xQueueSend(rgb_free,&rgb,portMAX_DELAY);
    }

    //开始启动jpeg解码器
    Decoder decoder = {};
    decoder.jpeg_free = jpeg_free;
    decoder.jpeg_full = jpeg_full;
    decoder.rgb_free = rgb_free;
    decoder.rgb_full = rgb_full;
    decoder.decoder_init();
    xTaskCreate(decoder.decode_task,"JPEG_DECODER_TASK",4096,&decoder,5,nullptr);


    ppa_init();
    xTaskCreate(ppa_display_task,"DISPLAY_TASK",4096,nullptr,5,nullptr);
    while(1){
        vTaskDelay(100);
    }
}

//连上网之后的回调函数
static void eth_connected(void* arg,esp_event_base_t base,int32_t event_id,void* event){
    if(event_id == ETHERNET_EVENT_CONNECTED){
        xSemaphoreGive(eth_ready);
    }
}

void eth_init(){
    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    netif = esp_netif_new(&netif_config);

    //停止DHCP并设置静态IP(直连摄像头,摄像头IP=192.168.1.108)
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t ip_info = {};
    ip_info.ip.addr = esp_ip4addr_aton("192.168.1.2");     
    ip_info.gw.addr = esp_ip4addr_aton("192.168.1.1");     // 直连时网关用不到,随便填
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_set_ip_info(netif,&ip_info);

    //创建spi BUS
    spi_bus_config_t bus_config = {};
    bus_config.miso_io_num = PIN_ETH_MI;
    bus_config.mosi_io_num = PIN_ETH_MO;
    bus_config.quadhd_io_num = -1;
    bus_config.quadwp_io_num = -1;
    bus_config.sclk_io_num = PIN_ETH_SCK;
    spi_bus_initialize(SPI2_HOST,&bus_config,SPI_DMA_CH_AUTO);

    //创建spi设备
    spi_device_interface_config_t if_config = {};
    if_config.clock_speed_hz =10*1000*1000;
    if_config.mode = 0;
    if_config.queue_size = 20;
    if_config.spics_io_num = PIN_ETH_CS;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST,&if_config);
    w5500_config.int_gpio_num = PIN_ETH_INT;
    
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = PIN_ETH_RST;
    gpio_install_isr_service(0);
    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config,&mac_config);
    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac,phy);
    esp_eth_handle_t eth_handle = nullptr;
    esp_eth_driver_install(&eth_config,&eth_handle);

    //给w5500设置一个mac地址
    uint8_t mac_addr[8];
    esp_read_mac(mac_addr,ESP_MAC_ETH);
    esp_eth_ioctl(eth_handle,ETH_CMD_S_MAC_ADDR,mac_addr);

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    esp_netif_attach(netif,glue);

    eth_ready = xSemaphoreCreateBinary();
    esp_event_handler_register(ETH_EVENT,ETHERNET_EVENT_CONNECTED,eth_connected,nullptr);

    esp_eth_start(eth_handle);
}

void frame_callback(const uint8_t* buf,size_t len){
    std::cout <<"new frame size:"<<len<<std::endl;
}

static esp_err_t http_event_handle(esp_http_client_event_t* event){
    if(event->event_id == HTTP_EVENT_ON_DATA){
        parser->feed((const uint8_t*)event->data,event->data_len,frame_callback);
    }
    return ESP_OK;
}


void client_create(){
    esp_http_client_config_t client_config = {};
    client_config.url = "http://192.168.1.108/cgi-bin/mjpg/video.cgi?channel=1&subtype=1";
    client_config.timeout_ms = 5000;
    client_config.username = CAM_USERNAME;
    client_config.password = CAM_PASSWORD;
    client_config.auth_type = HTTP_AUTH_TYPE_DIGEST;
    client_config.event_handler = http_event_handle;
    client_config.user_data = nullptr;

    client = esp_http_client_init(&client_config);
    parser->reset();
}

static void mjpegReceiveTask(void* pvParameters){
    while(1){
        client_create();
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
        std::cout <<"断线重连中.."<<std::endl;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }   
}


void screen_init(){
    spi_bus_config_t bus_config = {};
    bus_config.miso_io_num = -1;
    bus_config.mosi_io_num = PIN_SCRE_MOSI;
    bus_config.quadhd_io_num = -1;
    bus_config.quadwp_io_num = -1;
    bus_config.sclk_io_num = PIN_SCRE_SCLK;
    bus_config.max_transfer_sz = 80*LCD_W*2;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST,&bus_config,SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = PIN_SCRE_CS;
    io_config.dc_gpio_num = PIN_SCRE_DC;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.pclk_hz = 40*1000*1000;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST,&io_config,&io_handle));

    esp_lcd_panel_dev_config_t dev_config = {};
    dev_config.bits_per_pixel = 16;
    dev_config.reset_gpio_num = PIN_SCRE_RST;
    dev_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    dev_config.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle,&dev_config,&panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    esp_lcd_panel_swap_xy(panel_handle,true);
    esp_lcd_panel_mirror(panel_handle,true,false);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle,true));

    //背光
    ledc_timer_config_t timer_config = {};
    timer_config.freq_hz = 25*1000;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    timer_config.duty_resolution = LEDC_TIMER_10_BIT;
    timer_config.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_config.timer_num = LEDC_TIMER_0;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    ledc_channel_config_t channel_config = {};
    channel_config.channel = LEDC_CHANNEL_0;
    channel_config.gpio_num = PIN_SCRE_BLK;
    channel_config.timer_sel = LEDC_TIMER_0;
    channel_config.speed_mode = LEDC_LOW_SPEED_MODE;
    channel_config.duty = 1023;
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));

    setter->setBackLight(100);
}

void ppa_init(){
    ppa_client_config_t config = {};
    config.oper_type = PPA_OPERATION_SRM;
    config.max_pending_trans_num = 3;
    if(ppa_register_client(&config,&ppa_handle) != ESP_OK){
        std::cout <<"PPA 注册失败"<<std::endl;
    }
}

esp_err_t srm_jpeg(const void*in_buffer,void* out_buffer){
    ppa_srm_oper_config_t srm_config = {};
    srm_config.in.pic_w = 640;
    srm_config.in.pic_h = 480;
    srm_config.in.block_w = 640;
    srm_config.in.block_h = 480;
    srm_config.in.block_offset_x = 0;
    srm_config.in.block_offset_y = 0;
    srm_config.in.buffer = in_buffer;
    srm_config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    srm_config.out.pic_w = 320;
    srm_config.out.pic_h = 240;
    srm_config.out.buffer = out_buffer;
    srm_config.out.buffer_size = 320*240*2;
    srm_config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    srm_config.byte_swap = true;
    srm_config.scale_x = 0.5;
    srm_config.scale_y = 0.5;
    srm_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;

    if(ppa_do_scale_rotate_mirror(ppa_handle,&srm_config) != ESP_OK){
        std::cout <<"ppa转换失败"<<std::endl;
        return ESP_FAIL;
    }
    return ESP_OK;
}


void ppa_display_task(void* arg){
    while(1){
        rgb_unit rgb = {};
        xQueueReceive(rgb_full,&rgb,portMAX_DELAY);
        if(lvgl_port_lock(0)){
            xSemaphoreTake(dma2d_mutex,portMAX_DELAY);
            esp_err_t ret = srm_jpeg(rgb.buffer,display_buffer);
            xSemaphoreGive(dma2d_mutex);
            if(!ret){
                lv_obj_invalidate(video);
            }
        }
        xQueueSend(rgb_free,&rgb,portMAX_DELAY);
        lvgl_port_unlock();
        
    }
}

void rotate_task(void* arg){
    RotateEncoder* rotate = (RotateEncoder*)arg;
    int count = 0;
    int last_ezoom = -1;
    int last_press = 0;
    while(1){
        // 定期重新登录，防止大华 session 超时后 ezoom 等控制失效
        setter->reloginIfStale(60*1000);

        bool is_press = false;
        if(gpio_get_level(GPIO_NUM_20) == 0){
            int now_press = esp_timer_get_time();
            if(now_press - last_press > 5*1000*100){
                is_press = true;
                last_press = now_press;
            }
        }
        if(is_press && !is_menu){
            while(gpio_get_level((gpio_num_t)PIN_EC_SW) == 0){
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if(lvgl_port_lock(0)){
                lv_obj_set_hidden(node_lists,false);
                lvgl_port_unlock();
                is_menu = true;
                lv_indev_enable(indev, true);
            }
        }   
        if(!is_menu){
            count += rotate->get_delta();
            if(count > 32){
                count = 32;
            }
            else if(count < 0){
                count = 0;
            }

            int ezoom = count/2;
            if(ezoom != last_ezoom){
                setter->setEzoom(ezoom);
                if(lvgl_port_lock(0)){
                    lv_label_set_text_fmt(zoom_str, "%d",ezoom);
                    lvgl_port_unlock();
                }
                last_ezoom = ezoom;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


static void lvgl_indev_cb(lv_indev_t* indev,lv_indev_data_t* data){
    RotateEncoder* encoder = (RotateEncoder*)lv_indev_get_driver_data(indev);
    static int count = 0;
    count += encoder->get_delta();
    int step = count/2;
    count -= step*2;
    data->enc_diff = step;

    if(gpio_get_level((gpio_num_t)PIN_EC_SW) == 0){
        data->key = LV_KEY_ENTER;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else{
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void fun_clicked_cb(lv_event_t* event){
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(event);
    if(target == functions[0]){
        lv_group_set_editing(function_group,true);
    }
    else if(target == functions[1]){
        if(lvgl_port_lock(0)){
            setter->switchPoint(!setter->hotPoint);
            std::string point_str = "显示冷热极点: " + std::string(setter->hotPoint ? "开" : "关");
            lv_label_set_text(fun_names[1],point_str.c_str());
            lvgl_port_unlock();
        }
    }
    else if(target == functions[2]){
        if(lvgl_port_lock(0)){
            setter->switchOutline(!setter->outLine);
            std::string outline_str = "火眼金睛模式: " + std::string(setter->outLine ? "开" : "关");
            lv_label_set_text(fun_names[2],outline_str.c_str());
            lvgl_port_unlock();
        }
    }
    else if(target == functions[3]){
    }
    else if(target == functions[4]){
        if(lvgl_port_lock(0)){
            lv_obj_set_hidden(node_lists,true);
            lvgl_port_unlock();
            is_menu = false;
            lv_indev_enable(indev, false);
            lv_group_focus_obj(functions[0]);
        }
    }
}

static void function_edit_cb(lv_event_t* event){
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(event);
    uint32_t key = lv_event_get_key(event);

    static int color_id = 0;
    if(target == functions[0]){//改颜色
        if(key == LV_KEY_LEFT){
            if(lvgl_port_lock(0)){
                color_id --;
                color_id = color_id % setter->color_strs.size();
                std::string color_str = "伪彩色: " + setter->color_strs[color_id].nike_name;
                lv_label_set_text(fun_names[0],color_str.c_str());
                lvgl_port_unlock();
            }
        }
        else if(key == LV_KEY_RIGHT){
            if(lvgl_port_lock(0)){
                color_id ++;
                color_id = color_id % setter->color_strs.size();
                std::string color_str = "伪彩色: " + setter->color_strs[color_id].nike_name;
                lv_label_set_text(fun_names[0],color_str.c_str());
                lvgl_port_unlock();
            }
        }
        if(key == LV_KEY_ENTER){
            setter->setColor(color_id);
            lv_group_set_editing(function_group,false);
        }
        
    }    
    if(target == functions[3]){//改背光
        if(key == LV_KEY_LEFT){
            if(lvgl_port_lock(0)){
                setter->setBackLight(setter->backLigthRate-5);
                std::string backlight_str = "屏幕亮度: " + std::to_string(setter->backLigthRate) + '%';
                lv_label_set_text(fun_names[3],backlight_str.c_str());
                lvgl_port_unlock();
            }
        }
        else if(key == LV_KEY_RIGHT){
            if(lvgl_port_lock(0)){
                setter->setBackLight(setter->backLigthRate+5);
                std::cout <<setter->backLigthRate<<std::endl;
                std::string backlight_str = "屏幕亮度: " + std::to_string(setter->backLigthRate) + '%';
                lv_label_set_text(fun_names[3],backlight_str.c_str());
                lvgl_port_unlock();
            }
        }
        if(key == LV_KEY_ENTER){
            lv_group_set_editing(function_group,false);
        }
    }
}

void lvgl_init(){
    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_config);

    lvgl_port_display_cfg_t display_config = {};
    display_config.buffer_size = 320*40;
    display_config.color_format = LV_COLOR_FORMAT_RGB565;
    display_config.double_buffer = true;
    display_config.hres = 320;
    display_config.vres = 240;
    display_config.io_handle = io_handle;
    display_config.monochrome = false;
    display_config.panel_handle = panel_handle;
    display_config.rotation = {
        .swap_xy = true,
        .mirror_x = true,
        .mirror_y = false
    };
    display_config.flags.buff_dma = true;
    lvgl_port_add_disp(&display_config);
}

void draw_video_ui(){
    lvgl_port_lock(0);

    video = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(video,display_buffer,320,240,LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(video,0,0);
    zoom_str = lv_label_create(lv_screen_active());
    lv_label_set_text(zoom_str,"0");
    lv_obj_set_style_text_color(zoom_str,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_pos(zoom_str,5,0);
    lv_obj_set_style_text_font(zoom_str,&lv_font_montserrat_26,0);    
    
    vol_str = lv_label_create(lv_screen_active());
    char vol[16];
    snprintf(vol,sizeof(vol),"%.2f V",battery_vol);
    lv_label_set_text(vol_str,vol);
    lv_obj_set_style_text_color(vol_str,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_pos(vol_str,270,220);
    lv_obj_set_style_text_font(vol_str,&lv_font_montserrat_14,0);


    node_lists = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(node_lists);
    static lv_style_t list_css = {};
    lv_style_set_width(&list_css,150);
    lv_style_set_height(&list_css,240);
    lv_style_set_bg_color(&list_css,lv_color_hex(0x0000FF));
    lv_style_set_bg_opa(&list_css,LV_OPA_COVER);
    
    lv_obj_set_pos(node_lists,320-150,0);
    lv_obj_set_layout(node_lists,LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(node_lists,LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(node_lists,&list_css,0);


    //lv group,用来形成一个组织，内部切换焦点
    function_group = lv_group_create();

    indev = lv_indev_create();
    lv_indev_set_type(indev,LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev,lvgl_indev_cb);
    lv_indev_set_driver_data(indev,rotate);
    lv_indev_set_group(indev,function_group);
    lv_indev_enable(indev, false);

    static lv_style_t function_css = {};
    lv_style_set_width(&function_css,150);
    lv_style_set_height(&function_css,30);
    lv_style_set_bg_color(&function_css,lv_color_hex(0x0000FF));
    lv_style_set_text_color(&function_css,lv_color_hex(0xFFFFFF));
    lv_style_set_bg_opa(&function_css,LV_OPA_COVER);

    static lv_style_t function_css_f = {};
    lv_style_set_text_color(&function_css_f,lv_color_hex(0x000000));
    lv_style_set_bg_color(&function_css_f,lv_color_hex(0xFFFFFF));
    /*
        颜色
        开启热点
        开启轮廓
        背光强度
        退出
    */
    for(int i = 0;i<5;i++){
        lv_obj_t* function_1 = lv_obj_create(node_lists);
        lv_obj_remove_style_all(function_1);
        lv_obj_t* fun_name = lv_label_create(function_1);
        lv_obj_remove_style_all(fun_name);

        lv_obj_set_style_text_font(fun_name, &thermal_font, 0);
        functions.push_back(function_1);
        fun_names.push_back(fun_name);
        lv_group_add_obj(function_group,function_1);
        lv_obj_set_scrollable(function_1,false);    //防止自动聚焦到这个元件之后焦点划不动
        lv_obj_add_style(function_1,&function_css,0);
        lv_obj_align(fun_name,LV_ALIGN_LEFT_MID,0,0);
        lv_obj_add_style(function_1,&function_css_f,LV_STATE_FOCUSED);
        lv_obj_set_clickable(function_1,true);
        lv_obj_add_event_cb(function_1,fun_clicked_cb,LV_EVENT_CLICKED,nullptr);
    }

    std::string color_str = "伪彩色: " + setter->color_strs[setter->color_id].nike_name;
    std::string point_str = "显示冷热极点: " + std::string(setter->hotPoint ? "开" : "关");
    std::string outline_str = "火眼金睛模式: " + std::string(setter->outLine ? "开" : "关");
    std::string backlight_str = "屏幕亮度: " + std::to_string(setter->backLigthRate) + '%';
    lv_label_set_text(fun_names[0],color_str.c_str());
    lv_label_set_text(fun_names[1],point_str.c_str());
    lv_label_set_text(fun_names[2],outline_str.c_str());
    lv_label_set_text(fun_names[3],backlight_str.c_str());
    lv_label_set_text(fun_names[4],"退出");
    lv_obj_set_hidden(node_lists,true);

    lv_obj_add_event_cb(functions[0], function_edit_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(functions[3], function_edit_cb, LV_EVENT_KEY, NULL);

    lv_obj_set_scrollable(functions[0], true);
    lv_obj_set_scrollable(functions[3], true);

    lvgl_port_unlock();
}

void draw_load_view(){
    lvgl_port_lock(0);
    lv_obj_t* back = lv_obj_create(lv_screen_active());
    static lv_style_t back_css = {};
    lv_style_set_width(&back_css,320);
    lv_style_set_height(&back_css,240);
    lv_style_set_bg_color(&back_css,lv_color_hex(0xFFFFFF));
    lv_style_set_bg_opa(&back_css,LV_OPA_COVER);
    lv_obj_add_style(back,&back_css,0);

    lv_obj_t* load_str = lv_label_create(back);
    lv_obj_set_style_text_font(load_str,&thermal_font,0);
    lv_label_set_text(load_str,"加载中...");
    lv_obj_set_style_text_color(load_str,lv_color_hex(0x000000),0);
    lv_obj_align(load_str,LV_ALIGN_CENTER,0,0);
    lvgl_port_unlock();
}

void battery_task(void* arg){
    adc_oneshot_unit_handle_t unit_handle = nullptr;
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = ADC_UNIT_2;
    init_config.ulp_mode = ADC_ULP_MODE_DISABLE;
    adc_oneshot_new_unit(&init_config,&unit_handle);

    adc_oneshot_chan_cfg_t channel_config = {};
    channel_config.atten = ADC_ATTEN_DB_12;
    channel_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(unit_handle,ADC_CHANNEL_0,&channel_config);

    adc_cali_handle_t cali_handle = nullptr;
    adc_cali_curve_fitting_config_t fit_config = {};
    fit_config.atten = ADC_ATTEN_DB_12;
    fit_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    fit_config.unit_id = ADC_UNIT_2;
    adc_cali_create_scheme_curve_fitting(&fit_config,&cali_handle);

    while(1){    
        int vol_raw = 0;
        int vol = 0;
        int vol_sum = 0;
        for(int i = 0;i<100;i++){
            adc_oneshot_read(unit_handle,ADC_CHANNEL_0,&vol_raw);
            adc_cali_raw_to_voltage(cali_handle,vol_raw,&vol);
            vol_sum += vol;
        }
        battery_vol = (vol_sum/100)/500.0f;
        if(lvgl_port_lock(0)){
            char vol[16];
            snprintf(vol,sizeof(vol),"%.2f V",battery_vol);
            lv_label_set_text(vol_str,vol);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}