
/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_vfs_fat.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_at.h"

#define AT_NETWORK_TIMEOUT_MS       (10000)
#define AT_URL_LEN_MAX              (8 * 1024)
#define AT_HEAP_BUFFER_SIZE         4096
#define AT_RESP_PREFIX_LEN_MAX      64
#define AT_FATFS_MOUNT_POINT        "/fatfs"
extern esp_err_t esp_at_http_set_header_if_config(esp_http_client_handle_t client);

typedef struct {
    bool fs_mounted;                /*!< File system mounted */
    char *path;                     /*!< File path */
    FILE *fp;                       /*!< File pointer */
    uint32_t total_size;            /*!< The total size of the file system */
    uint32_t had_read_size;         /*!< The file size that has been written to flash */
} at_read_fs_handle_t;

typedef struct {
    char *url;                      /*!< URL */
    int32_t post_size;              /*!< Total size of the file to post */
    SemaphoreHandle_t sync_sema;    /*!< Semaphore for synchronization */
    esp_http_client_handle_t client;    /*!< HTTP client handle */
    at_read_fs_handle_t *fs_handle;     /*!< File system handle */
} at_fs_to_http_server_t;


typedef struct {
    bool fs_mounted;                /*!< File system mounted */
    char *path;                     /*!< File path */
    FILE *fp;                       /*!< File pointer */
    uint32_t available_size;        /*!< The available size of the file system */
    uint32_t total_size;            /*!< The total size of the file system */
    uint32_t wrote_size;            /*!< The file size that has been written to flash */
} at_write_fs_handle_t;

typedef struct {
    char *url;                      /*!< URL */
    int32_t total_size;             /*!< Total size of the file */
    int32_t recv_size;              /*!< Received size of the file */
    bool is_chunked;                /*!< Chunked flag */
    SemaphoreHandle_t sync_sema;    /*!< Semaphore for synchronization */
    esp_http_client_handle_t client;    /*!< HTTP client handle */
    at_write_fs_handle_t *fs_handle;    /*!< File system handle */
} at_httpget_to_fs_t;

// static variables
static at_httpget_to_fs_t *sp_http_to_fs;
static const char *TAG = "at_http_to_fs";

// static variables
static at_fs_to_http_server_t *sp_fs_to_http;
static const char *TAG_POST = "at_fs_to_http";
/***************************************************************************************************************************************/
at_write_fs_handle_t *at_http_to_fs_begin(char *path)
{
    at_write_fs_handle_t *fs_handle = (at_write_fs_handle_t *)calloc(1, sizeof(at_write_fs_handle_t));
    if (!fs_handle) {
        ESP_LOGE(TAG, "calloc failed");
        return NULL;
    }

    // mount file system
    if (!at_fatfs_mount()) {
        free(fs_handle);
        ESP_LOGE(TAG, "at_fatfs_mount failed");
        return NULL;
    }
    fs_handle->fs_mounted = true;

    // get available size
    uint64_t fs_total_size = 0, fs_free_size = 0;
    if (esp_vfs_fat_info(AT_FATFS_MOUNT_POINT, &fs_total_size, &fs_free_size) != ESP_OK) {
        free(fs_handle);
        at_fatfs_unmount();
        ESP_LOGE(TAG, "esp_vfs_fat_info failed");
        return NULL;
    }
    fs_handle->total_size = fs_total_size;
    fs_handle->available_size = fs_free_size;
    printf("fatfs available size:%u, total size:%u\r\n", fs_handle->available_size, fs_handle->total_size);

    // init path
    fs_handle->path = (char *)calloc(1, strlen(AT_FATFS_MOUNT_POINT) + strlen(path) + 2);
    if (!fs_handle->path) {
        free(fs_handle);
        at_fatfs_unmount();
        ESP_LOGE(TAG, "calloc failed");
        return NULL;
    }
    sprintf(fs_handle->path, "%s/%s", AT_FATFS_MOUNT_POINT, path);

    // open file
    remove(fs_handle->path);
    fs_handle->fp = fopen(fs_handle->path, "wb");
    if (!fs_handle->fp) {
        free(fs_handle);
        at_fatfs_unmount();
        ESP_LOGE(TAG, "fopen failed");
        return NULL;
    }

    return fs_handle;
}

esp_err_t at_http_to_fs_write(at_write_fs_handle_t *fs_handle, uint8_t *data, size_t len)
{
    if (!fs_handle || !fs_handle->fp) {
        ESP_LOGE(TAG, "invalid argument");
        return ESP_ERR_INVALID_ARG;
    }

    if (fseek(fs_handle->fp, fs_handle->wrote_size, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek failed");
        return ESP_FAIL;
    }

    size_t wrote_len = fwrite(data, 1, len, fs_handle->fp);
    if (wrote_len != len) {
        ESP_LOGE(TAG, "fwrite failed, to write len=%d, wrote len=%d", len, wrote_len);
        return ESP_FAIL;
    }

    fs_handle->wrote_size += len;
    return ESP_OK;
}

static void at_http_to_fs_clean(void)
{
    if (sp_http_to_fs) {
        // http client
        if (sp_http_to_fs->sync_sema) {
            vSemaphoreDelete(sp_http_to_fs->sync_sema);
            sp_http_to_fs->sync_sema = NULL;
        }
        if (sp_http_to_fs->url) {
            free(sp_http_to_fs->url);
            sp_http_to_fs->url = NULL;
        }
        if (sp_http_to_fs->client) {
            esp_http_client_cleanup(sp_http_to_fs->client);
            sp_http_to_fs->client = NULL;
        }

        // file system
        if (sp_http_to_fs->fs_handle) {
            if (sp_http_to_fs->fs_handle->fp) {
                fclose(sp_http_to_fs->fs_handle->fp);
                sp_http_to_fs->fs_handle->fp = NULL;
            }
            if (sp_http_to_fs->fs_handle->path) {
                free(sp_http_to_fs->fs_handle->path);
                sp_http_to_fs->fs_handle->path = NULL;
            }
            if (sp_http_to_fs->fs_handle->fs_mounted) {
                at_fatfs_unmount();
                sp_http_to_fs->fs_handle->fs_mounted = false;
            }
            free(sp_http_to_fs->fs_handle);
            sp_http_to_fs->fs_handle = NULL;
        }

        // itself
        free(sp_http_to_fs);
        sp_http_to_fs = NULL;
    }
}

static void at_sp_http_to_fs_wait_data_cb(void)
{
    xSemaphoreGive(sp_http_to_fs->sync_sema);
}

static esp_err_t at_http_get_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        printf("http(https) error\r\n");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        printf("http(https) connected\r\n");
        break;
    case HTTP_EVENT_HEADER_SENT:
        printf("http(https) header sent\r\n");
        break;
    case HTTP_EVENT_ON_HEADER:
        printf("http(https) headed key=%s, value=%s\r\n", evt->header_key, evt->header_value);
        if (strcmp(evt->header_key, "Content-Length") == 0) {
            sp_http_to_fs->total_size = atoi(evt->header_value);
            printf("total_size=%d\r\n", sp_http_to_fs->total_size);
            sp_http_to_fs->is_chunked = false;
        }
        break;
    case HTTP_EVENT_ON_DATA:
        sp_http_to_fs->recv_size += evt->data_len;

        // chunked check
        if (sp_http_to_fs->is_chunked) {
            printf("received total len=%d\r\n", sp_http_to_fs->recv_size);
        } else {
            printf("total_len=%d(%d), %0.1f%%!\r\n", sp_http_to_fs->total_size,
                   sp_http_to_fs->recv_size, (sp_http_to_fs->recv_size * 1.0) * 100 / sp_http_to_fs->total_size);
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        printf("http(https) finished\r\n");
        break;
    case HTTP_EVENT_DISCONNECTED:
        printf("http(https) disconnected\r\n");
        break;
    default:
        break;
    }

    return ESP_OK;
}

static uint8_t at_setup_cmd_httpget_to_fs(uint8_t para_num)
{
    esp_err_t ret = ESP_OK;
    int32_t cnt = 0, url_len = 0;
    uint8_t *dst_path = NULL;

    // dst file path
    if (esp_at_get_para_as_str(cnt++, &dst_path) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    if (at_str_is_null(dst_path)) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // url len
    if (esp_at_get_para_as_digit(cnt++, &url_len) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    if (url_len <= 0 || url_len > AT_URL_LEN_MAX) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // parameters are ready
    if (cnt != para_num) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    sp_http_to_fs = (at_httpget_to_fs_t *)calloc(1, sizeof(at_httpget_to_fs_t));
    if (!sp_http_to_fs) {
        ret = ESP_ERR_NO_MEM;
        goto cmd_exit;
    }

    // init resources
    sp_http_to_fs->fs_handle = at_http_to_fs_begin((char *)dst_path);
    sp_http_to_fs->url = (char *)calloc(1, url_len + 1);
    sp_http_to_fs->sync_sema = xSemaphoreCreateBinary();

    if (!sp_http_to_fs->fs_handle || !sp_http_to_fs->url || !sp_http_to_fs->sync_sema) {
        ret = ESP_ERR_NO_MEM;
        goto cmd_exit;
    }

    // receive url from AT port
    int32_t had_recv_len = 0;
    esp_at_port_enter_specific(at_sp_http_to_fs_wait_data_cb);
    esp_at_response_result(ESP_AT_RESULT_CODE_OK_AND_INPUT_PROMPT);
    while (xSemaphoreTake(sp_http_to_fs->sync_sema, portMAX_DELAY)) {
        had_recv_len += esp_at_port_read_data((uint8_t *)(sp_http_to_fs->url) + had_recv_len, url_len - had_recv_len);
        if (had_recv_len == url_len) {
            printf("Recv %d bytes\r\n", had_recv_len);
            esp_at_port_exit_specific();

            int32_t remain_len = esp_at_port_get_data_length();
            if (remain_len > 0) {
                esp_at_port_recv_data_notify(remain_len, portMAX_DELAY);
            }
            break;
        }
    }
    printf("ready to download %s to %s\r\n", sp_http_to_fs->url, sp_http_to_fs->fs_handle->path);

    // init http client
    esp_http_client_config_t config = {
        .url = (const char*)sp_http_to_fs->url,
        .event_handler = at_http_get_event_handler,
        .timeout_ms = AT_NETWORK_TIMEOUT_MS,
        .buffer_size_tx = 4096,
    };
    sp_http_to_fs->is_chunked = true;
    sp_http_to_fs->client = esp_http_client_init(&config);
    if (!sp_http_to_fs->client) {
        ret = ESP_FAIL;
        goto cmd_exit;
    }
    esp_http_client_set_method(sp_http_to_fs->client, HTTP_METHOD_GET);

    // establish http connection
    ret = esp_http_client_open(sp_http_to_fs->client, 0);
    if (ret != ESP_OK) {
        goto cmd_exit;
    }
    esp_http_client_fetch_headers(sp_http_to_fs->client);
    int status_code = esp_http_client_get_status_code(sp_http_to_fs->client);
    if (status_code >= HttpStatus_BadRequest) {
        ESP_LOGE(TAG, "recv http status code: %d", status_code);
        ret = ESP_FAIL;
        goto cmd_exit;
    }
    if (sp_http_to_fs->fs_handle->available_size < sp_http_to_fs->total_size) {
        ESP_LOGE(TAG, "fatfs available size:%u, but res total size:%d", sp_http_to_fs->fs_handle->available_size, sp_http_to_fs->total_size);
        ret = ESP_FAIL;
        goto cmd_exit;
    }

    // download data to file
    int data_len = 0;
    uint8_t *data = (uint8_t *)malloc(AT_HEAP_BUFFER_SIZE);
    if (!data) {
        ret = ESP_ERR_NO_MEM;
        goto cmd_exit;
    }
    do {
        data_len = esp_http_client_read(sp_http_to_fs->client, (char *)data, AT_HEAP_BUFFER_SIZE);
        if (data_len > 0) {
            ret = at_http_to_fs_write(sp_http_to_fs->fs_handle, data, data_len);
            if (ret != ESP_OK) {
                break;
            }
        } else if (data_len < 0) {
            ESP_LOGE(TAG, "Connection aborted!");
            break;
        } else {
            printf("Connection closed\r\n");
            ret = ESP_OK;
            break;
        }
    } while (ret == ESP_OK && data_len > 0);
    free(data);

    if (sp_http_to_fs->is_chunked) {
        printf("total received len:%d, total wrote size:%d\r\n", sp_http_to_fs->recv_size, sp_http_to_fs->fs_handle->wrote_size);
    } else {
        if (sp_http_to_fs->total_size != sp_http_to_fs->fs_handle->wrote_size) {
            ESP_LOGE(TAG, "total expected len:%d, but total wrote size:%d", sp_http_to_fs->total_size, sp_http_to_fs->fs_handle->wrote_size);
            ret = ESP_FAIL;
        } else {
            printf("total wrote size matches expected size:%d\r\n", sp_http_to_fs->fs_handle->wrote_size);
        }
    }

cmd_exit:
    // clean resources
    at_http_to_fs_clean();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "command ret: 0x%x", ret);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    return ESP_AT_RESULT_CODE_OK;
}

/* ****************************************************************************************************************************** */

static at_read_fs_handle_t *at_fs_to_http_begin(char *path)
{
    // mount file system
    if (!at_fatfs_mount()) {
        ESP_LOGE(TAG_POST, "at_fatfs_mount failed");
        return NULL;
    }

    // init handle
    at_read_fs_handle_t *fs_handle = (at_read_fs_handle_t *)calloc(1, sizeof(at_read_fs_handle_t));
    if (!fs_handle) {
        ESP_LOGE(TAG_POST, "calloc failed");
        return NULL;
    }
    fs_handle->fs_mounted = true;

    // init path
    fs_handle->path = (char *)calloc(1, strlen(AT_FATFS_MOUNT_POINT) + strlen(path) + 2);
    if (!fs_handle->path) {
        free(fs_handle);
        at_fatfs_unmount();
        ESP_LOGE(TAG_POST, "calloc failed");
        return NULL;
    }
    sprintf(fs_handle->path, "%s/%s", AT_FATFS_MOUNT_POINT, path);

    // get file size
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (stat(fs_handle->path, &st) == -1) {
        ESP_LOGE(TAG_POST, "stat(%s) failed\n", fs_handle->path);
        free(fs_handle->path);
        free(fs_handle);
        at_fatfs_unmount();
        return NULL;
    }
    fs_handle->total_size = st.st_size;

    // open file
    fs_handle->fp = fopen(fs_handle->path, "rb");
    if (!fs_handle->fp) {
        ESP_LOGE(TAG_POST, "fopen(%s) failed", fs_handle->path);
        free(fs_handle->path);
        free(fs_handle);
        at_fatfs_unmount();
        return NULL;
    }

    return fs_handle;
}

static int at_fs_read(at_read_fs_handle_t *fs_handle, uint8_t *data, size_t len)
{
    if (!fs_handle || !fs_handle->fp || !data) {
        ESP_LOGE(TAG_POST, "invalid argument");
        return -ESP_ERR_INVALID_ARG;
    }

    if (fseek(fs_handle->fp, fs_handle->had_read_size, SEEK_SET) != 0) {
        ESP_LOGE(TAG_POST, "fseek failed");
        return ESP_FAIL;
    }

    size_t had_read_size = fread(data, 1, len, fs_handle->fp);
    fs_handle->had_read_size += had_read_size;
    return had_read_size;
}

static void at_fs_to_http_clean(void)
{
    if (sp_fs_to_http) {
        // http client
        if (sp_fs_to_http->sync_sema) {
            vSemaphoreDelete(sp_fs_to_http->sync_sema);
            sp_fs_to_http->sync_sema = NULL;
        }
        if (sp_fs_to_http->url) {
            free(sp_fs_to_http->url);
            sp_fs_to_http->url = NULL;
        }
        if (sp_fs_to_http->client) {
            esp_http_client_cleanup(sp_fs_to_http->client);
            sp_fs_to_http->client = NULL;
        }

        // file system
        if (sp_fs_to_http->fs_handle) {
            if (sp_fs_to_http->fs_handle->fp) {
                fclose(sp_fs_to_http->fs_handle->fp);
                sp_fs_to_http->fs_handle->fp = NULL;
            }
            if (sp_fs_to_http->fs_handle->path) {
                free(sp_fs_to_http->fs_handle->path);
                sp_fs_to_http->fs_handle->path = NULL;
            }
            if (sp_fs_to_http->fs_handle->fs_mounted) {
                at_fatfs_unmount();
                sp_fs_to_http->fs_handle->fs_mounted = false;
            }
            free(sp_fs_to_http->fs_handle);
            sp_fs_to_http->fs_handle = NULL;
        }

        // itself
        free(sp_fs_to_http);
        sp_fs_to_http = NULL;
    }
}

static void at_custom_wait_data_cb(void)
{
    xSemaphoreGive(sp_fs_to_http->sync_sema);
}

static esp_err_t at_http_event_handler(esp_http_client_event_t *evt)
{
    int header_len = 0;
    uint8_t *data = NULL;
    ESP_LOGI(TAG_POST, "http event id=%d", evt->event_id);

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        data = malloc(evt->data_len + AT_RESP_PREFIX_LEN_MAX);
        if (!data) {
            ESP_LOGE(TAG_POST, "malloc failed");
            return ESP_ERR_NO_MEM;
        }
        header_len = snprintf((char *)data, AT_RESP_PREFIX_LEN_MAX, "%s:%d,", esp_at_get_current_cmd_name(), evt->data_len);
        memcpy(data + header_len, evt->data, evt->data_len);
        memcpy(data + header_len + evt->data_len, "\r\n", 2);
        esp_at_port_write_data(data, header_len + evt->data_len + 2);
        free(data);
        printf("\r\n%.*s\r\n", evt->data_len, (char *)evt->data);
        break;
    default:
        break;
    }

    return ESP_OK;
}

static uint8_t at_setup_cmd_fs_to_http_server(uint8_t para_num)
{
    esp_err_t ret = ESP_OK;
    int32_t cnt = 0, url_len = 0;
    uint8_t *dst_path = NULL, *data = NULL;
    char *body_start = NULL, *body_end = NULL;

    // dst file path
    if (esp_at_get_para_as_str(cnt++, &dst_path) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    if (at_str_is_null(dst_path)) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // url len
    if (esp_at_get_para_as_digit(cnt++, &url_len) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    if (url_len <= 0 || url_len > AT_URL_LEN_MAX) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // parameters are ready
    if (cnt != para_num) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    sp_fs_to_http = (at_fs_to_http_server_t *)calloc(1, sizeof(at_fs_to_http_server_t));
    if (!sp_fs_to_http) {
        ret = ESP_ERR_NO_MEM;
        goto cmd_exit;
    }

    // init resources
    sp_fs_to_http->fs_handle = at_fs_to_http_begin((char *)dst_path);
    sp_fs_to_http->url = (char *)calloc(1, url_len + 1);
    sp_fs_to_http->sync_sema = xSemaphoreCreateBinary();

    if (!sp_fs_to_http->fs_handle || !sp_fs_to_http->url || !sp_fs_to_http->sync_sema) {
        ret = ESP_ERR_NO_MEM;
        goto cmd_exit;
    }

    // receive url from AT port
    int32_t had_recv_len = 0;
    esp_at_port_enter_specific(at_custom_wait_data_cb);
    esp_at_response_result(ESP_AT_RESULT_CODE_OK_AND_INPUT_PROMPT);
    while (xSemaphoreTake(sp_fs_to_http->sync_sema, portMAX_DELAY)) {
        had_recv_len += esp_at_port_read_data((uint8_t *)(sp_fs_to_http->url) + had_recv_len, url_len - had_recv_len);
        if (had_recv_len == url_len) {
            printf("Recv %d bytes\r\n", had_recv_len);
            esp_at_port_exit_specific();

            int32_t remain_len = esp_at_port_get_data_length();
            if (remain_len > 0) {
                esp_at_port_recv_data_notify(remain_len, portMAX_DELAY);
            }
            break;
        }
    }
    printf("ready to post %s (size:%d) to %s\r\n", sp_fs_to_http->fs_handle->path, sp_fs_to_http->fs_handle->total_size, sp_fs_to_http->url);

    // init http client
    esp_http_client_config_t config = {
        .url = (const char*)sp_fs_to_http->url,
        .event_handler = at_http_event_handler,
        .timeout_ms = AT_NETWORK_TIMEOUT_MS,
        .buffer_size_tx = 4096,
    };
    sp_fs_to_http->client = esp_http_client_init(&config);
    if (!sp_fs_to_http->client) {
        ret = ESP_FAIL;
        goto cmd_exit;
    }
    esp_http_client_set_method(sp_fs_to_http->client, HTTP_METHOD_POST);
    // esp_http_client_set_header(sp_fs_to_http->client, "Content-Type", "multipart/form-data");

    // set new header
    const char *boundary = "--myboundary";
    char value[128];
    snprintf(value, 128, "multipart/form-data; boundary=--%s", boundary);
    //printf("esp_http_client_set_header ready\n");
    esp_http_client_set_header(sp_fs_to_http->client, "Content-Type", value);
    //printf("esp_http_client_set_header end\n");
    //esp_http_client_set_header(sp_fs_to_http->client, "recordId", "1");
    //printf("esp_at_http_set_header_if_config ready\n");
    esp_at_http_set_header_if_config(sp_fs_to_http->client);
    //printf("esp_at_http_set_header_if_config end\n");
    // construct http body start and end
    int rlen = 0;
    body_start = calloc(1, 512);
    body_end = calloc(1, 64);
    if (!body_start || !body_end) {
        ret = ESP_ERR_NO_MEM;
        goto cmd_exit;
    }
    int start_len = snprintf(body_start, 512,
        "----%s\r\nContent-Disposition: form-data; name=\"username\"\r\n\r\nAlice\r\n----%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\nContent-Type: application/octet-stream\r\n\r\n",
         boundary, boundary, sp_fs_to_http->fs_handle->path);
    int end_len = snprintf(body_end, 512, "\r\n----%s--\r\n", boundary);

    // establish http connection
    ret = esp_http_client_open(sp_fs_to_http->client, sp_fs_to_http->fs_handle->total_size + start_len + end_len);
    if (ret != ESP_OK) {
        goto cmd_exit;
    }

    rlen = esp_http_client_write(sp_fs_to_http->client, body_start, start_len);
    if (rlen != start_len) {
        ESP_LOGE(TAG_POST, "esp_http_client_write() failed");
        ret = ESP_FAIL;
        goto cmd_exit;
    }

    // post file to remote server
    data = (uint8_t *)malloc(AT_HEAP_BUFFER_SIZE);
    if (!data) {
        ret = ESP_ERR_NO_MEM;
        goto cmd_exit;
    }
    do {
        int unposted_len = sp_fs_to_http->fs_handle->total_size - sp_fs_to_http->fs_handle->had_read_size;
        int to_post_len = AT_HEAP_BUFFER_SIZE < unposted_len ? AT_HEAP_BUFFER_SIZE : unposted_len;
        to_post_len = at_fs_read(sp_fs_to_http->fs_handle, data, to_post_len);
        if (to_post_len <= 0) {
            ret = ESP_FAIL;
            break;
        }
        int wlen = esp_http_client_write(sp_fs_to_http->client, (char *)data, to_post_len);
        if (wlen > 0) {
            sp_fs_to_http->post_size += wlen;
            if (sp_fs_to_http->post_size == sp_fs_to_http->fs_handle->total_size) {
                ret = ESP_OK;
                break;
            }
        } else if (wlen < 0) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG_POST, "Connection aborted!");
            break;
        } else {
            ret = ESP_FAIL;
            ESP_LOGE(TAG_POST, "esp_http_client_write() timeout!");
            break;
        }
    } while (1);

    rlen = esp_http_client_write(sp_fs_to_http->client, body_end, end_len);
    if (rlen != end_len) {
        ESP_LOGE(TAG_POST, "esp_http_client_write() failed");
        ret = ESP_FAIL;
        goto cmd_exit;
    }

    // post over
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_POST, "total expected len:%d, but total post size:%d", sp_fs_to_http->fs_handle->total_size, sp_fs_to_http->post_size);
        goto cmd_exit;
    }
    ESP_LOGI(TAG_POST, "total post size matches expected size:%d", sp_fs_to_http->fs_handle->total_size);

    // fetch response header
    int header_ret = esp_http_client_fetch_headers(sp_fs_to_http->client);
    if (header_ret < 0) {
        ret = header_ret;
        goto cmd_exit;
    }
    // status code
    int status_code = esp_http_client_get_status_code(sp_fs_to_http->client);
    if (status_code != HttpStatus_Ok) {
        ESP_LOGE(TAG_POST, "recv http status code: %d", status_code);
        ret = -status_code;
        goto cmd_exit;
    }
    // process the server response
    ret = esp_http_client_perform(sp_fs_to_http->client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_POST, "esp_http_client_perform failed: 0x%x", ret);
        goto cmd_exit;
    }

cmd_exit:
    if (data) {
        free(data);
    }
    if (body_start) {
        free(body_start);
    }
    if (body_end) {
        free(body_end);
    }
    // clean resources
    at_fs_to_http_clean();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_POST, "command ret: 0x%x", ret);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    return ESP_AT_RESULT_CODE_OK;
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

/****************************************************************************************************************************************/

static const esp_at_cmd_struct at_custom_cmd[] = {
    {"+HTTPGET_TO_FS", NULL, NULL, at_setup_cmd_httpget_to_fs, NULL},
    {"+FS_TO_HTTP_SERVER", NULL, NULL, at_setup_cmd_fs_to_http_server, NULL},
    {"+TEST", at_test_cmd_test, at_query_cmd_test, at_setup_cmd_test, at_exe_cmd_test},

};


bool esp_at_custom_cmd_register(void)
{
    return esp_at_custom_cmd_array_regist(at_custom_cmd, sizeof(at_custom_cmd) / sizeof(esp_at_cmd_struct));
}

ESP_AT_CMD_SET_INIT_FN(esp_at_custom_cmd_register, 1);

