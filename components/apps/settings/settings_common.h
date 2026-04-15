/*
 * CyberDeck — Settings screens: includes y tipos comunes.
 *
 * Incluir este header en cada settings_*.c en lugar de repetir los includes
 * individualmente. Agrega aquí los includes que aparecen en ≥2 settings files.
 */

#pragma once

/* App framework */
#include "app_settings.h"
#include "app_registry.h"

/* UI layer */
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_engine.h"
#include "ui_statusbar.h"
#include "ui_navbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "ui_keyboard.h"

/* Services */
#include "svc_settings.h"
#include "svc_event.h"

/* libc */
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
