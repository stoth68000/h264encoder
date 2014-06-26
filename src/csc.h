#ifndef CSC_H
#define CSC_H

#include "encoder.h"
#include "main.h"

struct csc_ctx_s
{
	int width, height;

	VADisplay   va_dpy;
	VASurfaceID rgb32_surface;
	VAContextID vpp_ctx;
	VABufferID  vpp_pipeline_buf;
};

int csc_alloc(struct csc_ctx_s *ctx,
	VADisplay va_dpy, VAConfigID cfg, VAContextID ctxid,
	int width, int height);

int csc_free(struct csc_ctx_s *ctx);

VAStatus csc_convert_rgbdata_to_yuv(struct csc_ctx_s *ctx,
	unsigned char *data, VASurfaceID yuv_surface_output);

#endif

