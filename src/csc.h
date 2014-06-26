#ifndef CSC_H
#define CSC_H

#include "encoder.h"
#include "main.h"

struct csc_ctx_s
{
	int width, height, stride;

	VADisplay      va_dpy;
	VASurfaceID    rgb32_surface;
	unsigned char *rgb32_buffer;

	/* video post processing is used for colorspace conversion */
	struct {
		VAConfigID cfg;
		VAContextID ctx;
		VABufferID pipeline_buf;
//		VASurfaceID output;
	} vpp;

#if 0
	struct {
		VAConfigID cfg;
		VAContextID ctx;
		VASurfaceID reference_picture[3];

		int intra_period;
		int output_size;
		int constraint_set_flag;
		struct {
			VAEncSequenceParameterBufferH264 seq;
			VAEncPictureParameterBufferH264 pic;
			VAEncSliceParameterBufferH264 slice;
		} param;
	} encoder;
#endif
};

/* Convert ctx->rgb32_surface to ctx->vpp.output (yuv) */
VAStatus csc_convert_rgb_to_yuv(struct csc_ctx_s *ctx, VASurfaceID yuv_surface_output);
int csc_alloc(struct csc_ctx_s *ctx,
	VADisplay va_dpy, VAConfigID cfg, VAContextID ctxid,
	int width, int height, int stride);
int csc_free(struct csc_ctx_s *ctx);
VAStatus csc_convert_rgbdata_to_yuv(struct csc_ctx_s *ctx, unsigned char *data, VASurfaceID yuv_surface_output);


#endif

