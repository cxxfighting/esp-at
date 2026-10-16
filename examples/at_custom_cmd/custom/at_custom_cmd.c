/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_at.h"


#define BLE_NTFY_MAX_PAYLOAD  100   /* 受BLE MTU限制，实际不要超过MTU‑3 */

/* ========== 调用esp‑at内部BLE GATTS发送通知接口 ==========
 * 参考esp‑at/components/at/src/at_ble_gatts.c 中原生AT+BLEGATTSNTFY实现
 * esp‑at内部封装：at_ble_gatts_send_notify_indicate()
 * 原型：
 * esp_err_t at_ble_gatts_send_notify_indicate(uint8_t conn_id,
 *                                             uint16_t attr_handle,
 *                                             uint8_t *value,
 *                                             uint16_t value_len,
 *                                             bool is_indicate);
 * is_indicate=false → NTFY; true → IND
 */
extern esp_err_t at_ble_gatts_send_notify_indicate(uint8_t conn_id,
                                                   uint16_t attr_handle,
                                                   uint8_t *value,
                                                   uint16_t value_len,
                                                   bool is_indicate);

/* 注意：srv_index + char_index 需要转换成实际ATT attr_handle！
 * 原生at_ble_gatts命令内部会做转换 at_ble_gatts_get_attr_handle_by_index()
 */
extern esp_err_t at_ble_gatts_get_attr_handle_by_index(uint8_t srv_idx,
                                                        uint8_t char_idx,
                                                        uint16_t *out_handle);

static uint8_t at_setup_cmd_ble_gatts_ntfy_hex(uint8_t para_num)
{
    uint8_t result = ESP_AT_RESULT_CODE_ERROR;
    int32_t tmp = 0;
    uint8_t conn_index = 0;
    uint8_t srv_index  = 0;
    uint8_t char_index = 0;
    uint16_t data_len  = 0;
    uint8_t *hex_buf = NULL;
    int idx = 0;

    uint8_t raw_data[BLE_NTFY_MAX_PAYLOAD] = {0};

    /* 解析第1参数 conn_index */
    if (esp_at_get_para_as_digit(idx++, &tmp) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    conn_index = (uint8_t)tmp;

    /* 解析第2参数 srv_index */
    if (esp_at_get_para_as_digit(idx++, &tmp) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    srv_index = (uint8_t)tmp;

    /* 解析第3参数 char_index */
    if (esp_at_get_para_as_digit(idx++, &tmp) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    char_index = (uint8_t)tmp;

    /* 解析第4参数 length */
    if (esp_at_get_para_as_digit(idx++, &tmp) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    data_len = (uint16_t)tmp;

    /* 解析第5参数 hex字符串 */
    if (esp_at_get_para_as_str(idx++, &hex_buf) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    /* 参数数量校验 */
    if (idx != para_num) {
        esp_at_port_write_data((uint8_t *)"ERROR_PARA\r\n", 12);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    /* 长度合法性校验 */
    if ((data_len == 0) || (data_len > BLE_NTFY_MAX_PAYLOAD)) {
        esp_at_port_write_data((uint8_t *)"ERROR_LEN\r\n", 11);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    /* hex字符串长度必须等于 2 * data_len */
    if (strlen((char *)hex_buf) != (size_t)(data_len * 2U)) {
        esp_at_port_write_data((uint8_t *)"ERROR_HEX\r\n", 12);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    /* hex字符串转原始二进制，和udp_send逻辑完全一致 */
    for (int i = 0; i < data_len; i++) {
        char tmpbuf[3] = {0};
        tmpbuf[0] = hex_buf[i * 2];
        tmpbuf[1] = hex_buf[i * 2 + 1];
        raw_data[i] = (uint8_t)strtol(tmpbuf, NULL, 16);
    }
    uint16_t attr_handle = 0;
    esp_err_t esp_ret = at_ble_gatts_get_attr_handle_by_index(srv_index, char_index, &attr_handle);
    if (esp_ret != ESP_OK) {
        esp_at_port_write_data((uint8_t *)"ERROR_HANDLE\r\n",14);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    esp_ret = at_ble_gatts_send_notify_indicate(conn_index, attr_handle, raw_data, data_len, false);
    if (esp_ret == ESP_OK) {
        esp_at_port_write_data((uint8_t *)"OK\r\n",4);
        result = ESP_AT_RESULT_CODE_PROCESS_DONE;
    } else {
        esp_at_port_write_data((uint8_t *)"ERROR_SEND\r\n",13);
        result = ESP_AT_RESULT_CODE_PROCESS_DONE;
    }

    return result;
}
/****************************************************************************************************************************************/
static uint8_t at_test_cmd_test(uint8_t *cmd_name)
{
    uint8_t buffer[64] = {0};
    snprintf((char *)buffer, 64, "test command: <AT%s=?> is executed\r\n", cmd_name);
    esp_at_port_write_data(buffer, strlen((char *)buffer));

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_query_cmd_test(uint8_t *cmd_name)
{
    uint8_t buffer[64] = {0};
    snprintf((char *)buffer, 64, "query command: <AT%s?> is executed\r\n", cmd_name);
    esp_at_port_write_data(buffer, strlen((char *)buffer));

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_test(uint8_t para_num)
{
    uint8_t index = 0;

    // get first parameter, and parse it into a digit
    int32_t digit = 0;
    if (esp_at_get_para_as_digit(index++, &digit) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // get second parameter, and parse it into a string
    uint8_t *str = NULL;
    if (esp_at_get_para_as_str(index++, &str) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // allocate a buffer and construct the data, then send the data to mcu via interface (uart/spi/sdio/socket)
    uint8_t *buffer = (uint8_t *)malloc(512);
    if (!buffer) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    int len = snprintf((char *)buffer, 512, "setup command: <AT%s=%d,\"%s\"> is executed\r\n",
                       esp_at_get_current_cmd_name(), digit, str);
    esp_at_port_write_data(buffer, len);

    // remember to free the buffer
    free(buffer);

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_exe_cmd_test(uint8_t *cmd_name)
{
    uint8_t buffer[64] = {0};
    snprintf((char *)buffer, 64, "execute command: <AT%s> is executed\r\n", cmd_name);
    esp_at_port_write_data(buffer, strlen((char *)buffer));

    return ESP_AT_RESULT_CODE_OK;
}

static const esp_at_cmd_t at_custom_cmd[] = {
    {"+BLEGATTSNTFYHEX", NULL, NULL, at_setup_cmd_ble_gatts_ntfy_hex, NULL},
    {"+TEST", at_test_cmd_test, at_query_cmd_test, at_setup_cmd_test, at_exe_cmd_test},
    /**
     * @brief You can define your own AT commands here.
     */
};

bool esp_at_custom_cmd_register(void)
{
    return esp_at_custom_cmd_array_register(at_custom_cmd, sizeof(at_custom_cmd) / sizeof(esp_at_cmd_t));
}

ESP_AT_CMD_SET_INIT_FN(esp_at_custom_cmd_register, 1);
