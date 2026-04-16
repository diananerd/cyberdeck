/*
 * CyberDeck — HTTPS download service with resume
 * FreeRTOS task on Core 0, processes a queue of download requests.
 */

#include "svc_downloader.h"
#include "svc_event.h"
#include "os_service.h"
#include "os_task.h"

#define SVC_DOWNLOADER_NAME "svc_downloader"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "svc_downloader";

#define DL_TASK_STACK       (6 * 1024)
#define DL_CHUNK_SIZE       4096
#define DL_SIDECAR_INTERVAL (64 * 1024)  /* Update sidecar every 64KB */
#define DL_QUEUE_DEPTH      4

typedef struct {
    char url[256];
    char dest[128];
    uint32_t request_id;
} dl_request_t;

static QueueHandle_t s_queue = NULL;
static uint32_t s_cancel_id = UINT32_MAX;

/* Sidecar file format: "<bytes_received>\n<etag>\n" */

static void write_sidecar(const char *dest, uint32_t received, const char *etag)
{
    char path[140];
    snprintf(path, sizeof(path), "%s.download", dest);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%lu\n%s\n", (unsigned long)received, etag ? etag : "");
        fclose(f);
    }
}

static bool read_sidecar(const char *dest, uint32_t *received, char *etag, size_t etag_len)
{
    char path[140];
    snprintf(path, sizeof(path), "%s.download", dest);
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[256];
    if (fgets(line, sizeof(line), f)) {
        *received = (uint32_t)strtoul(line, NULL, 10);
    }
    if (fgets(line, sizeof(line), f) && etag) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        strncpy(etag, line, etag_len - 1);
        etag[etag_len - 1] = '\0';
    }
    fclose(f);
    return *received > 0;
}

static void delete_sidecar(const char *dest)
{
    char path[140];
    snprintf(path, sizeof(path), "%s.download", dest);
    remove(path);
}

static void do_download(const dl_request_t *req)
{
    ESP_LOGI(TAG, "Downloading: %s -> %s (id=%lu)",
             req->url, req->dest, (unsigned long)req->request_id);
    os_service_update(SVC_DOWNLOADER_NAME, SVC_STATE_RUNNING, req->url);

    /* Check for resume state */
    uint32_t resume_from = 0;
    char saved_etag[128] = {0};
    bool resuming = read_sidecar(req->dest, &resume_from, saved_etag, sizeof(saved_etag));

    esp_http_client_config_t cfg = {
        .url = req->url,
        .timeout_ms = 10000,
        .buffer_size = DL_CHUNK_SIZE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        svc_event_post(EVT_DOWNLOAD_ERROR, &req->request_id, sizeof(req->request_id));
        return;
    }

    /* Set Range header for resume */
    if (resuming && resume_from > 0) {
        char range[32];
        snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)resume_from);
        esp_http_client_set_header(client, "Range", range);
        if (saved_etag[0]) {
            esp_http_client_set_header(client, "If-Match", saved_etag);
        }
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        svc_event_post(EVT_DOWNLOAD_ERROR, &req->request_id, sizeof(req->request_id));
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    /* Handle 416 Range Not Satisfiable or ETag mismatch -> restart */
    if (status == 416 || (resuming && status != 206)) {
        ESP_LOGW(TAG, "Resume not possible (status %d), restarting", status);
        resume_from = 0;
        delete_sidecar(req->dest);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        /* Retry without range — recursive would be risky, just start fresh */
        dl_request_t retry = *req;
        do_download(&retry);
        return;
    }

    uint32_t total = (content_length > 0) ? (uint32_t)content_length + resume_from : 0;

    /* Open file for write/append */
    FILE *f = fopen(req->dest, resuming && resume_from > 0 ? "ab" : "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open file: %s", req->dest);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        svc_event_post(EVT_DOWNLOAD_ERROR, &req->request_id, sizeof(req->request_id));
        return;
    }

    /* Save ETag for future resume */
    char etag[128] = {0};
    esp_http_client_get_header(client, "ETag", (char **)NULL);
    /* esp_http_client doesn't have a clean get-response-header; store from first request */

    uint8_t buf[DL_CHUNK_SIZE];
    uint32_t received = resume_from;
    uint32_t since_sidecar = 0;
    bool cancelled = false;

    while (1) {
        if (s_cancel_id == req->request_id) {
            cancelled = true;
            break;
        }

        int read_len = esp_http_client_read(client, (char *)buf, DL_CHUNK_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Read error");
            break;
        }
        if (read_len == 0) break; /* done */

        fwrite(buf, 1, read_len, f);
        received += read_len;
        since_sidecar += read_len;

        /* Update sidecar periodically */
        if (since_sidecar >= DL_SIDECAR_INTERVAL) {
            write_sidecar(req->dest, received, etag);
            since_sidecar = 0;
        }

        /* Post progress */
        svc_download_progress_t prog = {
            .request_id = req->request_id,
            .bytes_received = received,
            .bytes_total = total,
        };
        svc_event_post(EVT_DOWNLOAD_PROGRESS, &prog, sizeof(prog));
    }

    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (cancelled) {
        write_sidecar(req->dest, received, etag);
        ESP_LOGI(TAG, "Download cancelled (id=%lu)", (unsigned long)req->request_id);
        s_cancel_id = UINT32_MAX;
        os_service_update(SVC_DOWNLOADER_NAME, SVC_STATE_IDLE, "");
    } else if (received > resume_from) {
        delete_sidecar(req->dest);
        ESP_LOGI(TAG, "Download complete: %lu bytes", (unsigned long)received);
        os_service_update(SVC_DOWNLOADER_NAME, SVC_STATE_IDLE, "");
        svc_event_post(EVT_DOWNLOAD_COMPLETE, &req->request_id, sizeof(req->request_id));
    } else {
        ESP_LOGE(TAG, "Download failed, no data received");
        os_service_update(SVC_DOWNLOADER_NAME, SVC_STATE_ERROR, "failed");
        svc_event_post(EVT_DOWNLOAD_ERROR, &req->request_id, sizeof(req->request_id));
    }
}

static void downloader_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Downloader task started");

    dl_request_t req;
    while (1) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) == pdTRUE) {
            do_download(&req);
        }
    }
}

esp_err_t svc_downloader_init(void)
{
    s_queue = xQueueCreate(DL_QUEUE_DEPTH, sizeof(dl_request_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    os_task_config_t cfg = {
        .name       = "downloader",
        .fn         = downloader_task,
        .arg        = NULL,
        .stack_size = DL_TASK_STACK,
        .priority   = OS_PRIO_HIGH,
        .core       = OS_CORE_BG,
        .owner      = OS_OWNER_SYSTEM,
    };
    esp_err_t ret = os_task_create(&cfg, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create downloader task: %s", esp_err_to_name(ret));
        return ret;
    }

    os_service_register(SVC_DOWNLOADER_NAME);
    os_service_update(SVC_DOWNLOADER_NAME, SVC_STATE_IDLE, "");

    ESP_LOGI(TAG, "Downloader initialized");
    return ESP_OK;
}

esp_err_t svc_downloader_enqueue(const char *url, const char *dest_path,
                                  uint32_t request_id)
{
    if (!s_queue) return ESP_ERR_INVALID_STATE;

    dl_request_t req = { .request_id = request_id };
    strncpy(req.url, url, sizeof(req.url) - 1);
    strncpy(req.dest, dest_path, sizeof(req.dest) - 1);

    if (xQueueSend(s_queue, &req, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Download queue full");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t svc_downloader_cancel(uint32_t request_id)
{
    s_cancel_id = request_id;
    return ESP_OK;
}
