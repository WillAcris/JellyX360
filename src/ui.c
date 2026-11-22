#include "ui.h"

#include <stdbool.h>
#include <string.h>

#include "jellyx360_shaders.h"
#include "jf.h"
#include "xsdk/log.h"
#include "xsdk/text.h"

enum {
	VERTEX_FLOATS = 8,
	VERTEX_BYTES = VERTEX_FLOATS * 4,
	QUAD_VERTICES = 6,
	QUAD_BYTES = QUAD_VERTICES * VERTEX_BYTES,
	VERTEX_CAPACITY = 64 * 1024,
};

// Position, colour, texture coordinate: ui.vert's locations 0, 1 and 2.
static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 8 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 24 },
};

static float channel(uint32_t argb, int shift)
{
	return (float)((argb >> shift) & 0xFF) / 255.0f;
}

int ui_init(Ui *ui)
{
	memset(ui, 0, sizeof *ui);
	NTSTATUS status = xsdk_shader_load(&ui->vertex, ui_vert.container, ui_vert.container_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_load(&ui->pixel, ui_frag.container, ui_frag.container_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_set_vertex_layout(&ui->vertex, layout, sizeof layout / sizeof layout[0],
			(const uint32_t[]){ VERTEX_BYTES });
	if (!NT_SUCCESS(status)) {
		jf_log("ui: shader setup failed: 0x%08X", (unsigned)status);
		return -1;
	}

	status = xsdk_texture_create(&ui->white, XSDK_FORMAT_A8R8G8B8, 1, 1, 1, false);
	if (!NT_SUCCESS(status)) {
		jf_log("ui: white texture failed: 0x%08X", (unsigned)status);
		return -1;
	}
	const uint32_t white = 0xFFFFFFFF;
	xsdk_texture_upload(&ui->white, 0, &white, sizeof white);
	xsdk_gpu_invalidate(ui->white.address, ui->white.size_bytes);
	const XsdkSampler sampler = { .mag_filter = XSDK_FILTER_LINEAR,
		.min_filter = XSDK_FILTER_LINEAR,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP };
	xsdk_texture_sampler_state(&ui->sampler, &sampler);

	const XsdkBlend blend = { .color_source = XSDK_BLEND_SRC_ALPHA,
		.color_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA,
		.color_op = XSDK_BLEND_OP_ADD,
		.alpha_source = XSDK_BLEND_SRC_ALPHA,
		.alpha_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA,
		.alpha_op = XSDK_BLEND_OP_ADD,
		.write_mask = XSDK_WRITE_ALL };
	xsdk_gpu_blend_state(&ui->blend, &blend);

	for (int i = 0; i < 2; i++) {
		ui->vertices[i] = xsdk_gpu_alloc(VERTEX_CAPACITY);
		if (!ui->vertices[i]) {
			jf_log("ui: vertex buffer allocation failed");
			return -1;
		}
	}
	ui->capacity_bytes = VERTEX_CAPACITY;
	return 0;
}

void ui_shutdown(Ui *ui)
{
	xsdk_texture_free(&ui->white);
	xsdk_shader_free(&ui->pixel);
	xsdk_shader_free(&ui->vertex);
	for (int i = 0; i < 2; i++) {
		if (ui->vertices[i]) {
			xsdk_gpu_free(ui->vertices[i]);
			ui->vertices[i] = NULL;
		}
	}
}

void ui_begin(Ui *ui, uint32_t width, uint32_t height)
{
	ui->slot = ui->frame & 1;
	if (ui->fence_valid[ui->slot]) {
		const NTSTATUS status = xsdk_gpu_wait(ui->fence[ui->slot], 1000);
		if (!NT_SUCCESS(status))
			jf_log("ui: fence wait failed: 0x%08X", (unsigned)status);
		ui->fence_valid[ui->slot] = false;
	}
	ui->width = width;
	ui->height = height;
	ui->used_bytes = 0;
	xsdk_gpu_set_target(width, height);
	// A resolve leaves no shaders, blend or texture behind, so every frame sets them again.
	xsdk_gpu_set_shaders(&ui->vertex, &ui->pixel);
	xsdk_built_shader_set_literals(&ui_vert);
	xsdk_built_shader_set_literals(&ui_frag);
	xsdk_gpu_set_blend(&ui->blend);
}

void ui_quad(Ui *ui, const XsdkTexture *texture, float x, float y, float width, float height, uint32_t argb)
{
	ui_quad_colors(ui, texture, x, y, width, height, argb, argb, argb, argb);
}

void ui_quad_colors(Ui *ui, const XsdkTexture *texture, float x, float y, float width, float height,
	uint32_t c_tl, uint32_t c_tr, uint32_t c_bl, uint32_t c_br)
{
	if (ui->used_bytes + QUAD_BYTES > ui->capacity_bytes || !width || !height)
		return;
	if (!texture)
		texture = &ui->white;

	const float left = x / (float)ui->width * 2.0f - 1.0f;
	const float right = (x + width) / (float)ui->width * 2.0f - 1.0f;
	const float top = 1.0f - y / (float)ui->height * 2.0f;
	const float bottom = 1.0f - (y + height) / (float)ui->height * 2.0f;
	const float r_tl = channel(c_tl, 16), g_tl = channel(c_tl, 8), b_tl = channel(c_tl, 0), a_tl = channel(c_tl, 24);
	const float r_tr = channel(c_tr, 16), g_tr = channel(c_tr, 8), b_tr = channel(c_tr, 0), a_tr = channel(c_tr, 24);
	const float r_bl = channel(c_bl, 16), g_bl = channel(c_bl, 8), b_bl = channel(c_bl, 0), a_bl = channel(c_bl, 24);
	const float r_br = channel(c_br, 16), g_br = channel(c_br, 8), b_br = channel(c_br, 0), a_br = channel(c_br, 24);
	const float corners[QUAD_VERTICES][VERTEX_FLOATS] = {
		{ left, top, r_tl, g_tl, b_tl, a_tl, 0.0f, 0.0f },
		{ right, top, r_tr, g_tr, b_tr, a_tr, 1.0f, 0.0f },
		{ left, bottom, r_bl, g_bl, b_bl, a_bl, 0.0f, 1.0f },
		{ left, bottom, r_bl, g_bl, b_bl, a_bl, 0.0f, 1.0f },
		{ right, top, r_tr, g_tr, b_tr, a_tr, 1.0f, 0.0f },
		{ right, bottom, r_br, g_br, b_br, a_br, 1.0f, 1.0f },
	};

	uint32_t words[QUAD_VERTICES * VERTEX_FLOATS];
	memcpy(words, corners, sizeof words);
	volatile uint32_t *const destination = ui->vertices[ui->slot] + ui->used_bytes / 4;
	for (uint32_t i = 0; i < QUAD_VERTICES * VERTEX_FLOATS; i++)
		destination[i] = words[i];
	const uint32_t address = xsdk_gpu_address(ui->vertices[ui->slot]) + ui->used_bytes;
	xsdk_gpu_invalidate(address, QUAD_BYTES);

	xsdk_gpu_set_shaders(&ui->vertex, &ui->pixel);
	xsdk_built_shader_set_literals(&ui_vert);
	xsdk_built_shader_set_literals(&ui_frag);
	xsdk_gpu_set_blend(&ui->blend);
	xsdk_gpu_set_texture(0, texture, &ui->sampler);
	xsdk_gpu_set_vertices(0, address, QUAD_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	ui->used_bytes += QUAD_BYTES;
}

void ui_rect(Ui *ui, float x, float y, float width, float height, uint32_t argb)
{
	ui_quad(ui, NULL, x, y, width, height, argb);
}

void ui_rect_gradient(Ui *ui, float x, float y, float width, float height, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br)
{
	ui_quad_colors(ui, NULL, x, y, width, height, tl, tr, bl, br);
}

void ui_shadow(Ui *ui, float x, float y, float width, float height)
{
	ui_rect(ui, x + 4, y + 6, width, height, 0x60000000);
}

void ui_end(Ui *ui)
{
	ui->fence[ui->slot] = xsdk_gpu_fence();
	ui->fence_valid[ui->slot] = true;
	ui->frame++;
}

void ui_text(float x, float y, uint32_t scale, uint32_t argb, const char *text)
{
	xsdk_text_draw((int32_t)x, (int32_t)y, scale, argb, text);
}
