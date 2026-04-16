/*
 * CyberDeck — Settings > About
 * Device identity, firmware details, and OTA update trigger.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "svc_settings.h"
#include "svc_ota.h"
#include "os_settings.h"
#include "hal_sdcard.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "settings_about";

#define CREDITS_PATH     HAL_SDCARD_MOUNT_POINT "/system/credits.txt"
#define CREDITS_DEFAULT  "Diana Mart\xc3\xadnez <diananerdoficial@gmail.com>\n"
#define CREDITS_LINE_MAX 128

/* Replace UTF-8 sequences unsupported by the embedded Montserrat font with
 * safe ASCII equivalents. Operates in-place; output is always <= input length. */
static void sanitize_utf8(char *s)
{
    unsigned char *r = (unsigned char *)s;
    unsigned char *w = (unsigned char *)s;

    while (*r) {
        /* 2-byte Latin-1 Supplement: 0xC3 0x80–0xBF → U+00C0–U+00FF */
        if (r[0] == 0xC3 && r[1] >= 0x80 && r[1] <= 0xBF) {
            unsigned char c = r[1];
            char rep = '?';
            /* Uppercase accented vowels / ñ */
            if      (c == 0x80 || c == 0x81 || c == 0x82 || c == 0x83 || c == 0x84 || c == 0x85) rep = 'A';
            else if (c == 0x86) rep = 'A';  /* Æ */
            else if (c == 0x87) rep = 'C';  /* Ç */
            else if (c == 0x88 || c == 0x89 || c == 0x8A || c == 0x8B) rep = 'E';
            else if (c == 0x8C || c == 0x8D || c == 0x8E || c == 0x8F) rep = 'I';
            else if (c == 0x91) rep = 'N';  /* Ñ */
            else if (c == 0x92 || c == 0x93 || c == 0x94 || c == 0x95 || c == 0x96) rep = 'O';
            else if (c == 0x99 || c == 0x9A || c == 0x9B || c == 0x9C) rep = 'U';
            /* Lowercase accented vowels / ñ */
            else if (c == 0xA0 || c == 0xA1 || c == 0xA2 || c == 0xA3 || c == 0xA4 || c == 0xA5) rep = 'a';
            else if (c == 0xA6) rep = 'a';  /* æ */
            else if (c == 0xA7) rep = 'c';  /* ç */
            else if (c == 0xA8 || c == 0xA9 || c == 0xAA || c == 0xAB) rep = 'e';
            else if (c == 0xAC || c == 0xAD || c == 0xAE || c == 0xAF) rep = 'i';
            else if (c == 0xB1) rep = 'n';  /* ñ */
            else if (c == 0xB2 || c == 0xB3 || c == 0xB4 || c == 0xB5 || c == 0xB6) rep = 'o';
            else if (c == 0xB9 || c == 0xBA || c == 0xBB || c == 0xBC) rep = 'u';
            *w++ = (unsigned char)rep;
            r += 2;
        }
        /* Any other multi-byte sequence: skip entirely */
        else if (r[0] >= 0x80) {
            uint8_t byte_count = (r[0] >= 0xF0) ? 4 : (r[0] >= 0xE0) ? 3 : 2;
            r += byte_count;
        }
        else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Ensure /system/credits.txt exists; create with default content if missing.
 * Returns true if the file is ready to read, false if SD is unavailable. */
static bool credits_ensure(void)
{
    /* Quick-check: try opening for read first (avoids unnecessary mkdir) */
    FILE *f = fopen(CREDITS_PATH, "r");
    if (f) { fclose(f); return true; }

    /* File missing — try to create it (mkdir -p /system, then write) */
    mkdir(HAL_SDCARD_MOUNT_POINT "/system", 0755);  /* ignore EEXIST */
    f = fopen(CREDITS_PATH, "w");
    if (!f) {
        ESP_LOGW(TAG, "Cannot create credits.txt (SD not mounted?)");
        return false;
    }
    fputs(CREDITS_DEFAULT, f);
    fclose(f);
    ESP_LOGI(TAG, "Created default credits.txt");
    return true;
}

/* Render credits section into parent flex-column container. */
static void render_credits(lv_obj_t *parent)
{
    lv_obj_t *sec_lbl = lv_label_create(parent);
    lv_label_set_text(sec_lbl, "CREDITS");
    ui_theme_style_label_dim(sec_lbl, &CYBERDECK_FONT_SM);
    lv_obj_set_style_pad_top(sec_lbl, 4, 0);

    if (!credits_ensure()) {
        lv_obj_t *na = lv_label_create(parent);
        lv_label_set_text(na, "(SD card not available)");
        ui_theme_style_label_dim(na, &CYBERDECK_FONT_SM);
        return;
    }

    FILE *f = fopen(CREDITS_PATH, "r");
    if (!f) {
        lv_obj_t *na = lv_label_create(parent);
        lv_label_set_text(na, "(unavailable)");
        ui_theme_style_label_dim(na, &CYBERDECK_FONT_SM);
        return;
    }

    const cyberdeck_theme_t *t = ui_theme_get();
    char line[CREDITS_LINE_MAX];
    bool any = false;
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline/CR */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;  /* skip blank lines */
        sanitize_utf8(line);

        /* Parse "Name <email>" — split on '<' */
        char name_buf[CREDITS_LINE_MAX] = {0};
        char email_buf[CREDITS_LINE_MAX] = {0};
        char *lt = strchr(line, '<');
        if (lt) {
            size_t name_len = (size_t)(lt - line);
            while (name_len > 0 && line[name_len - 1] == ' ') name_len--;
            memcpy(name_buf, line, name_len);
            char *gt = strchr(lt, '>');
            if (gt) {
                size_t email_len = (size_t)(gt - lt - 1);
                memcpy(email_buf, lt + 1, email_len);
            } else {
                /* No closing '>': treat rest of line as email */
                strncpy(email_buf, lt + 1, sizeof(email_buf) - 1);
            }
        } else {
            strncpy(name_buf, line, sizeof(name_buf) - 1);
        }

        /* Blank spacer between entries (not before first) */
        if (any) {
            lv_obj_t *gap = lv_obj_create(parent);
            lv_obj_set_size(gap, LV_PCT(100), 6);
            lv_obj_set_style_bg_opa(gap, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(gap, 0, 0);
            lv_obj_clear_flag(gap, LV_OBJ_FLAG_SCROLLABLE);
        }

        /* Name — primary color, MD font */
        if (name_buf[0]) {
            lv_obj_t *name_lbl = lv_label_create(parent);
            lv_label_set_text(name_lbl, name_buf);
            lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(name_lbl, LV_PCT(100));
            lv_obj_set_style_text_color(name_lbl, t->primary, 0);
            lv_obj_set_style_text_font(name_lbl, &CYBERDECK_FONT_MD, 0);
        }

        /* Email — dim color, SM font */
        if (email_buf[0]) {
            lv_obj_t *email_lbl = lv_label_create(parent);
            lv_label_set_text(email_lbl, email_buf);
            lv_label_set_long_mode(email_lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(email_lbl, LV_PCT(100));
            ui_theme_style_label_dim(email_lbl, &CYBERDECK_FONT_SM);
        }

        any = true;
    }
    fclose(f);

    if (!any) {
        lv_obj_t *empty = lv_label_create(parent);
        lv_label_set_text(empty, "(empty)");
        ui_theme_style_label_dim(empty, &CYBERDECK_FONT_SM);
    }
}

static void ota_btn_cb(lv_event_t *e)
{
    (void)e;
    const char *ota_url = os_settings_get()->ota_url;  /* E3: read from cache */
    if (ota_url[0] == '\0') {
        ui_effect_toast("No OTA URL configured", 2000);
        return;
    }
    ui_effect_toast("Starting OTA update...", 2000);
    svc_ota_start(ota_url);
    ESP_LOGI(TAG, "OTA triggered: %s", ota_url);
}

/* D1: returns NULL (no state needed) */
static void *about_on_create(lv_obj_t *screen, const view_args_t *args, void *app_data)
{
    (void)args;
    (void)app_data;
    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);

    const esp_app_desc_t *desc = esp_app_get_description();

    char ver_str[32];
    snprintf(ver_str, sizeof(ver_str), "%s", desc ? desc->version : "0.1.0");
    ui_common_data_row(content, "FIRMWARE:", ver_str);

    char idf_str[24];
    snprintf(idf_str, sizeof(idf_str), "%s", IDF_VER);
    ui_common_data_row(content, "IDF VERSION:", idf_str);

    char app_str[32];
    snprintf(app_str, sizeof(app_str), "%s", desc ? desc->project_name : "cyberdeck");
    ui_common_data_row(content, "APPLICATION:", app_str);

    char build_str[36];
    snprintf(build_str, sizeof(build_str), "%.16s %.8s",
             desc ? desc->date : "?", desc ? desc->time : "");
    ui_common_data_row(content, "BUILD DATE:", build_str);

    uint32_t boot_count = os_settings_get()->boot_count;  /* E3: read from cache */
    char boot_str[16];
    snprintf(boot_str, sizeof(boot_str), "%lu", (unsigned long)boot_count);
    ui_common_data_row(content, "BOOT COUNT:", boot_str);

    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    char chip_str[32];
    snprintf(chip_str, sizeof(chip_str), "ESP32-S3  rev %d  %d cores",
             chip.revision, chip.cores);
    ui_common_data_row(content, "CHIP:", chip_str);

    ui_common_data_row(content, "FLASH:", CONFIG_ESPTOOLPY_FLASHSIZE);

    ui_common_section_gap(content);

    const char *ota_url = os_settings_get()->ota_url;          /* E3: read from cache */
    const char *ota_display = (ota_url[0] != '\0') ? ota_url : "(not configured)";

    lv_obj_t *ota_key = lv_label_create(content);
    lv_label_set_text(ota_key, "OTA ENDPOINT:");
    ui_theme_style_label_dim(ota_key, &CYBERDECK_FONT_SM);

    lv_obj_t *ota_lbl = lv_label_create(content);
    lv_label_set_text(ota_lbl, ota_display);
    lv_label_set_long_mode(ota_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ota_lbl, LV_PCT(100));
    ui_theme_style_label(ota_lbl, &CYBERDECK_FONT_MD);

    ui_common_section_gap(content);
    render_credits(content);

    ui_common_spacer(content);

    lv_obj_t *btn_row = ui_common_action_row(content);
    lv_obj_t *ota_btn = ui_common_btn(btn_row, "Check for Update");
    ui_common_btn_style_primary(ota_btn);
    lv_obj_add_event_cb(ota_btn, ota_btn_cb, LV_EVENT_CLICKED, NULL);

    return NULL;
}

const view_cbs_t settings_about_cbs = {
    .on_create  = about_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = NULL,
};
