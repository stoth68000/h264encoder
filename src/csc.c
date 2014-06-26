#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <string.h>
#include "csc.h"
#include "encoder.h"

static void download_surface(struct csc_ctx_s *ctx, VASurfaceID surface_id)
{
	VAImage image;
	VAStatus va_status;
	void *pbuffer = NULL;

	va_status = vaDeriveImage(ctx->va_dpy, surface_id, &image);
	CHECK_VASTATUS(va_status, "vaDeriveImage");
	va_status = vaMapBuffer(ctx->va_dpy, image.buf, &pbuffer);
	CHECK_VASTATUS(va_status, "vaMapBuffer");

	unsigned char *c = pbuffer;
	for (int i = 0; i < 16; i++)
		printf("%02x", *(c + i));
	printf("\n");
	
	va_status = vaUnmapBuffer(ctx->va_dpy, image.buf);
	CHECK_VASTATUS(va_status, "vaUnmapBuffer");
	va_status = vaDestroyImage(ctx->va_dpy, image.image_id);
	CHECK_VASTATUS(va_status, "vaDestroyImage");
}

/* Map a surface, shift the inbuf pixels into it */
static void upload_rgb_to_surface(struct csc_ctx_s *ctx, VASurfaceID surface_id)
{
	VAImage image;
	VAStatus va_status;
	void *pbuffer = NULL;
	
	va_status = vaDeriveImage(ctx->va_dpy, surface_id, &image);
	CHECK_VASTATUS(va_status, "vaDeriveImage");
	va_status = vaMapBuffer(ctx->va_dpy, image.buf, &pbuffer);
	CHECK_VASTATUS(va_status, "vaMapBuffer");

	memcpy(pbuffer, ctx->rgb32_buffer, ctx->height * ctx->stride);

	va_status = vaUnmapBuffer(ctx->va_dpy, image.buf);
	CHECK_VASTATUS(va_status, "vaUnmapBuffer");
	va_status = vaDestroyImage(ctx->va_dpy, image.image_id);
	CHECK_VASTATUS(va_status, "vaDestroyImage");
}

unsigned char v = 0;
static void upload_counter_to_surface(struct csc_ctx_s *ctx, VASurfaceID surface_id)
{
	VAImage image;
	VAStatus va_status;
	void *pbuffer = NULL;
	
	va_status = vaDeriveImage(ctx->va_dpy, surface_id, &image);
	CHECK_VASTATUS(va_status, "vaDeriveImage");
	va_status = vaMapBuffer(ctx->va_dpy, image.buf, &pbuffer);
	CHECK_VASTATUS(va_status, "vaMapBuffer");

	memset(pbuffer, v++, 16);

	va_status = vaUnmapBuffer(ctx->va_dpy, image.buf);
	CHECK_VASTATUS(va_status, "vaUnmapBuffer");
	va_status = vaDestroyImage(ctx->va_dpy, image.image_id);
	CHECK_VASTATUS(va_status, "vaDestroyImage");
}

#if 0
unsigned char cscval = 0x00;
static void update_rgb_surface(struct csc_ctx_s *ctx)
{
	VAImage image;
	VAStatus va_status;
	void *pbuffer = NULL;
	
	va_status = vaDeriveImage(ctx->va_dpy, ctx->rgb32_surface, &image);
	CHECK_VASTATUS(va_status, "vaDeriveImage");
	va_status = vaMapBuffer(ctx->va_dpy, image.buf, &pbuffer);
	CHECK_VASTATUS(va_status, "vaMapBuffer");

	memset(pbuffer, cscval++, image.width * image.height);

	va_status = vaUnmapBuffer(ctx->va_dpy, image.buf);
	CHECK_VASTATUS(va_status, "vaUnmapBuffer");
	va_status = vaDestroyImage(ctx->va_dpy, image.image_id);
	CHECK_VASTATUS(va_status, "vaDestroyImage");
}
#endif

static VAStatus create_surface_RGB(struct csc_ctx_s *ctx)
{
	VAStatus status;
        VASurfaceAttrib va_attribs[1];
        va_attribs[0].type = VASurfaceAttribPixelFormat;
        va_attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        va_attribs[0].value.type = VAGenericValueTypeInteger;
        va_attribs[0].value.value.i = VA_FOURCC_BGRX;

        status = vaCreateSurfaces(ctx->va_dpy, VA_RT_FORMAT_RGB32,
		ctx->width, ctx->height, &ctx->rgb32_surface, 1, &va_attribs[0], 1);
	upload_rgb_to_surface(ctx, ctx->rgb32_surface);
	return status;
}

#if 0
static VAStatus create_surface_RGB2(struct csc_ctx_s *ctx)
{
#if 0
	VAStatus status;
        /* create source surfaces */
        status = vaCreateSurfaces(ctx->va_dpy, VA_RT_FORMAT_RGB32, ctx->width, ctx->height, &ctx->rgb32_surface, 1, NULL, 0);
	upload_rgb_to_surface(ctx, ctx->rgb32_surface);
#else
	VASurfaceAttrib va_attribs[2];
	VASurfaceAttribExternalBuffers va_attrib_extbuf;
	VAStatus status;

	va_attrib_extbuf.pixel_format = VA_FOURCC_BGRX;
	va_attrib_extbuf.width = ctx->width;
	va_attrib_extbuf.height = ctx->height;
	va_attrib_extbuf.data_size = ctx->height * ctx->stride;
	va_attrib_extbuf.num_planes = 1;
	va_attrib_extbuf.pitches[0] = ctx->stride;
	va_attrib_extbuf.pitches[0] = 0;
	va_attrib_extbuf.offsets[0] = 0;
	va_attrib_extbuf.buffers = ctx->rgb32_buffer;
	va_attrib_extbuf.num_buffers = 1;
	va_attrib_extbuf.flags = 0;
	va_attrib_extbuf.private_data = NULL;

	int i = 0;
#if 0
	va_attribs[i].type = VASurfaceAttribMemoryType;
	va_attribs[i].flags = VA_SURFACE_ATTRIB_SETTABLE;
	va_attribs[i].value.type = VAGenericValueTypeInteger;
	va_attribs[i].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
	i++;
#endif
	va_attribs[i].type = VASurfaceAttribExternalBufferDescriptor;
	va_attribs[i].flags = VA_SURFACE_ATTRIB_SETTABLE;
	va_attribs[i].value.type = VAGenericValueTypePointer;
	va_attribs[i].value.value.p = &va_attrib_extbuf;

	status = vaCreateSurfaces(ctx->va_dpy, VA_RT_FORMAT_RGB32,
				  ctx->width, ctx->height, &ctx->rgb32_surface, 1,
				  va_attribs, i);
	upload_rgb_to_surface(ctx, ctx->rgb32_surface);
#endif
	return status;
}
#endif

/* Convert ctx->rgb32_surface to ctx->vpp.output (yuv) */
VAStatus csc_convert_rgbdata_to_yuv(struct csc_ctx_s *ctx, unsigned char *data, VASurfaceID yuv_surface_output)
{
	VAProcPipelineParameterBuffer *pipeline_param;
	VAStatus status;

	{
		VAImage image;
		VAStatus va_status;
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

	status = vaMapBuffer(ctx->va_dpy, ctx->vpp.pipeline_buf, (void **)&pipeline_param);
	if (status != VA_STATUS_SUCCESS) {
printf("1\n");
		return status;
	}

	memset(pipeline_param, 0, sizeof *pipeline_param);

	pipeline_param->surface = ctx->rgb32_surface;
	pipeline_param->surface_color_standard  = VAProcColorStandardNone;

	pipeline_param->output_background_color = 0xff000000;
	pipeline_param->output_color_standard   = VAProcColorStandardNone;

	status = vaUnmapBuffer(ctx->va_dpy, ctx->vpp.pipeline_buf);
	if (status != VA_STATUS_SUCCESS) {
printf("2\n");
		return status;
	}

	status = vaBeginPicture(ctx->va_dpy, ctx->vpp.ctx, yuv_surface_output);
	if (status != VA_STATUS_SUCCESS) {
printf("3\n");
		return status;
	}

	status = vaRenderPicture(ctx->va_dpy, ctx->vpp.ctx, &ctx->vpp.pipeline_buf, 1);
	if (status != VA_STATUS_SUCCESS) {
printf("4\n");
		return status;
	}

	status = vaEndPicture(ctx->va_dpy, ctx->vpp.ctx);
	if (status != VA_STATUS_SUCCESS) {
printf("5\n");
		return status;
	}

	//download_surface(ctx, yuv_surface_output);
	return status;
}

/* Convert ctx->rgb32_surface to ctx->vpp.output (yuv) */
VAStatus csc_convert_rgb_to_yuv(struct csc_ctx_s *ctx, VASurfaceID yuv_surface_output)
{
	VAProcPipelineParameterBuffer *pipeline_param;
	VAStatus status;

	//update_rgb_surface(ctx);
	upload_counter_to_surface(ctx, yuv_surface_output);
	download_surface(ctx, yuv_surface_output);

	status = vaMapBuffer(ctx->va_dpy, ctx->vpp.pipeline_buf, (void **)&pipeline_param);
	if (status != VA_STATUS_SUCCESS) {
printf("1\n");
		return status;
	}

	memset(pipeline_param, 0, sizeof *pipeline_param);

	pipeline_param->surface = ctx->rgb32_surface;
	pipeline_param->surface_color_standard  = VAProcColorStandardNone;

	pipeline_param->output_background_color = 0xff000000;
	pipeline_param->output_color_standard   = VAProcColorStandardNone;

	status = vaUnmapBuffer(ctx->va_dpy, ctx->vpp.pipeline_buf);
	if (status != VA_STATUS_SUCCESS) {
printf("2\n");
		return status;
	}

	status = vaBeginPicture(ctx->va_dpy, ctx->vpp.ctx, yuv_surface_output);
	if (status != VA_STATUS_SUCCESS) {
printf("3\n");
		return status;
	}

	status = vaRenderPicture(ctx->va_dpy, ctx->vpp.ctx, &ctx->vpp.pipeline_buf, 1);
	if (status != VA_STATUS_SUCCESS) {
printf("4\n");
		return status;
	}

	status = vaEndPicture(ctx->va_dpy, ctx->vpp.ctx);
	if (status != VA_STATUS_SUCCESS) {
printf("5\n");
		return status;
	}

	download_surface(ctx, yuv_surface_output);
	return status;
}
int csc_alloc(struct csc_ctx_s *ctx,
	VADisplay va_dpy, VAConfigID cfg, VAContextID ctxid,
	int width, int height, int stride)
{
	VAStatus status;

	printf("%s(cfgid = %x, ctxid = %x, %d,%d,%d)\n",
		__func__,
		cfg, ctxid, width, height, stride);
	memset(ctx, 0, sizeof(*ctx));
	ctx->va_dpy = va_dpy;
	ctx->vpp.cfg = cfg;
	ctx->vpp.ctx = ctxid;
	ctx->width = width;
	ctx->height = height;
	ctx->stride = stride;

	/* Create a fake RGB frame */
	ctx->rgb32_buffer = malloc(stride * height);
	unsigned int *d = (unsigned int *)ctx->rgb32_buffer;
	for (int i = 0; i < height; i++) {
		for (int j = 0; j < width; j += 4)
			*(d++) = 0xFFFF0000;
			//       0xXXRRGGBB
	}

	status = vaCreateBuffer(ctx->va_dpy, ctx->vpp.ctx,
				VAProcPipelineParameterBufferType,
				sizeof(VAProcPipelineParameterBuffer),
				1, NULL, &ctx->vpp.pipeline_buf);
	if (status != VA_STATUS_SUCCESS) {
		printf("failed to create VPP pipeline buffer\n");
		return -1;
	}

	create_surface_RGB(ctx);

	return 0;
}

int csc_free(struct csc_ctx_s *ctx)
{
	if (ctx->rgb32_surface)
		vaDestroySurfaces(ctx->va_dpy, &ctx->rgb32_surface, 1);

//	if (ctx->rgb32_buffer)
//		free(ctx->rgb32_buffer);

	vaDestroyBuffer(ctx->va_dpy, ctx->vpp.pipeline_buf);

	memset(ctx, 0, sizeof(*ctx));

	return 0;
}

