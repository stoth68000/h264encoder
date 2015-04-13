#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <string.h>
#include "csc.h"
#include "encoder.h"

#define CHECK_VASTATUS(va_status,func)                                  \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr,"%s:%s (%d) failed, exit %d\n", __func__, func, __LINE__, va_status); \
        exit(1);                                                        \
    }

VAStatus csc_convert_rgbdata_to_yuv(struct csc_ctx_s *ctx, unsigned char *data, VASurfaceID yuv_surface_output)
{
	VAProcPipelineParameterBuffer *pipeline_param;
	VAStatus va_status;

	{
		VAImage image;
		void *pbuffer = NULL;
	
		va_status = vaDeriveImage(ctx->va_dpy, ctx->rgb32_surface, &image);
		CHECK_VASTATUS(va_status, "vaDeriveImage");
		va_status = vaMapBuffer(ctx->va_dpy, image.buf, &pbuffer);
		CHECK_VASTATUS(va_status, "vaMapBuffer");

		memcpy(pbuffer, data, image.width * 4 * image.height);

		va_status = vaUnmapBuffer(ctx->va_dpy, image.buf);
		CHECK_VASTATUS(va_status, "vaUnmapBuffer");
		va_status = vaDestroyImage(ctx->va_dpy, image.image_id);
		CHECK_VASTATUS(va_status, "vaDestroyImage");
	}

	va_status = vaMapBuffer(ctx->va_dpy, ctx->vpp_pipeline_buf, (void **)&pipeline_param);
	CHECK_VASTATUS(va_status, "vaMapBuffer");

	memset(pipeline_param, 0, sizeof *pipeline_param);

	pipeline_param->surface = ctx->rgb32_surface;
	pipeline_param->surface_color_standard  = VAProcColorStandardNone;

	pipeline_param->output_background_color = 0xff000000;
	pipeline_param->output_color_standard   = VAProcColorStandardNone;

	va_status = vaUnmapBuffer(ctx->va_dpy, ctx->vpp_pipeline_buf);
	CHECK_VASTATUS(va_status, "vaUnmapBuffer");

	va_status = vaBeginPicture(ctx->va_dpy, ctx->vpp_ctx, yuv_surface_output);
	CHECK_VASTATUS(va_status, "vaBeginPicture");

	va_status = vaRenderPicture(ctx->va_dpy, ctx->vpp_ctx, &ctx->vpp_pipeline_buf, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture");

	va_status = vaEndPicture(ctx->va_dpy, ctx->vpp_ctx);
	CHECK_VASTATUS(va_status, "vaEndPicture");

	return va_status;
}

int csc_alloc(struct csc_ctx_s *ctx,
	VADisplay va_dpy, VAConfigID cfg, VAContextID ctxid,
	int width, int height)
{
	VAStatus status;

	memset(ctx, 0, sizeof(*ctx));
	ctx->va_dpy = va_dpy;
	ctx->vpp_ctx = ctxid;

	status = vaCreateBuffer(ctx->va_dpy, ctx->vpp_ctx,
				VAProcPipelineParameterBufferType,
				sizeof(VAProcPipelineParameterBuffer),
				1, NULL, &ctx->vpp_pipeline_buf);
	if (status != VA_STATUS_SUCCESS) {
		printf("failed to create VPP pipeline buffer\n");
		return -1;
	}

        VASurfaceAttrib va_attribs[1];
        va_attribs[0].type = VASurfaceAttribPixelFormat;
        va_attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        va_attribs[0].value.type = VAGenericValueTypeInteger;
        va_attribs[0].value.value.i = VA_FOURCC_BGRX;

        status = vaCreateSurfaces(ctx->va_dpy, VA_RT_FORMAT_RGB32,
		width, height, &ctx->rgb32_surface, 1, &va_attribs[0], 1);

	return 0;
}

int csc_free(struct csc_ctx_s *ctx)
{
	if (ctx->rgb32_surface)
		vaDestroySurfaces(ctx->va_dpy, &ctx->rgb32_surface, 1);

	vaDestroyBuffer(ctx->va_dpy, ctx->vpp_pipeline_buf);

	memset(ctx, 0, sizeof(*ctx));

	return 0;
}

