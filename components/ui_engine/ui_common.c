/*
 * S3 Cyber-Deck — Common widget builders
 * Reusable UI components with cyberdeck styling.
 */

#include "ui_common.h"
#include "ui_theme.h"
#include "ui_statusbar.h"

/* ---------- Internal helpers ---------- */

typedef struct {
    ui_list_cb_t cb;
    void *data;
    uint32_t index;
} list_item_ctx_t;

static void list_item_click_cb(lv_event_t *e)
{
    list_item_ctx_t *ctx = (list_item_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->cb) {
        ctx->cb(ctx->index, ctx->data);
    }
}

/* ========== Panels ========== */

lv_obj_t *ui_common_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    ui_theme_style_container(panel);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

lv_obj_t *ui_common_content_area(lv_obj_t *parent)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 8, 0);
    lv_obj_set_style_pad_top(cont, 4, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cont, 4, 0);

    /* Scrollbar */
    ui_theme_style_scrollbar(cont);

    return cont;
}

/* ========== Lists ========== */

lv_obj_t *ui_common_list(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 0, 0);
    ui_theme_style_scrollbar(list);
    return list;
}

lv_obj_t *ui_common_list_add(lv_obj_t *list, const char *text,
                              uint32_t index, ui_list_cb_t cb, void *data)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    ui_theme_style_list_item(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text ? text : "");
    ui_theme_style_label(label, &CYBERDECK_FONT_MD);

    if (cb) {
        /* Allocate context — LVGL doesn't free user_data, but these live for the screen's lifetime */
        list_item_ctx_t *ctx = lv_mem_alloc(sizeof(list_item_ctx_t));
        if (ctx) {
            ctx->cb = cb;
            ctx->data = data;
            ctx->index = index;
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, list_item_click_cb, LV_EVENT_CLICKED, ctx);

            /* Press feedback: invert colors */
            const cyberdeck_theme_t *t = ui_theme_get();
            lv_obj_set_style_bg_color(row, t->primary, LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        }
    }

    return row;
}

lv_obj_t *ui_common_list_add_two_line(lv_obj_t *list, const char *primary,
                                       const char *secondary, uint32_t index,
                                       ui_list_cb_t cb, void *data)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    ui_theme_style_list_item(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 2, 0);

    lv_obj_t *lbl1 = lv_label_create(row);
    lv_label_set_text(lbl1, primary ? primary : "");
    ui_theme_style_label(lbl1, &CYBERDECK_FONT_MD);

    lv_obj_t *lbl2 = lv_label_create(row);
    lv_label_set_text(lbl2, secondary ? secondary : "");
    ui_theme_style_label_dim(lbl2, &CYBERDECK_FONT_SM);

    if (cb) {
        list_item_ctx_t *ctx = lv_mem_alloc(sizeof(list_item_ctx_t));
        if (ctx) {
            ctx->cb = cb;
            ctx->data = data;
            ctx->index = index;
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, list_item_click_cb, LV_EVENT_CLICKED, ctx);

            const cyberdeck_theme_t *t = ui_theme_get();
            lv_obj_set_style_bg_color(row, t->primary, LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        }
    }

    return row;
}

/* ========== Grid ========== */

lv_obj_t *ui_common_grid(lv_obj_t *parent, uint8_t cols, lv_coord_t row_h)
{
    /* Build column descriptors: equal-width columns */
    static lv_coord_t col_dsc[9];  /* max 8 cols + LV_GRID_TEMPLATE_LAST */
    for (int i = 0; i < cols && i < 8; i++) {
        col_dsc[i] = LV_GRID_FR(1);
    }
    col_dsc[cols < 8 ? cols : 8] = LV_GRID_TEMPLATE_LAST;

    static lv_coord_t row_dsc[2];
    row_dsc[0] = row_h;
    row_dsc[1] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 4, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);

    return grid;
}

lv_obj_t *ui_common_grid_cell(lv_obj_t *grid, const char *icon,
                               const char *label, uint8_t col, uint8_t row)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    lv_obj_t *cell = lv_obj_create(grid);
    ui_theme_style_container(cell);
    lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, col, 1,
                         LV_GRID_ALIGN_STRETCH, row, 1);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 4, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

    /* Press feedback */
    lv_obj_set_style_bg_color(cell, t->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_STATE_PRESSED);

    if (icon && icon[0]) {
        lv_obj_t *lbl_icon = lv_label_create(cell);
        lv_label_set_text(lbl_icon, icon);
        lv_obj_set_style_text_color(lbl_icon, t->text, 0);
        lv_obj_set_style_text_font(lbl_icon, &CYBERDECK_FONT_XL, 0);
        lv_obj_set_style_text_color(lbl_icon, t->bg_dark, LV_STATE_PRESSED);
    }

    if (label && label[0]) {
        lv_obj_t *lbl = lv_label_create(cell);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, t->text, 0);
        lv_obj_set_style_text_font(lbl, &CYBERDECK_FONT_SM, 0);
        lv_obj_set_style_text_color(lbl, t->bg_dark, LV_STATE_PRESSED);
    }

    return cell;
}

/* ========== Cards ========== */

lv_obj_t *ui_common_card(lv_obj_t *parent, const char *title,
                          lv_coord_t w, lv_coord_t h)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    lv_obj_t *card = lv_obj_create(parent);
    ui_theme_style_container(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (title && title[0]) {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, title);
        lv_obj_set_style_text_color(lbl, t->text, 0);
        lv_obj_set_style_text_font(lbl, &CYBERDECK_FONT_MD, 0);

        /* Divider after title */
        ui_common_divider(card);
    }

    /* Body container for content */
    lv_obj_t *body = lv_obj_create(card);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    return body;
}

/* ========== Buttons ========== */

lv_obj_t *ui_common_btn(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = lv_btn_create(parent);
    ui_theme_style_btn(btn);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text ? text : "");

    return btn;
}

lv_obj_t *ui_common_btn_full(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = ui_common_btn(parent, text);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_style_text_align(btn, LV_TEXT_ALIGN_CENTER, 0);
    /* Center the label */
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label) {
        lv_obj_center(label);
    }
    return btn;
}

/* ========== Divider ========== */

lv_obj_t *ui_common_divider(lv_obj_t *parent)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(line, t->primary_dim, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    return line;
}
