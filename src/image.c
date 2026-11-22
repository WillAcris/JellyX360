#include "image.h"

#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

int image_decode(const uint8_t *data, size_t length, uint32_t **pixels, int *width, int *height)
{
	int w = 0;
	int h = 0;
	int components = 0;
	unsigned char *rgba = stbi_load_from_memory(data, (int)length, &w, &h, &components, 4);
	if (!rgba || w <= 0 || h <= 0) {
		stbi_image_free(rgba);
		return -1;
	}
	uint32_t *argb = malloc((size_t)w * (size_t)h * sizeof *argb);
	if (!argb) {
		stbi_image_free(rgba);
		return -1;
	}
	for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
		const unsigned char *const source = rgba + i * 4;
		argb[i] = (uint32_t)source[3] << 24 | (uint32_t)source[0] << 16 | (uint32_t)source[1] << 8 | source[2];
	}
	stbi_image_free(rgba);
	*pixels = argb;
	*width = w;
	*height = h;
	return 0;
}

void image_free(uint32_t *pixels)
{
	free(pixels);
}
