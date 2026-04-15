#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback for list item tap.
 * @param index  Item index in the list
 * @param data   User data provided at creation
 */
typedef void (*ui_list_cb_t)(uint32_t index, void *data);

/* ========== Panels / Containers ========== */

/**
 * @brief Create a styled panel (container with border).
 * @param parent Parent object
 * @return The panel object
 */
lv_obj_t *ui_common_panel(lv_obj_t *parent);

/**
 * @brief Create a full-width content area below the status bar.
 *        Sets up vertical scroll and cyberdeck styling.
 * @param parent Parent screen
 * @return The content container
 */
lv_obj_t *ui_common_content_area(lv_obj_t *parent);

/* ========== Lists ========== */

/**
 * @brief Create a styled list container (vertical flex, full width).
 * @param parent Parent object
 * @return The list container
 */
lv_obj_t *ui_common_list(lv_obj_t *parent);

/**
 * @brief Add an item to a list container.
 * @param list   List created by ui_common_list()
 * @param text   Item text
 * @param index  Item index (passed to callback)
 * @param cb     Click callback (NULL to disable)
 * @param data   User data for callback
 * @return The item row object
 */
lv_obj_t *ui_common_list_add(lv_obj_t *list, const char *text,
                              uint32_t index, ui_list_cb_t cb, void *data);

/**
 * @brief Add an item with primary + secondary text.
 * @param list    List container
 * @param primary Main text
 * @param secondary  Smaller dim text below
 * @param index   Item index
 * @param cb      Click callback
 * @param data    User data
 * @return The item row object
 */
lv_obj_t *ui_common_list_add_two_line(lv_obj_t *list, const char *primary,
                                       const char *secondary, uint32_t index,
                                       ui_list_cb_t cb, void *data);

/* ========== Grid ========== */

/**
 * @brief Create a grid container for icon/card layouts.
 * @param parent  Parent object
 * @param cols    Number of columns
 * @param row_h   Row height in pixels
 * @return The grid container
 */
lv_obj_t *ui_common_grid(lv_obj_t *parent, uint8_t cols, lv_coord_t row_h);

/**
 * @brief Add a cell to a grid with icon text and label.
 * @param grid    Grid container
 * @param icon    Icon character/string (can be NULL)
 * @param label   Label text
 * @param col     Column position
 * @param row     Row position
 * @return The cell object
 */
lv_obj_t *ui_common_grid_cell(lv_obj_t *grid, const char *icon,
                               const char *label, uint8_t col, uint8_t row);

/* ========== Cards ========== */

/**
 * @brief Create a card widget (bordered box with title).
 * @param parent Parent object
 * @param title  Card title (NULL for no title)
 * @param w      Width (LV_PCT or px)
 * @param h      Height (LV_SIZE_CONTENT or px)
 * @return The card body (content area below title)
 */
lv_obj_t *ui_common_card(lv_obj_t *parent, const char *title,
                          lv_coord_t w, lv_coord_t h);

/* ========== Buttons ========== */

/**
 * @brief Create a styled button with label text.
 * @param parent Parent object
 * @param text   Button label
 * @return The button object
 */
lv_obj_t *ui_common_btn(lv_obj_t *parent, const char *text);

/**
 * @brief Create a full-width action button (for forms/dialogs).
 */
lv_obj_t *ui_common_btn_full(lv_obj_t *parent, const char *text);

/* ========== Divider ========== */

/**
 * @brief Create a horizontal divider line.
 */
lv_obj_t *ui_common_divider(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
