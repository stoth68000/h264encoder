#ifndef CSC_H
#define CSC_H

#include "encoder.h"
#include "main.h"

struct csc_ctx_s
{
	int width, height, stride;

	VADisplay      va_dpy;
	VASurfaceID    rgb32_surface;

	/* video post processing is used for colorspace conversion */
	struct {
		VAConfigID cfg;
		VAContextID ctx;
		VABufferID pipeline_buf;
	} vpp;
};

/* Convert ctx->rgb32_surface to ctx->vpp.output (yuv) */
int csc_alloc(struct csc_ctx_s *ctx,
	VADisplay va_dpy, VAConfigID cfg, VAContextID ctxid,
	int width, int height, int stride);
int csc_free(struct csc_ctx_s *ctx);
VAStatus csc_convert_rgbdata_to_yuv(struct csc_ctx_s *ctx, unsigned char *data, VASurfaceID yuv_surface_output);

#endif

