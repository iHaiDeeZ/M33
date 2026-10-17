// M33 tools: rasterizes images.svg with PPSSPP's nanosvg the way UI/UIAtlas.cpp does, to review the brand images.
//   gcc -O2 -o svg-raster tools/svg-raster.c -I <ppsspp>/ext/nanosvg/src -lm
//   svg-raster <staged images.svg> <scale> <out dir> I_ICON I_LOGO ...
// Rasterizes images.svg like PPSSPP's UIAtlas.cpp (only the listed ids visible), crops each id,
// writes <id>.ppm-like raw RGBA files with a header line.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"
int main(int argc, char **argv) {
	const char *file = argv[1]; float scale = atof(argv[2]); const char *outdir = argv[3];
	NSVGimage *img = nsvgParseFromFile(file, "px", 96.0f);
	if (!img) { printf("parse failed\n"); return 1; }
	for (int a = 4; a < argc; a++) {
		const char *id = argv[a];
		float b[4] = {1e9, 1e9, -1e9, -1e9}; int found = 0;
		for (NSVGshape *s = img->shapes; s; s = s->next) {
			if (strcmp(s->id, id) == 0) { s->flags |= NSVG_FLAGS_VISIBLE; found++;
				if (s->bounds[0] < b[0]) b[0] = s->bounds[0]; if (s->bounds[1] < b[1]) b[1] = s->bounds[1];
				if (s->bounds[2] > b[2]) b[2] = s->bounds[2]; if (s->bounds[3] > b[3]) b[3] = s->bounds[3];
			} else s->flags &= ~NSVG_FLAGS_VISIBLE;
		}
		if (!found) { printf("%s: not found\n", id); continue; }
		int W = img->width * scale, H = img->height * scale;
		unsigned char *px = calloc(W * H, 4);
		NSVGrasterizer *r = nsvgCreateRasterizer();
		nsvgRasterize(r, img, 0, 0, scale, px, W, H, W * 4);
		int x0 = floorf(b[0] * scale), y0 = floorf(b[1] * scale), x1 = ceilf(b[2] * scale), y1 = ceilf(b[3] * scale);
		if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
		char path[512]; snprintf(path, sizeof path, "%s/%s.rgba", outdir, id);
		FILE *f = fopen(path, "wb"); fprintf(f, "%d %d\n", x1 - x0, y1 - y0);
		for (int y = y0; y < y1; y++) fwrite(px + (y * W + x0) * 4, 4, x1 - x0, f);
		fclose(f); printf("%s: %d shapes, %dx%d px at scale %.1f\n", id, found, x1 - x0, y1 - y0, scale);
		nsvgDeleteRasterizer(r); free(px);
	}
	return 0;
}
