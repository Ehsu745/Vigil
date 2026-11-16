/* gpu.h: the black hole drawn on the GPU (win/gpu.c) */
#ifndef VIGIL_GPU_H
#define VIGIL_GPU_H
#include "../src/vigil.h"
int gpu_init(void);                         /* 1 if the GPU can draw it (tried once; later calls say how it went) */
const char *gpu_why(void);                  /* why not, if it cannot */
/* the black hole of session s, n x n pixels, the hole's plane +-E M across: RGBA, the bottom row first (alpha: its shadow
 * and the gas in front); NULL if the GPU cannot */
/* mini: a small core's own (a satellite asleep): a tight disc, no stars */
const unsigned char *gpu_hole(const Session *s, int n, float E, double now, int mini); /* (already turned by its roll: lay it in straight) */
#endif
