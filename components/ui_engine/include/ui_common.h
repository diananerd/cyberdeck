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
 * @brief Create a horizontal divider line (for list views only).
 */
lv_obj_t *ui_common_divider(lv_obj_t *parent);

/* ========== Data display helpers ========== */

/**
 * @brief Create a stacked data row: small dim label above larger primary value.
 * @param parent  Parent container
 * @param label   Field name (shown small, dimmed)
 * @param value   Field value (shown larger, primary color)
 * @return The value label object — caller can call lv_label_set_text() to update it.
 */
lv_obj_t *ui_common_data_row(lv_obj_t *parent, const char *label, const char *value);

/**
 * @brief Create a fixed-height transparent gap between logical sections.
 *        Adds ~18 px of visual breathing room on top of the normal row spacing,
 *        creating a clear Gestalt separation between unrelated content groups.
 */
lv_obj_t *ui_common_section_gap(lv_obj_t *parent);

/**
 * @brief Create an invisible spacer that absorbs remaining flex space.
 *        Place before an action_row to pin buttons to the bottom.
 */
lv_obj_t *ui_common_spacer(lv_obj_t *parent);

/**
 * @brief Create a right-aligned row for [secondary][primary] action buttons.
 * @return The row container — add buttons via ui_common_btn().
 */
lv_obj_t *ui_common_action_row(lv_obj_t *parent);

/**
 * @brief Re-style an existing button as a solid primary (filled) CTA.
 *        Call after ui_common_btn() to promote it to primary action.
 */
void ui_common_btn_style_primary(lv_obj_t *btn);

#ifdef __cplusplus
}
#endif
