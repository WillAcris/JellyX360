#ifndef JF_UI_H
#define JF_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "xsdk/gpu.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// An immediate-mode 2D layer: coloured or textured quads in clip space, and text through xsdk_text_draw.
// The vertex buffer is double buffered, so a frame's writes never touch what the GPU may still be drawing.
typedef struct {
	XsdkShader vertex;
	XsdkShader pixel;
	XsdkTexture white;
	XsdkSamplerState sampler;
	XsdkBlendState blend;
	volatile uint32_t *vertices[2];
	uint32_t capacity_bytes;
	uint32_t used_bytes;
	uint32_t width;
	uint32_t height;
	uint32_t frame;
	uint32_t slot;
	uint32_t fence[2];
	bool fence_valid[2];
} Ui;

int ui_init(Ui *ui);
void ui_shutdown(Ui *ui);
// Waits for the slot's old frame, sets the target and the state the quads need.
void ui_begin(Ui *ui, uint32_t width, uint32_t height);
void ui_quad(Ui *ui, const XsdkTexture *texture, float x, float y, float width, float height, uint32_t argb);
void ui_quad_colors(Ui *ui, const XsdkTexture *texture, float x, float y, float width, float height, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br);
void ui_rect(Ui *ui, float x, float y, float width, float height, uint32_t argb);
// Diagonal gradient quad: tl/top-left, tr/top-right, bl/bottom-left, br/bottom-right.
void ui_rect_gradient(Ui *ui, float x, float y, float width, float height, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br);
// Soft shadow behind a card.
void ui_shadow(Ui *ui, float x, float y, float width, float height);
// After the frame's resolve: fences what was drawn and moves to the next slot.
void ui_end(Ui *ui);
void ui_text(float x, float y, uint32_t scale, uint32_t argb, const char *text);

#endif
