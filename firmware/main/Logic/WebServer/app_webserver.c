/**
 * @file app_webserver.c
 * @brief 现代化工业网关 Web 控制台 (支持本地 DIDO、万能 485 设备物模型控制与采集、模板生成、万能Modbus调试、网络配置与OTA)。
 */

#include "app_webserver.h"
#include "app_DO.h"
#include "app_DI.h"
#include "app_wifi.h"
#include "app.network.h"
#include "app_rs485_engine.h"
#include "app_can_engine.h"

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "esp_check.h"

static const char *TAG = "APP_WEBSERVER";
static httpd_handle_t s_server = NULL;

/* ---------------- 前端页面 HTML (选项卡设计：工作台 vs 万能485控制 vs 网络配置) ---------------- */

/* ---------------- 管理员认证凭据与 Token 配置 ---------------- */
#define AUTH_COOKIE_NAME "auth_token"
#define SESSION_RANDOM_BYTES 24
#define SESSION_MAX_AGE_SECONDS 86400
#define SESSION_MAX_AGE_US (INT64_C(86400) * 1000000)
#define AUTH_COOKIE_ATTRIBUTES "; Path=/; HttpOnly; SameSite=Strict"

static char s_session_token[SESSION_RANDOM_BYTES * 2 + 1];
static int64_t s_session_expires_us;

static bool credentials_configured(void)
{
    const char *allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-";
    const char *user = CONFIG_IOT5_WEB_USERNAME;
    const char *password = CONFIG_IOT5_WEB_PASSWORD;
    size_t user_len = strlen(user);
    size_t password_len = strlen(password);
    return user_len >= 1 && user_len <= 31 &&
           password_len >= 12 && password_len <= 63 &&
           strspn(user, allowed) == user_len && strspn(password, allowed) == password_len;
}

static void clear_session(void)
{
    s_session_token[0] = '\0';
    s_session_expires_us = 0;
}

static void create_session(void)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random_bytes[SESSION_RANDOM_BYTES];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t i = 0; i < sizeof(random_bytes); ++i) {
        s_session_token[i * 2] = hex[random_bytes[i] >> 4];
        s_session_token[i * 2 + 1] = hex[random_bytes[i] & 0x0f];
    }
    s_session_token[sizeof(s_session_token) - 1] = '\0';
    s_session_expires_us = esp_timer_get_time() + SESSION_MAX_AGE_US;
}

static bool is_request_authenticated(httpd_req_t *req)
{
    if (s_session_expires_us <= esp_timer_get_time()) {
        return false;
    }

    char auth_buf[128];
    const char bearer[] = "Bearer ";
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, sizeof(auth_buf)) == ESP_OK &&
        strncmp(auth_buf, bearer, sizeof(bearer) - 1) == 0 &&
        strcmp(auth_buf + sizeof(bearer) - 1, s_session_token) == 0) {
        return true;
    }

    char cookie_buf[256];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buf, sizeof(cookie_buf)) != ESP_OK) {
        return false;
    }
    const char cookie_name[] = AUTH_COOKIE_NAME "=";
    char *next = NULL;
    for (char *cookie = strtok_r(cookie_buf, ";", &next); cookie != NULL;
         cookie = strtok_r(NULL, ";", &next)) {
        cookie += strspn(cookie, " \t");
        if (strncmp(cookie, cookie_name, sizeof(cookie_name) - 1) == 0 &&
            strcmp(cookie + sizeof(cookie_name) - 1, s_session_token) == 0) {
            return true;
        }
    }
    return false;
}

#define CHECK_AUTH(req) do { \
    if (!is_request_authenticated(req)) { \
        httpd_resp_set_status(req, "401 Unauthorized"); \
        httpd_resp_set_type(req, "application/json"); \
        httpd_resp_sendstr(req, "{\"error\":\"Unauthorized\",\"msg\":\"请先登录管理员账号\"}"); \
        return ESP_FAIL; \
    } \
} while(0)

static const char *INDEX_HTML = 
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>ESP32-S3 智能工控网关</title>"
"<style>"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:12px;background:#f0f2f5;color:#333;}"
".container{max-width:800px;margin:0 auto;background:#fff;border-radius:10px;padding:18px;box-shadow:0 2px 10px rgba(0,0,0,0.08);}"
".header{text-align:center;border-bottom:2px solid #1890ff;padding-bottom:10px;margin-bottom:12px;}"
".header h2{margin:0;color:#1890ff;font-size:22px;}"
".tabs{display:flex;border-bottom:1px solid #ddd;margin-bottom:15px;background:#fafafa;border-radius:6px;}"
".tab{flex:1;text-align:center;padding:10px 0;cursor:pointer;font-weight:bold;color:#666;transition:0.2s;}"
".tab.active{color:#1890ff;border-bottom:3px solid #1890ff;background:#fff;border-radius:6px 6px 0 0;}"
".content{display:none;}.content.active{display:block;}"
"h3{color:#333;border-left:4px solid #1890ff;padding-left:8px;margin:16px 0 10px;font-size:15px;}"
".row{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid #f0f0f0;font-size:14px;}"
".btn{border:none;padding:6px 14px;border-radius:4px;cursor:pointer;font-weight:bold;transition:0.2s;font-size:13px;}"
".btn-on{background:#52c41a;color:#fff;}.btn-on:hover{background:#389e0d;}"
".btn-off{background:#ff4d4f;color:#fff;margin-left:6px;}.btn-off:hover{background:#cf1322;}"
".btn-primary{background:#1890ff;color:#fff;}.btn-primary:hover{background:#096dd9;}"
".btn-warn{background:#faad14;color:#fff;}.btn-warn:hover{background:#d48806;}"
".btn-outline{background:#fff;border:1px solid #d9d9d9;color:#555;}.btn-outline:hover{border-color:#1890ff;color:#1890ff;}"
".badge{display:inline-block;padding:3px 10px;border-radius:12px;font-size:12px;font-weight:bold;}"
".badge-on{background:#f6ffed;color:#52c41a;border:1px solid #b7eb8f;}"
".badge-off{background:#fff1f0;color:#ff4d4f;border:1px solid #ffa39e;}"
".badge-gray{background:#fafafa;color:#8c8c8c;border:1px solid #d9d9d9;}"
".card-box{background:#f9f9f9;padding:12px;border-radius:8px;margin-bottom:15px;border:1px solid #eee;}"
".point-card{background:#fff;border:1px solid #e8e8e8;border-radius:6px;padding:10px;margin-bottom:8px;display:flex;justify-content:space-between;align-items:center;}"
".point-val{font-size:20px;font-weight:bold;color:#1890ff;}"
"input,select{width:100%;padding:8px;margin:4px 0 8px;box-sizing:border-box;border:1px solid #d9d9d9;border-radius:4px;font-size:13px;}"
"input:disabled{background:#e9ecef;cursor:not-allowed;color:#6c757d;}"
"#pbar{width:100%;background:#e8e8e8;height:12px;border-radius:6px;overflow:hidden;display:none;margin-top:10px;}"
"#pval{width:0%;height:100%;background:#1890ff;}"
".hex-log{background:#272822;color:#a6e22e;font-family:monospace;padding:10px;border-radius:6px;font-size:12px;line-height:1.6;overflow-x:auto;}"
".login-card{max-width:380px;margin:40px auto;background:#fff;border-radius:10px;padding:26px;box-shadow:0 4px 18px rgba(0,0,0,0.1);text-align:center;border:1px solid #e8e8e8;}"
".login-icon{font-size:42px;color:#1890ff;margin-bottom:10px;}"
"</style></head><body>"
"<!-- 1. 独立安全登录界面 (默认未登录直接呈现) -->"
"<div id='login_view' class='login-card'>"
"<div class='login-icon'>🔒</div>"
"<h2 style='margin:0 0 8px;color:#1890ff;font-size:20px;'>ESP32-S3 工控网关</h2>"
"<p style='margin:0 0 20px;color:#888;font-size:13px;'>请输入管理员身份凭据访问控制台</p>"
"<div style='text-align:left;'>"
"<label style='font-size:13px;font-weight:bold;'>管理员账号:</label>"
"<input type='text' id='login_user' value='' placeholder='请输入管理员账号' autocomplete='username' onkeydown='if(event.key===\"Enter\")doLogin()'>"
"<label style='font-size:13px;font-weight:bold;'>管理员密码:</label>"
"<input type='password' id='login_pwd' value='' placeholder='请输入管理员密码' autocomplete='current-password' onkeydown='if(event.key===\"Enter\")doLogin()'>"
"</div>"
"<button class='btn btn-primary' style='width:100%;padding:10px;font-size:15px;margin-top:10px;' onclick='doLogin()'>登 录 系 统</button>"
"<div id='login_msg' style='font-size:12px;color:#ff4d4f;margin-top:10px;min-height:18px;'></div>"
"</div>"

"<!-- 2. 主控制台 (登录验证通过后展示) -->"
"<div id='main_view' class='container' style='display:none;'>"
"<div class='header' style='display:flex;justify-content:space-between;align-items:center;'><h2>ESP32-S3 智能工控控制台</h2><button class='btn btn-outline' style='font-size:12px;padding:4px 12px;' onclick='doLogout()'>安全退出</button></div>"
"<div class='tabs'>"
"<div class='tab active' onclick='switchTab(0)'>本地工作台</div>"
"<div class='tab' onclick='switchTab(1)'>485智能设备控制与采集</div>"
"<div class='tab' onclick='switchTab(2)'>CAN工业联动 (STM32)</div>"
"<div class='tab' onclick='switchTab(3)'>网络与系统配置</div>"
"</div>"

"<!-- 选项卡 1：本地工作台 -->"
"<div id='tab0' class='content active'>"
"<h3>本板 DO 输出控制</h3>"
"<div class='card-box'>"
"<div class='row'><span>DO 1 输出</span><div><span id='do_badge1' class='badge badge-off' style='margin-right:8px;'>关闭</span><button class='btn btn-on' onclick='setDO(1,1)'>开启</button><button class='btn btn-off' onclick='setDO(1,0)'>关闭</button></div></div>"
"<div class='row'><span>DO 2 输出</span><div><span id='do_badge2' class='badge badge-off' style='margin-right:8px;'>关闭</span><button class='btn btn-on' onclick='setDO(2,1)'>开启</button><button class='btn btn-off' onclick='setDO(2,0)'>关闭</button></div></div>"
"<div class='row'><span>DO 3 输出</span><div><span id='do_badge3' class='badge badge-off' style='margin-right:8px;'>关闭</span><button class='btn btn-on' onclick='setDO(3,1)'>开启</button><button class='btn btn-off' onclick='setDO(3,0)'>关闭</button></div></div>"
"<div class='row'><span>DO 4 输出</span><div><span id='do_badge4' class='badge badge-off' style='margin-right:8px;'>关闭</span><button class='btn btn-on' onclick='setDO(4,1)'>开启</button><button class='btn btn-off' onclick='setDO(4,0)'>关闭</button></div></div>"
"</div>"
"<h3>本板 DI 输入检测 (实时)</h3>"
"<div class='card-box'>"
"<div class='row'><span>DI 1 光耦</span><span id='di1' class='badge badge-off'>无信号</span></div>"
"<div class='row'><span>DI 2 光耦</span><span id='di2' class='badge badge-off'>无信号</span></div>"
"<div class='row'><span>DI 3 光耦</span><span id='di3' class='badge badge-off'>无信号</span></div>"
"<div class='row'><span>DI 4 光耦</span><span id='di4' class='badge badge-off'>无信号</span></div>"
"</div>"
"<h3>固件远程升级 (OTA)</h3>"
"<div class='card-box'>"
"<input type='file' id='ota_file' accept='.bin'>"
"<button class='btn btn-primary' onclick='uploadOTA()'>一键上传刷写固件</button>"
"<div id='pbar'><div id='pval'></div></div>"
"<div id='ota_msg' style='font-size:12px;color:#888;margin-top:6px;'>请选择编译生成的 .bin 固件</div>"
"</div>"
"</div>"

"<!-- 选项卡 2：485 智能设备控制与采集 (万能物模型引擎) -->"
"<div id='tab1' class='content'>"
"<h3>485 设备物模型控制中心</h3>"
"<div class='card-box' style='background:#e6f7ff;border:1px solid #91d5ff;'>"
"<div style='display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px;'>"
"<div><strong>⚡ 快速载入预设设备模板:</strong></div>"
"<div>"
"<button id='poll_btn' class='btn btn-outline' style='padding:4px 10px;font-size:12px;' onclick='togglePoll()'>自动轮询: 检测中</button>"
"</div>"
"</div>"
"<div style='display:flex;gap:8px;margin-top:8px;'>"
"<select id='tpl_type' style='flex:3;margin:0;'>"
"<option value='0'>DAM0404D 继电器卡 (4路DO输出 + 4路DI监测)</option>"
"<option value='1'>通用 8 路 Modbus 继电器控制板</option>"
"<option value='2'>标准 Modbus 温湿度环境传感器</option>"
"<option value='3'>通用变频器控制 (启停开关 + 频率设定)</option>"
"</select>"
"<input type='number' id='tpl_slave' value='254' placeholder='站号' style='flex:1;margin:0;width:60px;' title='设备从机地址(1~254)'>"
"<button class='btn btn-primary' style='flex:1;margin:0;padding:6px 12px;' onclick='applyTemplate()'>应用模板</button>"
"</div>"
"</div>"

"<div id='switch_section' style='display:none;'>"
"<div style='display:flex;justify-content:space-between;align-items:center;margin:12px 0 6px;'>"
"<strong style='color:#1890ff;'>■ 开关量 / 继电器远程控制区</strong>"
"<div>"
"<button class='btn btn-on' style='padding:4px 10px;font-size:12px;' onclick='ctrlAllSwitches(1)'>一键全部开启</button>"
"<button class='btn btn-off' style='padding:4px 10px;font-size:12px;' onclick='ctrlAllSwitches(0)'>一键全部关闭</button>"
"</div>"
"</div>"
"<div id='switch_box' class='card-box'></div>"
"</div>"

"<div id='di_section' style='display:none;'>"
"<strong style='color:#1890ff;display:block;margin:12px 0 6px;'>■ 离散量 / 光耦输入实时监测区</strong>"
"<div id='di_box' class='card-box' style='display:flex;flex-wrap:wrap;gap:8px;'></div>"
"</div>"

"<div id='analog_section' style='display:none;'>"
"<strong style='color:#1890ff;display:block;margin:12px 0 6px;'>■ 模拟量 / 传感器数据采集看板</strong>"
"<div id='analog_box' class='card-box'></div>"
"</div>"

"<div id='holding_section' style='display:none;'>"
"<strong style='color:#1890ff;display:block;margin:12px 0 6px;'>■ 寄存器数值设定区 (如变频器调速/设参)</strong>"
"<div id='holding_box' class='card-box'></div>"
"</div>"

"<div id='empty_box' class='card-box' style='text-align:center;color:#888;padding:25px;'>暂无配置点位，请选择上方模板一键生成，或在下方手动添加</div>"

"<h3>+ 自定义点位配置 / 修改</h3>"
"<div class='card-box'>"
"<div style='display:flex;gap:8px;'>"
"<div style='flex:1;'><label>槽位:</label><select id='pt_idx'></select></div>"
"<div style='flex:2;'><label>点位名称:</label><input type='text' id='pt_name' placeholder='如: 5号继电器 / 进线电压'></div>"
"<div style='flex:2;'><label>点位形态:</label><select id='pt_kind'>"
"<option value='0'>开关控制型 (05写单线圈/继电器)</option>"
"<option value='1'>状态监测型 (02读离散输入/光耦)</option>"
"<option value='2'>模拟量看板型 (03/04读寄存器)</option>"
"<option value='3'>数值设定型 (06写单寄存器/调速)</option>"
"</select></div>"
"</div>"
"<div style='display:flex;gap:8px;'>"
"<div style='flex:1;'><label>站号(ID):</label><input type='number' id='pt_slave' value='1'></div>"
"<div style='flex:1;'><label>寄存器地址:</label><input type='number' id='pt_addr' value='0'></div>"
"<div style='flex:1;'><label>倍率:</label><input type='number' step='0.001' id='pt_scale' value='0.1'></div>"
"<div style='flex:1;'><label>工程单位:</label><input type='text' id='pt_unit' placeholder='℃, V, Hz'></div>"
"</div>"
"<div style='display:flex;gap:8px;margin-top:6px;'>"
"<button class='btn btn-on' style='flex:1;' onclick='saveCustomPoint()'>保存该点位</button>"
"<button class='btn btn-off' style='flex:1;' onclick='deleteCustomPoint()'>删除该点位</button>"
"<button class='btn btn-outline' style='flex:1;' onclick='clearAllPoints()'>清空全部点位</button>"
"</div>"
"<div id='pt_msg' style='font-size:12px;color:#1890ff;margin-top:4px;'></div>"
"</div>"

"<h3>万能 Modbus 调试与控制助手 (支持任意第三方 485 设备)</h3>"
"<div class='card-box'>"
"<div style='display:flex;gap:8px;'>"
"<div style='flex:1;'><label>从机站号:</label><input type='number' id='raw_slave' value='254'></div>"
"<div style='flex:2;'><label>功能码:</label><select id='raw_fc'>"
"<option value='5'>05 写单线圈 (控制继电器/开关)</option>"
"<option value='6'>06 写单保持寄存器 (变频调速/设参)</option>"
"<option value='15'>0F 写多线圈 (全开全关)</option>"
"<option value='1'>01 读线圈状态</option>"
"<option value='2'>02 读离散输入</option>"
"<option value='3'>03 读保持寄存器</option>"
"<option value='4'>04 读输入寄存器</option>"
"</select></div>"
"</div>"
"<div style='display:flex;gap:8px;'>"
"<div style='flex:1;'><label>起始地址(十进制):</label><input type='number' id='raw_addr' value='0'></div>"
"<div style='flex:1;'><label>参数值/数量(十进制):</label><input type='number' id='raw_val' value='65280' title='05码65280为开,0为关;读码为读取数量'></div>"
"</div>"
"<button class='btn btn-primary' style='width:100%;padding:9px;margin-top:4px;' onclick='sendRawModbus()'>一键组包并发送 (自动计算 CRC16)</button>"
"<div style='margin-top:8px;'><div class='hex-log' id='raw_log'>等待发送指令...</div></div>"
"</div>"
"</div>"

"<!-- 选项卡 3：CAN 工业联动与 STM32 控制台 -->"
"<div id='tab2' class='content'>"
"<h3>启明欣欣 STM32F407 节点联动 (CAN1 500 kbps)</h3>"
"<div class='card-box'>"
"<div class='row'>"
"<div><b>STM32 板载指示灯 (D1)</b><div style='font-size:12px;color:#888;'>通过 CAN 发送标准数据帧 ID: 0x101 (01=点亮短鸣 / 00=熄灭)</div></div>"
"<div>"
"<button class='btn btn-on' onclick='ctrlStm32Led(1)'>点亮 D1</button>"
"<button class='btn btn-off' onclick='ctrlStm32Led(0)'>熄灭 D1</button>"
"</div>"
"</div>"
"<div class='row'><span>STM32 当前目标状态:</span><span id='can_stm32_d0_status' class='badge badge-gray'>未知</span></div>"
"<div class='row'><span>STM32 按键 (KEY0) 上报状态:</span><span id='can_stm32_key0_status' class='badge badge-gray'>等待按键按下...</span></div>"
"</div>"

"<h3>万能 CAN 报文发送与测试</h3>"
"<div class='card-box'>"
"<div style='display:flex;gap:10px;'>"
"<div style='flex:1;'><label style='font-size:12px;'>报文 ID (Hex):</label><input type='text' id='can_tx_id' value='0x101' placeholder='如 0x101'></div>"
"<div style='flex:1;'><label style='font-size:12px;'>帧格式:</label><select id='can_tx_type'><option value='0'>标准帧 (11位)</option><option value='1'>扩展帧 (29位)</option></select></div>"
"</div>"
"<label style='font-size:12px;'>数据负载 (Hex，最多8字节，空格分隔):</label>"
"<input type='text' id='can_tx_data' value='01' placeholder='如 01 02 03'>"
"<div style='display:flex;justify-content:space-between;align-items:center;margin-top:4px;'>"
"<button class='btn btn-primary' onclick='sendCanRaw()'>发送 CAN 报文</button>"
"<span id='can_tx_msg' style='font-size:12px;'></span>"
"</div>"
"</div>"

"<h3>CAN 总线报文监视器 (500 kbps)</h3>"
"<div class='card-box'>"
"<div style='display:flex;justify-content:space-between;margin-bottom:8px;font-size:13px;'>"
"<span>TX 计数: <b id='can_tx_count' style='color:#1890ff;'>0</b> | RX 计数: <b id='can_rx_count' style='color:#52c41a;'>0</b></span>"
"<button class='btn btn-outline' style='padding:2px 8px;font-size:11px;' onclick='clearCanLogs()'>清空日志</button>"
"</div>"
"<div id='can_traffic_log' class='hex-log' style='height:160px;overflow-y:auto;white-space:pre-wrap;'>等待通信数据...</div>"
"</div>"
"</div>"
"<!-- 选项卡 4：网络与系统配置 -->"
"<div id='tab3' class='content'>"
"<h3>无线 Wi-Fi 配置 (断电记忆)</h3>"
"<div class='card-box'>"
"<div class='row'><span>Wi-Fi 状态:</span><span id='wifi_status' class='badge badge-off'>未连接</span></div>"
"<div class='row'><span>Wi-Fi IP 地址:</span><span id='wifi_ip'>-</span></div>"
"<button class='btn btn-primary' style='margin-bottom:8px;' onclick='scanWifi()'>扫描周边 Wi-Fi 热点</button>"
"<label>选择周边热点:</label><select id='ssid_select'><option value=''>点击上方按钮扫描</option></select>"
"<label>或手动输入 SSID:</label><input type='text' id='ssid_input' placeholder='Wi-Fi 名称'>"
"<label>Wi-Fi 密码:</label><input type='password' id='wifi_pwd' placeholder='请输入密码'>"
"<button class='btn btn-on' style='width:100%;padding:10px;' onclick='connectWifi()'>保存并连接 (断电自动重连)</button>"
"<div id='wifi_msg' style='font-size:12px;color:#1890ff;margin-top:6px;'></div>"
"</div>"

"<h3>有线以太网配置 (W5500)</h3>"
"<div class='card-box'>"
"<div class='row'><span>物理链路:</span><span id='eth_link' class='badge badge-on'>连接正常</span></div>"
"<div class='row'><span>物理 MAC:</span><span id='eth_mac'>-</span></div>"
"<label>IP 分配模式:</label>"
"<select id='eth_mode' onchange='toggleEthMode()'>"
"<option value='dhcp'>DHCP (动态获取 - 默认)</option>"
"<option value='static'>Static (静态指定 IP)</option>"
"</select>"
"<div id='eth_static_inputs'>"
"<label>静态 IP 地址:</label><input type='text' id='eth_ip' placeholder='192.168.1.188' disabled>"
"<label>子网掩码:</label><input type='text' id='eth_mask' placeholder='255.255.255.0' disabled>"
"<label>默认网关:</label><input type='text' id='eth_gw' placeholder='192.168.1.1' disabled>"
"<label>DNS 服务器:</label><input type='text' id='eth_dns' placeholder='114.114.114.114' disabled>"
"</div>"
"<button class='btn btn-on' style='width:100%;padding:10px;' onclick='saveEthConfig()'>保存并应用以太网配置</button>"
"<div id='eth_msg' style='font-size:12px;color:#1890ff;margin-top:6px;'></div>"
"</div>"
"</div>"

"</div>"
"</div>"
"<script>"
"let g_dido_timer = null, g_pts_timer = null, g_can_timer = null;"
"/* --- 登录与认证管理 --- */"
"function checkAuth(){"
"  fetch('/api/auth/status', {credentials:'include'})"
"    .then(r=>r.json())"
"    .then(d=>{"
"      let lv = document.getElementById('login_view');"
"      let mv = document.getElementById('main_view');"
"      if(d && d.authenticated){"
"        if(lv) lv.style.display = 'none';"
"        if(mv) mv.style.display = 'block';"
"        startTimers();"
"      } else {"
"        if(lv) lv.style.display = 'block';"
"        if(mv) mv.style.display = 'none';"
"        stopTimers();"
"      }"
"    }).catch(e=>{"
"      let lv = document.getElementById('login_view');"
"      let mv = document.getElementById('main_view');"
"      if(lv) lv.style.display = 'block';"
"      if(mv) mv.style.display = 'none';"
"      stopTimers();"
"    });"
"}"
"function doLogin(){"
"  var u = document.getElementById('login_user').value.trim();"
"  var p = document.getElementById('login_pwd').value.trim();"
"  var msg = document.getElementById('login_msg');"
"  if(!u || !p){ msg.innerText = '请输入账号和密码！'; msg.style.color = '#ff4d4f'; return; }"
"  msg.innerText = '正在验证身份...'; msg.style.color = '#1890ff';"
"  fetch('/api/login', {method:'POST', credentials:'include', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:new URLSearchParams({user:u,pwd:p})})"
"    .then(r=>r.json())"
"    .then(d=>{"
"      let lv = document.getElementById('login_view');"
"      let mv = document.getElementById('main_view');"
"      if(d && d.success){"
"        msg.innerText = '登录成功，正在进入系统...'; msg.style.color = '#52c41a';"
"        if(lv) lv.style.display = 'none';"
"        if(mv) mv.style.display = 'block';"
"        startTimers();"
"      } else {"
"        msg.innerText = (d && d.msg) ? d.msg : '账号或密码错误！'; msg.style.color = '#ff4d4f';"
"      }"
"    })"
"    .catch(err=>{"
"      msg.innerText = '登录请求异常: ' + err; msg.style.color = '#ff4d4f';"
"    });"
"}"
"function doLogout(){"
"  if(!confirm('确定要退出管理员登录吗？')) return;"
"  fetch('/api/logout', {method:'POST', credentials:'include'}).then(r=>r.json()).then(d=>{"
"    checkAuth();"
"  }).catch(()=>{ checkAuth(); });"
"}"
"function startTimers(){"
"  if(!g_dido_timer) g_dido_timer = setInterval(updateDIDO, 600);"
"  if(!g_pts_timer) g_pts_timer = setInterval(updatePoints, 1000);"
"  if(!g_can_timer) g_can_timer = setInterval(updateCanStatus, 1000);"
"  updateDIDO(); updatePoints(); updateCanStatus(); loadWifiInfo(); loadEthInfo(); checkPollStatus();"
"}"
"function stopTimers(){"
"  if(g_dido_timer){ clearInterval(g_dido_timer); g_dido_timer = null; }"
"  if(g_pts_timer){ clearInterval(g_pts_timer); g_pts_timer = null; }"
"  if(g_can_timer){ clearInterval(g_can_timer); g_can_timer = null; }"
"}"
"function switchTab(idx){"
"  let tabs = document.querySelectorAll('.tab'); let contents = document.querySelectorAll('.content');"
"  tabs.forEach((t, i) => { t.classList.toggle('active', i === idx); });"
"  contents.forEach((c, i) => { c.classList.toggle('active', i === idx); });"
"}"

"function updateDIDO(){"
"  fetch('/api/di', {credentials:'include'}).then(r=>{"
"    if(r.status===401){ return null; }"
"    return r.json();"
"  }).then(d=>{"
"    if(!d || (d.di===undefined && d.di1===undefined)) return;"
"    for(let i=1;i<=4;i++){"
"      let el = document.getElementById('di'+i);"
"      if(d['di'+i]===1){el.className='badge badge-on';el.innerText='有信号 (闭合)';}"
"      else{el.className='badge badge-off';el.innerText='无信号 (断开)';}"
"      let do_el = document.getElementById('do_badge'+i);"
"      if(do_el && d['do'+i]!==undefined){"
"        if(d['do'+i]===1){do_el.className='badge badge-on';do_el.innerText='开启 (吸合)';}"
"        else{do_el.className='badge badge-off';do_el.innerText='关闭';}"
"      }"
"    }"
"  }).catch(e=>{});"
"}"
""

"function setDO(ch, st){"
"  fetch('/api/do?ch='+ch+'&state='+st, {credentials:'include'}).then(r=>r.text()).then(t=>{updateDIDO();});"
"}"

"/* --- 万能 485 动态物模型渲染与控制 --- */"
"let g_pts = [];"
"function initSlots(){"
"  let sel = document.getElementById('pt_idx'); sel.innerHTML = '';"
"  for(let i=0;i<24;i++){"
"    let opt = document.createElement('option'); opt.value = i; opt.innerText = '槽位 ' + (i+1);"
"    sel.appendChild(opt);"
"  }"
"}"
"initSlots();"

"function renderPoints(pts){\n  if(!pts || !Array.isArray(pts)) return;"
"  g_pts = pts;"
"  let swBox = document.getElementById('switch_box'); swBox.innerHTML = '';"
"  let diBox = document.getElementById('di_box'); diBox.innerHTML = '';"
"  let anBox = document.getElementById('analog_box'); anBox.innerHTML = '';"
"  let hdBox = document.getElementById('holding_box'); hdBox.innerHTML = '';"
"  let hasSw=false, hasDi=false, hasAn=false, hasHd=false, total=0;"

"  pts.forEach((p, idx)=>{"
"    if(!p.enabled) return;"
"    total++;"
"    if(p.kind === 0){"
"      hasSw = true;"
"      let div = document.createElement('div'); div.className = 'point-card';"
"      let badgeCls = p.state ? 'badge-on' : 'badge-off';"
"      let badgeTxt = p.state ? '已吸合/开启' : '已断开/关闭';"
"      div.innerHTML = '<div><strong>'+p.name+'</strong> <span style=\"font-size:12px;color:#888;\">(站号:'+p.slave+' 地址:0x'+p.reg.toString(16)+')</span><br><span class=\"badge '+badgeCls+'\">'+badgeTxt+'</span> <span style=\"font-size:11px;color:'+(p.online?'#52c41a':'#ff4d4f')+';\">'+(p.online?'● 在线':'● 离线')+'</span></div>' +"
"                      '<div><button class=\"btn btn-on\" onclick=\"ctrlSwitch('+idx+',1)\">开启</button><button class=\"btn btn-off\" onclick=\"ctrlSwitch('+idx+',0)\">关闭</button></div>';"
"      swBox.appendChild(div);"
"    } else if(p.kind === 1){"
"      hasDi = true;"
"      let span = document.createElement('div');"
"      span.style = 'flex:1;min-width:140px;background:#fff;border:1px solid #d9d9d9;border-radius:6px;padding:8px 12px;display:flex;justify-content:space-between;align-items:center;';"
"      let cls = p.state ? 'badge-on' : 'badge-off';"
"      let txt = p.state ? '导通 (有信号)' : '断开 (无信号)';"
"      span.innerHTML = '<span>'+p.name+'</span><span class=\"badge '+cls+'\">'+txt+'</span>';"
"      diBox.appendChild(span);"
"    } else if(p.kind === 2){"
"      hasAn = true;"
"      let div = document.createElement('div'); div.className = 'point-card';"
"      div.innerHTML = '<div><strong>'+p.name+'</strong> <span style=\"font-size:12px;color:#888;\">(站号:'+p.slave+')</span><br><span class=\"badge '+(p.online?'badge-on':'badge-off')+'\">'+(p.online?'通信正常':'通信超时')+'</span></div>' +"
"                      '<div class=\"point-val\">'+p.val.toFixed(2)+' <span style=\"font-size:14px;color:#666;\">'+p.unit+'</span></div>';"
"      anBox.appendChild(div);"
"    } else if(p.kind === 3){"
"      hasHd = true;"
"      let div = document.createElement('div'); div.className = 'point-card';"
"      div.innerHTML = '<div><strong>'+p.name+'</strong> <span style=\"font-size:12px;color:#888;\">(站号:'+p.slave+')</span><br>当前设定值: <span style=\"font-weight:bold;color:#1890ff;\">'+p.val.toFixed(2)+' '+p.unit+'</span></div>' +"
"                      '<div style=\"display:flex;gap:6px;\"><input type=\"number\" id=\"hd_in_'+idx+'\" value=\"'+p.val+'\" style=\"width:80px;padding:4px;margin:0;\"><button class=\"btn btn-primary\" style=\"padding:4px 10px;\" onclick=\"setHoldingVal('+idx+')\">设定</button></div>';"
"      hdBox.appendChild(div);"
"    }"
"  });"

"  document.getElementById('switch_section').style.display = hasSw ? 'block' : 'none';"
"  document.getElementById('di_section').style.display = hasDi ? 'block' : 'none';"
"  document.getElementById('analog_section').style.display = hasAn ? 'block' : 'none';"
"  document.getElementById('holding_section').style.display = hasHd ? 'block' : 'none';"
"  document.getElementById('empty_box').style.display = (total === 0) ? 'block' : 'none';"
"}"

"function updatePoints(){"
"  fetch('/api/rs485/points', {credentials:'include'}).then(r=>{"
"    if(r.status===401){ return null; }"
"    return r.json();"
"  }).then(pts=>{ if(pts) renderPoints(pts); }).catch(e=>{});"
"}"
""

"function ctrlSwitch(idx, state){"
"  fetch('/api/rs485/ctrl/switch?idx='+idx+'&state='+state, {method:'POST', credentials:'include'}).then(r=>r.text()).then(t=>{updatePoints();});"
"}"

"function ctrlAllSwitches(state){"
"  fetch('/api/rs485/ctrl/all?state='+state, {method:'POST', credentials:'include'}).then(r=>r.text()).then(t=>{updatePoints();});"
"}"

"function setHoldingVal(idx){"
"  let v = document.getElementById('hd_in_'+idx).value;"
"  fetch('/api/rs485/ctrl/write?idx='+idx+'&val='+v, {method:'POST', credentials:'include'}).then(r=>r.text()).then(t=>{updatePoints();});"
"}"

"function applyTemplate(){"
"  let t = document.getElementById('tpl_type').value;"
"  let s = document.getElementById('tpl_slave').value;"
"  if(!confirm('应用该模板将重新配置点位表，是否继续？')) return;"
"  fetch('/api/rs485/template?type='+t+'&slave='+s, {method:'POST', credentials:'include'}).then(r=>r.text()).then(res=>{"
"    alert('预设设备模板应用成功！'); updatePoints();"
"  });"
"}"

"function togglePoll(){"
"  fetch('/api/rs485/poll', {credentials:'include'}).then(r=>r.json()).then(d=>{"
"    let next = d.auto_poll ? 0 : 1;"
"    fetch('/api/rs485/poll?enable='+next, {method:'POST', credentials:'include'}).then(r=>r.text()).then(t=>{"
"      checkPollStatus();"
"    });"
"  });"
"}"

"function checkPollStatus(){"
"  fetch('/api/rs485/poll').then(r=>r.json()).then(d=>{"
"    let b = document.getElementById('poll_btn');"
"    if(d.auto_poll){ b.innerText = '● 自动轮询: 运行中 (点击暂停)'; b.style.color='#52c41a'; b.style.borderColor='#b7eb8f'; }"
"    else{ b.innerText = '○ 自动轮询: 已暂停 (总线静止/灯不闪)'; b.style.color='#faad14'; b.style.borderColor='#ffe58f'; }"
"  });"
"}"
""

"function saveCustomPoint(){"
"  let idx = document.getElementById('pt_idx').value;"
"  let name = document.getElementById('pt_name').value;"
"  let kind = document.getElementById('pt_kind').value;"
"  let slave = document.getElementById('pt_slave').value;"
"  let addr = document.getElementById('pt_addr').value;"
"  let scale = document.getElementById('pt_scale').value;"
"  let unit = document.getElementById('pt_unit').value;"
"  if(!name){alert('点位名称不能为空！');return;}"
"  let url = '/api/rs485/set?idx='+idx+'&name='+encodeURIComponent(name)+'&kind='+kind+'&slave='+slave+'&addr='+addr+'&scale='+scale+'&unit='+encodeURIComponent(unit);"
"  fetch(url, {method:'POST'}).then(r=>r.text()).then(t=>{"
"    document.getElementById('pt_msg').innerText = '点位已成功保存！';"
"    setTimeout(()=>{document.getElementById('pt_msg').innerText='';}, 2500);"
"    updatePoints();"
"  });"
"}"

"function deleteCustomPoint(){"
"  let idx = document.getElementById('pt_idx').value;"
"  fetch('/api/rs485/del?idx='+idx, {method:'POST', credentials:'include'}).then(r=>r.text()).then(t=>{"
"    document.getElementById('pt_msg').innerText = '点位已删除！';"
"    setTimeout(()=>{document.getElementById('pt_msg').innerText='';}, 2500);"
"    updatePoints();"
"  });"
"}"

"function clearAllPoints(){"
"  if(!confirm('确定要清空所有 485 点位吗？')) return;"
"  fetch('/api/rs485/clear', {method:'POST', credentials:'include'}).then(r=>r.text()).then(t=>{"
"    updatePoints();"
"  });"
"}"

"/* --- 万能 Modbus 调试助手 --- */"
"function sendRawModbus(){"
"  let slave = document.getElementById('raw_slave').value;"
"  let fc = document.getElementById('raw_fc').value;"
"  let addr = document.getElementById('raw_addr').value;"
"  let val = document.getElementById('raw_val').value;"
"  let log = document.getElementById('raw_log');"
"  log.innerText = '正在发送并等待从机响应...';"
"  fetch('/api/rs485/raw?slave='+slave+'&fc='+fc+'&addr='+addr+'&val='+val, {method:'POST', credentials:'include'})"
"    .then(r=>r.json()).then(res=>{"
"      let str = '>> 发送 TX: ' + res.tx + '\\n';"
"      if(res.success){"
"        str += '<< 接收 RX: ' + res.rx + '\\n状态: [成功] 从机应答正常，CRC 校验通过！';"
"        log.style.color = '#a6e22e';"
"      } else {"
"        str += '<< 接收 RX: ' + (res.rx || '无数据') + '\\n状态: [失败] ' + res.err;"
"        log.style.color = '#ff4d4f';"
"      }"
"      log.innerText = str;"
"      updatePoints();"
"    }).catch(e=>{ log.innerText = '请求失败: ' + e; log.style.color = '#ff4d4f'; });"
"}"


"/* --- CAN 总线与 STM32 联动业务 --- */"
"function ctrlStm32Led(st){"
"  fetch('/api/can/stm32/led?state='+st, {method:'POST', credentials:'include'})"
"    .then(r=>r.json())"
"    .then(d=>{"
"      if(d && d.success){ updateCanStatus(); }"
"      else{ alert('CAN 发送失败: ' + (d.err || '从机未响应')); }"
"    }).catch(e=>{ alert('请求失败: ' + e); });"
"}"

"function sendCanRaw(){"
"  let id = document.getElementById('can_tx_id').value.trim();"
"  let ext = document.getElementById('can_tx_type').value;"
"  let data = document.getElementById('can_tx_data').value.trim();"
"  let msg = document.getElementById('can_tx_msg');"
"  if(!id){ msg.innerText = '请输入报文 ID！'; msg.style.color='#ff4d4f'; return; }"
"  msg.innerText = '正在发送...'; msg.style.color='#1890ff';"
"  fetch('/api/can/send?id='+encodeURIComponent(id)+'&data='+encodeURIComponent(data)+'&ext='+ext, {method:'POST', credentials:'include'})"
"    .then(r=>r.json())"
"    .then(d=>{"
"      if(d && d.success){"
"        msg.innerText = '发送成功！'; msg.style.color='#52c41a';"
"        updateCanStatus();"
"      } else {"
"        msg.innerText = '发送失败: ' + (d.err || '从机未确认'); msg.style.color='#ff4d4f';"
"      }"
"    }).catch(e=>{ msg.innerText = '网络错误: ' + e; msg.style.color='#ff4d4f'; });"
"}"

"function clearCanLogs(){"
"  fetch('/api/can/clear', {method:'POST', credentials:'include'})"
"    .then(r=>r.json()).then(d=>{ updateCanStatus(); });"
"}"

"function updateCanStatus(){"
"  fetch('/api/can/status?_=' + Date.now(), {credentials:'include'})"
"    .then(r=>{"
"      if(r.status===401){ return null; }"
"      return r.json();"
"    })"
"    .then(d=>{"
"      if(!d) return;"
"      document.getElementById('can_tx_count').innerText = d.tx_count || 0;"
"      document.getElementById('can_rx_count').innerText = d.rx_count || 0;"
"      let d0 = document.getElementById('can_stm32_d0_status');"
"      if(d.stm32_d0){ d0.className='badge badge-on'; d0.innerText='已点亮 (01)'; }"
"      else{ d0.className='badge badge-off'; d0.innerText='已熄灭 (00)'; }"
"      let k0 = document.getElementById('can_stm32_key0_status');"
"      if(d.key0){ k0.className='badge badge-on'; k0.innerText='已触发 (时间: ' + (d.key0_time || '') + ')'; }"
"      else{ k0.className='badge badge-gray'; k0.innerText='等待按键按下...'; }"
"      let logBox = document.getElementById('can_traffic_log');"
"      if(d.logs && d.logs.length > 0){"
"        let lines = d.logs.map(item => '[' + item.time + '] ' + (item.dir==='TX'?'>> TX':'<< RX') + ' ID:' + item.id + (item.is_ext?'(EXT)':'') + ' DLC:' + item.len + ' [' + item.data + '] ' + (item.desc || ''));"
"        logBox.innerText = lines.join(String.fromCharCode(10));"
"      } else {"
"        logBox.innerText = '总线就绪 (波特率 500 kbps，无通信活动)...';"
"      }"
"    }).catch(e=>{});"
"}"

"/* --- 网络与系统配置 --- */"
"function loadWifiInfo(){"
"  fetch('/api/wifi/info', {credentials:'include'}).then(r=>r.json()).then(d=>{"
"    let s = document.getElementById('wifi_status');"
"    if(d.connected){s.className='badge badge-on';s.innerText='已连接到: '+d.ssid;document.getElementById('wifi_ip').innerText=d.ip;}"
"    else{s.className='badge badge-off';s.innerText='未连接';document.getElementById('wifi_ip').innerText='-';}"
"  }).catch(e=>{});"
"}"
""

"function scanWifi(){"
"  let sel = document.getElementById('ssid_select');sel.innerHTML='<option>正在扫描周边 Wi-Fi...</option>';"
"  fetch('/api/wifi/scan', {credentials:'include'}).then(r=>r.json()).then(list=>{"
"    sel.innerHTML='<option value=\"\">-- 请选择 Wi-Fi --</option>';"
"    list.forEach(item=>{"
"      let opt=document.createElement('option');opt.value=item.ssid;opt.innerText=item.ssid+' ('+item.rssi+' dBm)';sel.appendChild(opt);"
"    });"
"    sel.onchange=function(){document.getElementById('ssid_input').value=sel.value;};"
"  });"
"}"

"function connectWifi(){"
"  let s=document.getElementById('ssid_input').value;let p=document.getElementById('wifi_pwd').value;let m=document.getElementById('wifi_msg');"
"  if(!s){alert('SSID 不能为空！');return;}"
"  m.innerText='正在发起连接并写入 Flash 记忆...';"
"  fetch('/api/wifi/connect?ssid='+encodeURIComponent(s)+'&pwd='+encodeURIComponent(p), {method:'POST', credentials:'include'})"
"    .then(r=>r.text()).then(t=>{m.innerText=t;setTimeout(loadWifiInfo, 4000);});"
"}"

"function toggleEthMode(){"
"  let isStatic = (document.getElementById('eth_mode').value === 'static');"
"  ['eth_ip','eth_mask','eth_gw','eth_dns'].forEach(id=>{"
"    document.getElementById(id).disabled = !isStatic;"
"  });"
"}"

"function loadEthInfo(){"
"  fetch('/api/eth/info', {credentials:'include'}).then(r=>r.json()).then(d=>{"
"    document.getElementById('eth_mac').innerText = d.mac;"
"    document.getElementById('eth_link').className = d.link ? 'badge badge-on' : 'badge badge-off';"
"    document.getElementById('eth_link').innerText = d.link ? '连接正常' : '网线未插';"
"    document.getElementById('eth_mode').value = d.dhcp ? 'dhcp' : 'static';"
"    document.getElementById('eth_ip').value = d.ip;"
"    document.getElementById('eth_mask').value = d.netmask;"
"    document.getElementById('eth_gw').value = d.gw;"
"    document.getElementById('eth_dns').value = d.dns;"
"    toggleEthMode();"
"  }).catch(e=>{});"
"}"
""

"function saveEthConfig(){"
"  let is_dhcp = (document.getElementById('eth_mode').value === 'dhcp') ? 1 : 0;"
"  let ip = document.getElementById('eth_ip').value;let mask = document.getElementById('eth_mask').value;"
"  let gw = document.getElementById('eth_gw').value;let dns = document.getElementById('eth_dns').value;"
"  let msg = document.getElementById('eth_msg');msg.innerText = '正在保存配置并应用...';"
"  fetch('/api/eth/config?dhcp='+is_dhcp+'&ip='+ip+'&mask='+mask+'&gw='+gw+'&dns='+dns, {method:'POST', credentials:'include'})"
"    .then(r=>r.text()).then(t=>{msg.innerText = '配置已生效！';setTimeout(loadEthInfo, 1500);});"
"}"

"function uploadOTA(){"
"  let file=document.getElementById('ota_file').files[0];if(!file){alert('请先选择 .bin 固件！');return;}"
"  let msg=document.getElementById('ota_msg');let pbar=document.getElementById('pbar');let pval=document.getElementById('pval');"
"  pbar.style.display='block';pval.style.width='0%';msg.innerText='正在上传刷写固件...';msg.style.color='#1890ff';"
"  let xhr=new XMLHttpRequest();xhr.open('POST','/ota/upload',true);"
"  xhr.upload.onprogress=function(e){if(e.lengthComputable){let p=Math.round((e.loaded/e.total)*100);pval.style.width=p+'%';msg.innerText='上传刷写中: '+p+'%';}};"
"  xhr.onload=function(){if(xhr.status==200){msg.innerText='升级成功！12秒后刷新页面...';msg.style.color='#52c41a';setTimeout(()=>{location.reload();},12000);}else{msg.innerText='升级失败: '+xhr.responseText;msg.style.color='#ff4d4f';}};"
"  xhr.onerror=function(){msg.innerText='网络错误！升级失败。';msg.style.color='#ff4d4f';};"
"  xhr.send(file);"
"}"
"/* 页面加载完成执行鉴权检查 */"
"checkAuth();"
"</script></body></html>";

/* ---------------- HTTP 路由处理器 ---------------- */

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    size_t total_len = strlen(INDEX_HTML);
    size_t sent = 0;
    while (sent < total_len) {
        size_t chunk_len = (total_len - sent > 4096) ? 4096 : (total_len - sent);
        esp_err_t err = httpd_resp_send_chunk(req, INDEX_HTML + sent, chunk_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTML 分块发送失败: %s", esp_err_to_name(err));
            return err;
        }
        sent += chunk_len;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t do_ctrl_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char ch_str[4] = {0}, state_str[4] = {0};
        httpd_query_key_value(buf, "ch", ch_str, sizeof(ch_str));
        httpd_query_key_value(buf, "state", state_str, sizeof(state_str));
        uint8_t ch = (uint8_t)atoi(ch_str);
        bool state = (atoi(state_str) == 1);
        if (ch >= 1 && ch <= 4) {
            bsp_do_set(ch, state);
            ESP_LOGI(TAG, "网页控制 DO%d -> %s", ch, state ? "开启" : "关闭");
        }
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t di_get_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"di1\":%d,\"di2\":%d,\"di3\":%d,\"di4\":%d,\"do1\":%d,\"do2\":%d,\"do3\":%d,\"do4\":%d}",
             bsp_di_get_raw(1) ? 1 : 0, bsp_di_get_raw(2) ? 1 : 0,
             bsp_di_get_raw(3) ? 1 : 0, bsp_di_get_raw(4) ? 1 : 0,
             bsp_do_get(1) ? 1 : 0, bsp_do_get(2) ? 1 : 0,
             bsp_do_get(3) ? 1 : 0, bsp_do_get(4) ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* 485 物模型点位查询接口 GET /api/rs485/points */
static esp_err_t rs485_points_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    uint8_t count = 0;
    rs485_point_t *pts = app_rs485_engine_get_points(&count);

    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int offset = snprintf(buf, 4096, "[");
    for (int i = 0; i < count; i++) {
        offset += snprintf(buf + offset, 4096 - offset,
                           "%s{\"idx\":%d,\"enabled\":%s,\"name\":\"%s\",\"kind\":%d,\"slave\":%d,\"reg\":%d,\"state\":%s,\"val\":%.2f,\"unit\":\"%s\",\"online\":%s}",
                           (i > 0) ? "," : "",
                           i,
                           pts[i].enabled ? "true" : "false",
                           pts[i].name,
                           (int)pts[i].kind,
                           pts[i].slave_id,
                           pts[i].reg_addr,
                           pts[i].current_state ? "true" : "false",
                           pts[i].current_val,
                           pts[i].unit,
                           pts[i].is_online ? "true" : "false");
    }
    snprintf(buf + offset, 4096 - offset, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

/* 485 开关点位即时控制 POST /api/rs485/ctrl/switch?idx=0&state=1 */
static esp_err_t rs485_ctrl_switch_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char idx_str[4] = {0}, state_str[4] = {0};
        httpd_query_key_value(buf, "idx", idx_str, sizeof(idx_str));
        httpd_query_key_value(buf, "state", state_str, sizeof(state_str));
        uint8_t idx = (uint8_t)atoi(idx_str);
        bool state = (atoi(state_str) == 1);
        app_rs485_engine_control_switch(idx, state);
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* 485 一键全开全关 POST /api/rs485/ctrl/all?state=1 */
static esp_err_t rs485_ctrl_all_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char state_str[4] = {0};
        httpd_query_key_value(buf, "state", state_str, sizeof(state_str));
        bool state = (atoi(state_str) == 1);
        app_rs485_engine_control_all_switches(state);
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* 485 数值设定 POST /api/rs485/ctrl/write?idx=0&val=45.5 */
static esp_err_t rs485_ctrl_write_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char idx_str[4] = {0}, val_str[16] = {0};
        httpd_query_key_value(buf, "idx", idx_str, sizeof(idx_str));
        httpd_query_key_value(buf, "val", val_str, sizeof(val_str));
        uint8_t idx = (uint8_t)atoi(idx_str);
        float val = (float)atof(val_str);
        app_rs485_engine_write_value(idx, val);
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* 485 快速应用预设设备模板 POST /api/rs485/template?type=0&slave=254 */
static esp_err_t rs485_template_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char type_str[4] = {0}, slave_str[8] = {0};
        httpd_query_key_value(buf, "type", type_str, sizeof(type_str));
        httpd_query_key_value(buf, "slave", slave_str, sizeof(slave_str));
        rs485_template_type_t t = (rs485_template_type_t)atoi(type_str);
        uint8_t s = (uint8_t)atoi(slave_str);
        if (s == 0) s = 1;
        app_rs485_engine_apply_template(t, s);
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* 485 自动轮询控制 GET & POST /api/rs485/poll */
static esp_err_t rs485_poll_get_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"auto_poll\":%s}", app_rs485_engine_is_auto_poll() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t rs485_poll_post_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char en_str[4] = {0};
        httpd_query_key_value(buf, "enable", en_str, sizeof(en_str));
        app_rs485_engine_set_auto_poll(atoi(en_str) == 1);
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* 485 点位自定义配置 POST /api/rs485/set */
static esp_err_t rs485_set_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[256];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char idx_str[4] = {0}, name[28] = {0}, kind_str[4] = {0}, slave_str[8] = {0};
        char addr_str[8] = {0}, scale_str[16] = {0}, unit[10] = {0};

        httpd_query_key_value(buf, "idx", idx_str, sizeof(idx_str));
        httpd_query_key_value(buf, "name", name, sizeof(name));
        httpd_query_key_value(buf, "kind", kind_str, sizeof(kind_str));
        httpd_query_key_value(buf, "slave", slave_str, sizeof(slave_str));
        httpd_query_key_value(buf, "addr", addr_str, sizeof(addr_str));
        httpd_query_key_value(buf, "scale", scale_str, sizeof(scale_str));
        httpd_query_key_value(buf, "unit", unit, sizeof(unit));

        uint8_t idx = (uint8_t)atoi(idx_str);
        rs485_point_t pt = {0};
        pt.enabled = true;
        strlcpy(pt.name, name, sizeof(pt.name));
        strlcpy(pt.unit, unit, sizeof(pt.unit));
        pt.kind = (rs485_point_kind_t)atoi(kind_str);
        pt.slave_id = (uint8_t)atoi(slave_str);
        pt.reg_addr = (uint16_t)atoi(addr_str);
        pt.scale = (float)atof(scale_str);
        if (pt.scale == 0.0f) pt.scale = 1.0f;

        app_rs485_engine_set_point(idx, &pt);
        ESP_LOGI(TAG, "成功保存点位[%d]: %s", idx, pt.name);
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t rs485_del_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char idx_str[4] = {0};
        httpd_query_key_value(buf, "idx", idx_str, sizeof(idx_str));
        uint8_t idx = (uint8_t)atoi(idx_str);
        app_rs485_engine_delete_point(idx);
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t rs485_clear_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    app_rs485_engine_clear_all_points();
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* 万能 Modbus 调试发送 API */
static esp_err_t rs485_raw_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char s_str[8] = {0}, fc_str[8] = {0}, addr_str[8] = {0}, val_str[16] = {0};
        httpd_query_key_value(buf, "slave", s_str, sizeof(s_str));
        httpd_query_key_value(buf, "fc", fc_str, sizeof(fc_str));
        httpd_query_key_value(buf, "addr", addr_str, sizeof(addr_str));
        httpd_query_key_value(buf, "val", val_str, sizeof(val_str));

        modbus_raw_cmd_t cmd = {
            .slave_id = (uint8_t)atoi(s_str),
            .func_code = (uint8_t)atoi(fc_str),
            .reg_addr = (uint16_t)atoi(addr_str),
            .val_or_qty = (uint16_t)atoi(val_str),
        };
        modbus_raw_res_t res;
        app_rs485_engine_execute_raw(&cmd, &res);

        char tx_hex[96] = {0};
        char rx_hex[192] = {0};
        for (size_t i = 0; i < res.tx_len; i++) {
            snprintf(tx_hex + strlen(tx_hex), sizeof(tx_hex) - strlen(tx_hex), "%02X ", res.tx_buf[i]);
        }
        for (size_t i = 0; i < res.rx_len; i++) {
            snprintf(rx_hex + strlen(rx_hex), sizeof(rx_hex) - strlen(rx_hex), "%02X ", res.rx_buf[i]);
        }

        char resp_json[512];
        snprintf(resp_json, sizeof(resp_json),
                 "{\"success\":%s,\"tx\":\"%s\",\"rx\":\"%s\",\"err\":\"%s\"}",
                 res.success ? "true" : "false",
                 tx_hex, rx_hex, res.err_msg);

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, resp_json);
        return ESP_OK;
    }
    httpd_resp_send_500(req);
    return ESP_FAIL;
}

static esp_err_t wifi_info_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[128];
    char ip[16] = {0}, ssid[33] = {0};
    bool conn = app_wifi_get_ip_info(ip, sizeof(ip), ssid, sizeof(ssid));
    snprintf(buf, sizeof(buf), "{\"connected\":%s,\"ip\":\"%s\",\"ssid\":\"%s\"}",
             conn ? "true" : "false", ip, ssid);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    wifi_scan_ap_info_t ap_records[10];
    uint16_t count = app_wifi_scan(ap_records, 10);

    char *buf = malloc(2048);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int offset = snprintf(buf, 2048, "[");
    for (int i = 0; i < count; i++) {
        offset += snprintf(buf + offset, 2048 - offset,
                           "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                           (i > 0) ? "," : "",
                           ap_records[i].ssid, ap_records[i].rssi);
    }
    snprintf(buf + offset, 2048 - offset, "]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char ssid[33] = {0}, pwd[65] = {0};
        httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
        httpd_query_key_value(buf, "pwd", pwd, sizeof(pwd));
        app_wifi_connect_to(ssid, pwd);
        httpd_resp_sendstr(req, "已保存并正在连接 Wi-Fi，请稍后刷新状态...");
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid params");
    return ESP_FAIL;
}

static esp_err_t eth_info_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    eth_network_info_t eth_info;
    app_network_get_eth_info(&eth_info);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"link\":%s,\"mac\":\"%s\",\"dhcp\":%s,\"ip\":\"%s\",\"netmask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\"}",
             eth_info.link_up ? "true" : "false", eth_info.mac,
             eth_info.is_dhcp ? "true" : "false", eth_info.ip, eth_info.netmask, eth_info.gateway, eth_info.dns);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t eth_config_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[256];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char dhcp_str[4] = {0}, ip[16] = {0}, mask[16] = {0}, gw[16] = {0}, dns[16] = {0};
        httpd_query_key_value(buf, "dhcp", dhcp_str, sizeof(dhcp_str));
        httpd_query_key_value(buf, "ip", ip, sizeof(ip));
        httpd_query_key_value(buf, "mask", mask, sizeof(mask));
        httpd_query_key_value(buf, "gw", gw, sizeof(gw));
        httpd_query_key_value(buf, "dns", dns, sizeof(dns));

        bool dhcp = (atoi(dhcp_str) == 1);
        app_network_set_config(dhcp, ip, mask, gw, dns);
        httpd_resp_sendstr(req, "以太网配置已保存并生效！");
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid params");
    return ESP_FAIL;
}

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (update_partition == NULL) {
        ESP_LOGE(TAG, "未找到可用 OTA 分区！");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "开始 OTA 写入目标分区: %s (大小 %d 字节)", update_partition->label, req->content_len);
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA 初始化失败: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = malloc(1024);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = 0;
    int remaining = req->content_len;

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, (remaining > 1024) ? 1024 : remaining);
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "接收升级包异常");
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA 写入 Flash 失败: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        remaining -= recv_len;
        received += recv_len;
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA 结束校验失败: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "激活新固件失败: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA 固件升级成功！已切换启动分区至 %s，准备重启系统...", update_partition->label);
    httpd_resp_sendstr(req, "OK");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}


/* ---------------- 身份验证与登录 API ---------------- */
static esp_err_t login_handler(httpd_req_t *req)
{
    char user[32] = {0}, pwd[64] = {0};
    char body[128];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid login body");
    }

    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete login body");
        }
        received += count;
    }
    body[received] = '\0';

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    if (memchr(body, '\0', received) != NULL ||
        httpd_query_key_value(body, "user", user, sizeof(user)) != ESP_OK ||
        httpd_query_key_value(body, "pwd", pwd, sizeof(pwd)) != ESP_OK ||
        strcmp(user, CONFIG_IOT5_WEB_USERNAME) != 0 ||
        strcmp(pwd, CONFIG_IOT5_WEB_PASSWORD) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "{\"success\":false,\"msg\":\"账号或密码错误\"}");
    }

    create_session();
    char cookie[128];
    char response[96];
    snprintf(cookie, sizeof(cookie), AUTH_COOKIE_NAME "=%s; Max-Age=%d" AUTH_COOKIE_ATTRIBUTES,
             s_session_token, SESSION_MAX_AGE_SECONDS);
    snprintf(response, sizeof(response), "{\"success\":true,\"token\":\"%s\"}", s_session_token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return httpd_resp_sendstr(req, response);
}

static esp_err_t logout_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    clear_session();
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Set-Cookie", AUTH_COOKIE_NAME "=; Max-Age=0" AUTH_COOKIE_ATTRIBUTES);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"success\":true}");
}

static esp_err_t auth_status_handler(httpd_req_t *req)
{
    bool auth = is_request_authenticated(req);
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"authenticated\":%s,\"user\":\"%s\"}",
             auth ? "true" : "false", auth ? CONFIG_IOT5_WEB_USERNAME : "");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

/* ---------------- CAN 工业联动与 STM32 控制处理函数 ---------------- */
static esp_err_t can_status_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    can_engine_status_t status;
    app_can_engine_get_status(&status);

    char *logs_buf = malloc(2048);
    if (!logs_buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    app_can_engine_get_logs_json(logs_buf, 2048);

    char *resp = malloc(2560);
    if (!resp) {
        free(logs_buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    snprintf(resp, 2560,
        "{\"running\":%s,\"tx_count\":%lu,\"rx_count\":%lu,"
        "\"stm32_d0\":%s,\"key0\":%s,\"key0_time\":\"%s\",\"last_rx\":\"%s\",\"logs\":%s}",
        status.is_running ? "true" : "false",
        (unsigned long)status.tx_count,
        (unsigned long)status.rx_count,
        status.stm32_d0_state ? "true" : "false",
        status.stm32_key0_triggered ? "true" : "false",
        status.last_key0_time,
        status.last_rx_desc,
        logs_buf
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);

    free(logs_buf);
    free(resp);
    return ESP_OK;
}

static esp_err_t can_stm32_led_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[64] = {0};
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char state_str[8] = {0};
        httpd_query_key_value(buf, "state", state_str, sizeof(state_str));
        bool on = (atoi(state_str) == 1);
        esp_err_t err = app_can_engine_control_stm32_d0(on);
        if (err == ESP_OK) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"success\":true}");
            return ESP_OK;
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"err\":\"CAN发送失败，请检查总线连接\"}");
    return ESP_OK;
}

static esp_err_t can_send_raw_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char buf[128] = {0};
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char id_str[16] = {0}, data_str[32] = {0}, ext_str[8] = {0};
        httpd_query_key_value(buf, "id", id_str, sizeof(id_str));
        httpd_query_key_value(buf, "data", data_str, sizeof(data_str));
        httpd_query_key_value(buf, "ext", ext_str, sizeof(ext_str));

        uint32_t id = (uint32_t)strtoul(id_str, NULL, 0);
        bool is_ext = (atoi(ext_str) == 1);

        uint8_t data_bytes[8] = {0};
        uint8_t len = 0;
        char *p = data_str;
        while (*p && len < 8) {
            while (*p == ' ' || *p == ',' || *p == '-') p++;
            if (!*p) break;
            char hex_byte[3] = {p[0], p[1] ? p[1] : '\0', '\0'};
            data_bytes[len++] = (uint8_t)strtol(hex_byte, NULL, 16);
            if (p[1]) p += 2;
            else p += 1;
        }

        esp_err_t err = app_can_engine_send(id, data_bytes, len, is_ext, "网页调试报文");
        if (err == ESP_OK) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"success\":true}");
            return ESP_OK;
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"err\":\"CAN发送失败\"}");
    return ESP_OK;
}

static esp_err_t can_clear_logs_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    app_can_engine_clear_logs();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

esp_err_t app_webserver_start(uint16_t port)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    if (!credentials_configured()) {
        ESP_LOGW(TAG, "在 menuconfig 的 IOT5 Web console 中配置有效凭据后启用 Web 服务");
        return ESP_ERR_INVALID_STATE;
    }
    clear_session();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 36; // 支持充足路由
    config.stack_size = 10240;

    ESP_LOGI(TAG, "正在启动 Web 服务器，端口: %d ...", port);
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "启动失败");

    httpd_uri_t uris[] = {
        /* 鉴权认证路由 */
        { .uri = "/api/login",         .method = HTTP_POST, .handler = login_handler },
        { .uri = "/api/logout",        .method = HTTP_POST, .handler = logout_handler },
        { .uri = "/api/auth/status",   .method = HTTP_GET,  .handler = auth_status_handler },

        /* CAN 联动路由 */
        { .uri = "/api/can/status",    .method = HTTP_GET,  .handler = can_status_handler },
        { .uri = "/api/can/stm32/led", .method = HTTP_POST, .handler = can_stm32_led_handler },
        { .uri = "/api/can/send",      .method = HTTP_POST, .handler = can_send_raw_handler },
        { .uri = "/api/can/clear",     .method = HTTP_POST, .handler = can_clear_logs_handler },

        { .uri = "/",                 .method = HTTP_GET,  .handler = index_get_handler },
        { .uri = "/api/do",            .method = HTTP_GET,  .handler = do_ctrl_handler },
        { .uri = "/api/di",            .method = HTTP_GET,  .handler = di_get_handler },
        { .uri = "/api/rs485/points",  .method = HTTP_GET,  .handler = rs485_points_handler },
        { .uri = "/api/rs485/ctrl/switch",.method = HTTP_POST, .handler = rs485_ctrl_switch_handler },
        { .uri = "/api/rs485/ctrl/all",   .method = HTTP_POST, .handler = rs485_ctrl_all_handler },
        { .uri = "/api/rs485/ctrl/write", .method = HTTP_POST, .handler = rs485_ctrl_write_handler },
        { .uri = "/api/rs485/template",   .method = HTTP_POST, .handler = rs485_template_handler },
        { .uri = "/api/rs485/poll",       .method = HTTP_GET,  .handler = rs485_poll_get_handler },
        { .uri = "/api/rs485/poll",       .method = HTTP_POST, .handler = rs485_poll_post_handler },
        { .uri = "/api/rs485/set",        .method = HTTP_POST, .handler = rs485_set_handler },
        { .uri = "/api/rs485/del",        .method = HTTP_POST, .handler = rs485_del_handler },
        { .uri = "/api/rs485/clear",      .method = HTTP_POST, .handler = rs485_clear_handler },
        { .uri = "/api/rs485/raw",        .method = HTTP_POST, .handler = rs485_raw_handler },
        { .uri = "/api/wifi/info",        .method = HTTP_GET,  .handler = wifi_info_handler },
        { .uri = "/api/wifi/scan",        .method = HTTP_GET,  .handler = wifi_scan_handler },
        { .uri = "/api/wifi/connect",     .method = HTTP_POST, .handler = wifi_connect_handler },
        { .uri = "/api/eth/info",         .method = HTTP_GET,  .handler = eth_info_handler },
        { .uri = "/api/eth/config",       .method = HTTP_POST, .handler = eth_config_handler },
        { .uri = "/ota/upload",           .method = HTTP_POST, .handler = ota_upload_handler },
    };

    for (int i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI(TAG, "Web 服务器就绪！访问 http://IP:%d/", port);
    return ESP_OK;
}

void app_webserver_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        clear_session();
    }
}
