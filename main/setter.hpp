#pragma once
#include <iostream>
#include "esp_http_client.h"
#include <string>
#include "esp_err.h"
#include "mbedtls/md5.h"
#include "cJSON.h"
#include "esp_timer.h"

class FakeColor{
public:
    std::string name;
    std::string nike_name;
};

class Setter{
public:
    Setter(): ezoom(0),color_id(0){
        esp_http_client_config_t client_config = {};
        client_config.timeout_ms = 5000;
        client_config.url = "http://192.168.1.108/RPC2_Login";
        client_config.event_handler = on_event;
        this->client = esp_http_client_init(&client_config);
        response.clear();
    }

    esp_err_t login(std::string u,std::string p){
        last_login_ms = esp_timer_get_time() / 1000;  // 记录本次登录时刻(失败也记录，用于退避，避免密集重试)
        this->userName = u;
        this->password = p;
        this->response.clear();

        esp_http_client_set_url(client,"http://192.168.1.108/RPC2_Login");
        esp_http_client_set_header(client,"Content-Type","application/json");
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_user_data(client,&this->response);
        char body_cstr[384];
        snprintf(body_cstr,sizeof(body_cstr),R"({
                "method": "global.login",
                "params": {
                    "userName": "%s",
                    "password": "",
                    "clientType": "web3.0",
                    "loginType": "direct"
                },
                "id": 1
            })",this->userName.c_str());
        esp_http_client_set_post_field(client,body_cstr,strlen(body_cstr));
        int login1_rst = esp_http_client_perform(client);
        if(login1_rst != ESP_OK){
            return ESP_FAIL;
        }
        if(esp_http_client_get_status_code(client) != 200){
            return ESP_FAIL;
        }
        cJSON* root = cJSON_Parse(this->response.c_str());
        cJSON* params = cJSON_GetObjectItem(root,"params");
        const char* temp_session = cJSON_GetObjectItem(root,"session")->valuestring;
        const char* realm = cJSON_GetObjectItem(params,"realm")->valuestring;
        const char* random = cJSON_GetObjectItem(params,"random")->valuestring;
        std::cout <<"t_session:"<<temp_session<<std::endl;
        std::string hash1_param = this->userName + ':' + realm + ':' + this->password;
        unsigned char hash1[16];
        mbedtls_md5((const unsigned char*)hash1_param.c_str(),hash1_param.size(),hash1);
        std::string hash1_str = "";
        for(int i = 0;i<16;i++){
            std::string hex_char = "0123456789ABCDEF";
            char b = hash1[i];
            hash1_str.push_back(hex_char[b >> 4]);
            hash1_str.push_back(hex_char[b & 0x0F]);
        }
        std::cout <<"hash1: "<<hash1_str<<std::endl;


        std::string hash2_param = this->userName + ':' + random + ':' + hash1_str;
        unsigned char hash2[16];
        mbedtls_md5((const unsigned char*)hash2_param.c_str(),hash2_param.size(),hash2);
        std::string hash2_str = "";
        for(int i = 0;i<16;i++){
            std::string hex_char = "0123456789ABCDEF";
            char b = hash2[i];
            hash2_str.push_back(hex_char[b >> 4]);
            hash2_str.push_back(hex_char[b & 0x0F]);
        }
        std::cout <<"hash2: "<<hash2_str<<std::endl;

        snprintf(body_cstr,sizeof(body_cstr),R"({
                "method": "global.login",
                "params": {
                    "userName": "%s",
                    "password": "%s",
                    "clientType": "web3.0",
                    "loginType": "direct"
                },
                "id": 1,
                "session":"%s"
            })",this->userName.c_str(),hash2_str.c_str(),temp_session);
        this->response.clear();
        esp_http_client_set_post_field(client,body_cstr,strlen(body_cstr));
        esp_http_client_perform(client);
        cJSON* root2 = cJSON_Parse(this->response.c_str());
        this->session = cJSON_GetObjectItem(root2,"session")->valuestring;
        std::cout <<"session: "<<session<<std::endl;

        cJSON_Delete(root);
        cJSON_Delete(root2);
        return ESP_OK;
    }

    // 距离上次登录超过 max_age_ms 就重新登录，避免大华 session 超时后控制(ezoom/调色等)失效
    void reloginIfStale(uint64_t max_age_ms = 60000){
        int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000);
        if((now_ms - last_login_ms) > (int64_t)max_age_ms){
            login(this->userName, this->password);
        }
    }

    esp_err_t setEzoom(int e){
        esp_http_client_set_url(client,"http://192.168.1.108/RPC2");
        esp_http_client_set_method(client,HTTP_METHOD_POST);
        esp_http_client_set_header(client,"Content-Type","application/json");
        this->ezoom = e;
        char* body_str = (char*)heap_caps_malloc(2048,MALLOC_CAP_SPIRAM);
        std::string table_str = getSetting1_str();
        sniprintf(body_str,2048,R"({
                "method": "configManager.setTemporaryConfig",
                "params": {
                    "name": "ThermographyOptions",
                    "table": [[
                        %s, %s, %s
                    ]],
                    "options": []
                },
                "id": 1,
                "session": "%s"
            })",table_str.c_str(),table_str.c_str(),table_str.c_str(),this->session.c_str());
        esp_http_client_set_post_field(client,body_str,strlen(body_str));
        this->response.clear();
        esp_err_t ret = esp_http_client_perform(client);
        std::cout << "setConfig status=" << esp_http_client_get_status_code(client)
                  << " ezoom=" << this->ezoom << " color=" << this->color_id
                  << " resp=" << this->response << std::endl;
        heap_caps_free(body_str);
        return ret;
    }

    esp_err_t setColor(int color){
        esp_http_client_set_url(client,"http://192.168.1.108/RPC2");
        esp_http_client_set_method(client,HTTP_METHOD_POST);
        esp_http_client_set_header(client,"Content-Type","application/json");
        this->color_id = color;
        char* body_str = (char*)heap_caps_malloc(2048,MALLOC_CAP_SPIRAM);
        std::string table_str = getSetting1_str();
        sniprintf(body_str,2048,R"({
                "method": "configManager.setTemporaryConfig",
                "params": {
                    "name": "ThermographyOptions",
                    "table": [[
                        %s, %s, %s
                    ]],
                    "options": []
                },
                "id": 1,
                "session": "%s"
            })",table_str.c_str(),table_str.c_str(),table_str.c_str(),this->session.c_str());
        esp_http_client_set_post_field(client,body_str,strlen(body_str));
        this->response.clear();
        esp_err_t ret = esp_http_client_perform(client);
        std::cout << "setConfig status=" << esp_http_client_get_status_code(client)
                  << " ezoom=" << this->ezoom << " color=" << this->color_id
                  << " resp=" << this->response << std::endl;
        heap_caps_free(body_str);
        return ret;
    }

    esp_err_t switchPoint(bool p){
        esp_http_client_set_url(client,"http://192.168.1.108/RPC2");
        esp_http_client_set_method(client,HTTP_METHOD_POST);
        esp_http_client_set_header(client,"Content-Type","application/json");
        this->hotPoint = p;
        char* body_str = (char*)heap_caps_malloc(4096,MALLOC_CAP_SPIRAM);
        std::string table2_str = getSetting2_str();
        std::string table3_str = getSetting3_str();
        sniprintf(body_str,4096,R"({
                "method": "system.multicall",
                "params": [
                    {
                        "method": "configManager.setConfig",
                        "params": {
                            "name": "HeatImagingThermometry",
                            "table": %s,
                            "HighCTMakerColor": [255, 0, 0, 255],
                            "LowCTMakerColor": [0, 0, 255, 255],
                            "options": []
                        },
                        "id": 3001,
                        "session": "%s"
                    },
                    {
                        "method": "configManager.setConfig",
                        "params": {
                            "name": "HotColdSpotWarning",
                            "table": [
                                %s
                            ],
                            "options": []
                        },
                        "id": 3002,
                        "session": "%s"
                    }
                ],
                "id": 3003,
                "session": "%s"
            }))",table2_str.c_str(),this->session.c_str(),table3_str.c_str(),this->session.c_str(),this->session.c_str());
        esp_http_client_set_post_field(client,body_str,strlen(body_str));
        this->response.clear();
        esp_err_t ret = esp_http_client_perform(client);
        std::cout << "multicall status=" << esp_http_client_get_status_code(client)
                  << " resp=" << this->response << std::endl;
        heap_caps_free(body_str);
        return ret;
    }

    esp_err_t switchOutline(bool o){
        esp_http_client_set_url(client,"http://192.168.1.108/RPC2");
        esp_http_client_set_method(client,HTTP_METHOD_POST);
        esp_http_client_set_header(client,"Content-Type","application/json");
        this->outLine = o;
        char* body_str = (char*)heap_caps_malloc(4096,MALLOC_CAP_SPIRAM);
        std::string table2_str = getSetting2_str();
        std::string table3_str = getSetting3_str();
        sniprintf(body_str,4096,R"({
                "method": "system.multicall",
                "params": [
                    {
                        "method": "configManager.setConfig",
                        "params": {
                            "name": "HeatImagingThermometry",
                            "table": %s,
                            "HighCTMakerColor": [255, 0, 0, 255],
                            "LowCTMakerColor": [0, 0, 255, 255],
                            "options": []
                        },
                        "id": 3001,
                        "session": "%s"
                    },
                    {
                        "method": "configManager.setConfig",
                        "params": {
                            "name": "HotColdSpotWarning",
                            "table": [
                                %s
                            ],
                            "options": []
                        },
                        "id": 3002,
                        "session": "%s"
                    }
                ],
                "id": 3003,
                "session": "%s"
            }))",table2_str.c_str(),this->session.c_str(),table3_str.c_str(),this->session.c_str(),this->session.c_str());
        esp_http_client_set_post_field(client,body_str,strlen(body_str));
        this->response.clear();
        esp_err_t ret = esp_http_client_perform(client);
        std::cout << "multicall status=" << esp_http_client_get_status_code(client)
                  << " resp=" << this->response << std::endl;
        heap_caps_free(body_str);
        return ret;
    }

    void setBackLight(int rate){
        if(rate > 100){
            backLigthRate = 100;
        }
        else if(rate < 0){
            backLigthRate = 0;
        }
        else{
            backLigthRate = rate;
        }
        ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,(backLigthRate/100.0f)*1024);
        ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
    }

    std::string getSetting1_str(){
        char* buf = (char*)heap_caps_malloc(512,MALLOC_CAP_SPIRAM);
        snprintf(buf, 512,
        R"({"EZoom":%d,"ThermographyGamma":3,"SmartOptimizer":15,"Agc":1,"AgcMaxGain":8,"AgcPlateau":0,)"
        R"("Mode":"LowTemperature","Colorization":"%s","BaseBrightness":64,"StretchIntensity":64,)"
        R"("OptimizedRegion":{"Type":"Full Screen","Enable":true,"Regions":[[0,0,0,0]]},)"
        R"("HighTempGainMode":{"Agc":1,"AgcMaxGain":8,"AgcPlateau":1},)"
        R"("Auto":{"LowToHigh":135,"LHROI":95,"HighToLow":120,"HLROI":15},)"
        R"("ContrastRect":[0,0,0,0],"Brightness":50,"Contrast":50})",
        ezoom, color_strs[color_id].name.c_str());
        std::string ret(buf);
        heap_caps_free(buf);
        return ret;
    }

    std::string getSetting2_str(){
        char* buf = (char*)heap_caps_malloc(512,MALLOC_CAP_SPIRAM);
        snprintf(buf, 512,
            R"({"TemperEnable":true,"RelativeHumidity":0,"AtmosphericTemperature":25,)"
            R"("ObjectEmissivity":0.9700000286102295,"ObjectDistance":3,"DistanceDecimalPart":0,)"
            R"("ReflectedTemperature":25,"Altitude":0,"Visibility":50,"RainFallCapacity":0,)"
            R"("HotSpotFollow":%s,"HotSpotColorMode":"Auto","TemperatureUnit":"Centigrade",)"
            R"("Isotherm":{"Enable":%s,"ColorBarDisplay":false,"MinLimitTemp":100,)"
            R"("MediumTemp":120,"MaxLimitTemp":140,"SaturationTemp":150,"Rect":[7600,7800,6000,2000]}})"
        ,this->hotPoint ? "true" : "false",this->outLine ? "true" : "false");
        return std::string(buf);
    }

    std::string getSetting3_str(){
        return std::string(
            R"({"HotAlarmEnable":false,"ColdAlarmEnable":false,"HotThreshold":0,"ColdThreshold":0,)"
            R"("HotAlarmCondition":"AboveMax","ColdAlarmCondition":"BelowMin",)"
            R"("HotSpotLinkEnable":true,"ColdSpotLinkEnable":true,)"
            R"("EventHandler":{"AlarmOut":1,"ExAlarmOut":1,"Snapshot":1,"Record":1,"Tour":1,"Matrix":1,)"
            R"("TipEnable":false,"Dejitter":0,"ExAlarmOutChannels":[0],"ExAlarmOutEnable":false,)"
            R"("FlashEnable":false,"FlashLatch":10,"LogEnable":false,"MailEnable":false,)"
            R"("PtzLinkEnable":false,"AlarmOutEnable":false,"MatrixChannels":[0],"MatrixEnable":false,)"
            R"("MessageEnable":false,"PtzLink":[["",0]],"RecordChannels":[0],"RecordEnable":true,)"
            R"("RecordLatch":10,"PtzLinkDelay":0,"AlarmOutLatch":0,"SnapshotChannels":[0],"SnapshotEnable":true,)"
            R"("TimeSection":[)"
            R"(["1 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59"],)"
            R"(["1 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59"],)"
            R"(["1 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59"],)"
            R"(["1 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59"],)"
            R"(["1 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59"],)"
            R"(["1 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59"],)"
            R"(["1 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59","0 00:00:00-23:59:59"]],)"
            R"("TourChannels":[0],"TourEnable":false,"VoiceEnable":false,"AlarmOutChannels":[0]}})"
        );
    }

    static int on_event(esp_http_client_event_t* event){
        auto* response = static_cast<std::string*>(event->user_data);
        if(event->event_id == HTTP_EVENT_ON_DATA){
            response->append(std::string((char*)event->data,event->data_len));
        }
        return ESP_OK;
    }
    esp_http_client_handle_t client;
    int ezoom;
    int64_t last_login_ms = 0;
    int color_id;
    bool hotPoint;
    bool outLine;
    int backLigthRate;
    std::string userName;
    std::string password;
    std::string response;
    std::string session;
    std::vector<FakeColor> color_strs = {
        {"Ironbow2","铁红"},
        {"White Hot","白热"},
        {"Black Hot","黑热"},
        {"Rainbow","彩虹"}
    };
};