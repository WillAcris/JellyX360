#ifndef JF_IMAGE_H
#define JF_IMAGE_H

#include <stddef.h>
#include <stdint.h>

// Decodes a JPEG or PNG into 0xAARRGGBB pixels, rows top to bottom. Returns 0 on success.
int image_decode(const uint8_t *data, size_t length, uint32_t **pixels, int *width, int *height);
void image_free(uint32_t *pixels);

#endif
