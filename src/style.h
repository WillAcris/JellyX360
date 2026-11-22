#ifndef JF_STYLE_H
#define JF_STYLE_H

#include <stdint.h>

// Palette. These are the exact values main.c used before the style layer existed.
static const uint32_t C_BG = 0xFF080711;
static const uint32_t C_BG_TOP = 0xFF1A1028;
static const uint32_t C_BG_BOTTOM = 0xFF0B0A10;
static const uint32_t C_PANEL = 0xFF171322;
static const uint32_t C_PANEL_TOP = 0xFF2A1F3D;
static const uint32_t C_PANEL_BOTTOM = 0xFF17141F;
static const uint32_t C_SIDEBAR = 0xFF17141F;
static const uint32_t C_HOVER = 0xFF251638;
static const uint32_t C_SELECT = 0xFFA52CF2;
static const uint32_t C_TEXT = 0xFFF3F5F8;
static const uint32_t C_DIM = 0xFFB8B4C9;
static const uint32_t C_ACCENT = 0xFFA855F7;
static const uint32_t C_ALERT = 0xFFEF4444;
static const uint32_t C_PLACEHOLDER = 0xFF26313D;
static const uint32_t C_PROGRESS_BG = 0xFF393442;
static const uint32_t C_PROGRESS_FILL = 0xFFB13CFA;
static const uint32_t C_SCRIM = 0xB0000000;
static const uint32_t C_BLACK = 0xFF000000;
static const uint32_t C_TRACK = 0x40FFFFFF;

// Spacing and panel sizes. Values match the current 720p layout exactly.
enum {
	UI_PAD = 24,
	UI_PAD_MD = 32,
	UI_PAD_LG = 56,
	UI_GAP = 12,
	UI_HEADER_H = 120,
	UI_FOOTER_H = 36,
	UI_SECTION_Y = 132,
	UI_ROW_H = 180,
	UI_CARD_W = 110,
	UI_CARD_H = 124,
	// Episode/library grid: landscape thumbnails, 4 columns x 3 rows.
	UI_GRID_CARD_W = 280,
	UI_GRID_CARD_H = 158,
	UI_GRID_COLS = 4,
	UI_GRID_ROWS = 3,
	UI_GRID_ROW_STEP = 192,
	// Detail cover stays portrait.
	UI_DETAIL_CARD_W = 200,
	UI_DETAIL_CARD_H = 300,
	UI_BORDER = 3,
	// Login form cards.
	UI_FIELD_W = 560,
	UI_FIELD_H = 72,
	UI_FIELD_GAP = 90,
	UI_FIELD_TEXT_X = 16,
	UI_LOGIN_PILL_W = 150,
	UI_LOGIN_PILL_H = 34,
	UI_LOGIN_PILL_GAP = 12,
	// Home header.
	UI_TAB_X = 320,
	UI_TAB_GAP = 32,
	UI_TAB_CHAR_W = 20,
	UI_AVATAR = 36,
	// "Jelly" is 5 cells wide at scale 2; "X360" starts right after it.
	UI_LOGO_X360_X = 156,
	// Player OSD.
	UI_OSD_HEADER_H = 72,
	UI_OSD_ACCENT_W = 6,
	UI_OSD_BAR_H = 8,
	// Live TV grid.
	UI_LIVE_COLS = 6,
	UI_LIVE_ROWS = 3,
	UI_LIVE_CARD_W = 180,
	UI_LIVE_CARD_H = 120,
	UI_LIVE_GAP = 16,
	UI_LIVE_START_Y = 156,
};

// ui_text's scale argument: one place to change the type hierarchy later.
enum {
	UI_SCALE_BODY = 1,
	UI_SCALE_SECTION = 2,
	UI_SCALE_TITLE = 3,
};

#endif
