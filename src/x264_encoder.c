#include "encoder.h"

static int x264_init(struct encoder_params_s *params)
{
	assert(params);
	printf("%s(%d, %d)\n", __func__, params->width, params->height);

	if ((params->width != 720) && (params->width % 32)) {
		printf("Width(%d) must be an exact multiple of 32 pixels\n", params->width);
		return -1;
	}
	if (params->height % 16) {
		printf("Height(%d) must be an exact multiple of 16 pixels\n", params->height);
		return -1;
	}

	switch (params->input_fourcc) {
	case E_FOURCC_YUY2:
	case E_FOURCC_BGRX:
		break;
	default:
		return -1;
	}

	/* store coded data into a file */
	if (params->encoder_nalOutputFilename) {
		params->nal_fp = fopen(params->encoder_nalOutputFilename, "w+");
		if (params->nal_fp == NULL) {
			printf("Open file %s failed, exit\n", params->encoder_nalOutputFilename);
			exit(1);
		}
	}

	encoder_print_input(params);

	struct x264_vars_s *x264_vars = &params->x264_vars;
	x264_param_t *x264Param = &params->x264_vars.x264_params;
	x264_param_default_preset(x264Param, "veryfast", "zerolatency");
	x264Param->i_threads = 1;
	x264Param->i_width = params->width;
	x264Param->i_height = params->height;
	x264Param->i_fps_num = params->frame_rate;
	x264Param->i_fps_den = 1;
	x264Param->i_keyint_max = params->frame_rate;
	x264Param->b_intra_refresh = 1;
	x264Param->rc.i_rc_method = X264_RC_CRF;
	x264Param->rc.f_rf_constant = 25;
	x264Param->rc.f_rf_constant_max = 35;
	x264Param->b_repeat_headers = 1;
	x264Param->b_annexb = 1;
	x264_param_apply_profile(x264Param, "baseline");

	/* Setup the encoder */
	x264_vars->encoder = x264_encoder_open(x264Param);
	x264_picture_alloc(&x264_vars->pic_in, X264_CSP_I420, params->width, params->height);
	printf("i_csp = %x\n", x264_vars->pic_in.img.i_csp);
	printf("i_plane = %d\n", x264_vars->pic_in.img.i_plane);
	x264_vars->img = &x264_vars->pic_in.img;
	for (int i = 0; i < x264_vars->img->i_plane; i++) {
		printf("stride[%d] = %d plane = %p\n", i,
			x264_vars->img->i_stride[i],
			x264_vars->img->plane[i]);
	}

	return 0;
}

static void x264_close(struct encoder_params_s *params)
{
        x264_picture_clean(&params->x264_vars.pic_in);
        x264_encoder_close(params->x264_vars.encoder);
}

static void x264_set_defaults(struct encoder_params_s *p)
{
	encoder_set_defaults(p);
	p->type = EM_X264;
}

static int x264_encode_frame(struct encoder_params_s *params, unsigned char *inbuf)
{
	if ((!params) || (!inbuf))
		return 0;

	switch (params->input_fourcc) {
	case E_FOURCC_YUY2:
	case E_FOURCC_BGRX:
		break;
	default:
		printf("Fatal, unsupported FOURCC\n");
		exit(1);
	}

	if (IS_YUY2(params) && params->enable_osd) {
		/* Warning: We're going to directly modify the input pixels. In fixed
		 * frame encoding we'll continuiously overwrite and alter the static
		 * image. If for any reason our OSD strings below begin to shorten,
		 * we'll leave old pixel data in the source image.
		 * This is intensional and saves an additional frame copy.
		 */
		encoder_display_render_reset(&params->display_ctx, inbuf, params->width * 2);

		/* Render any OSD */
		char str[256];
		time_t now = time(NULL);
		struct tm *tm = localtime(&now);
		sprintf(str, "%04d/%02d/%02d-%02d:%02d:%02d",
			tm->tm_year + 1900,
			tm->tm_mon + 1,
			tm->tm_mday,
			tm->tm_hour,
			tm->tm_min,
			tm->tm_sec
			);
		encoder_display_render_string(&params->display_ctx, (unsigned char*)str, strlen(str), 0, 10);

		sprintf(str, "FRM: %lld", params->frames_processed);
		encoder_display_render_string(&params->display_ctx, (unsigned char*)str, strlen(str), 0, 11);
	}

	struct x264_vars_s *x264_vars = &params->x264_vars;

	if (IS_YUY2(params)) {
		/* Convert YUY2 to I420. */
		YUY2ToI420(inbuf, params->width * 2,
			x264_vars->img->plane[0], x264_vars->img->i_stride[0],
			x264_vars->img->plane[1], x264_vars->img->i_stride[1],
			x264_vars->img->plane[2], x264_vars->img->i_stride[2],
			params->width, params->height);
	} else
	if (IS_BGRX(params)) {
		/* Convert BGRA to I420. */
		BGRAToI420(inbuf, params->width * 4,
			x264_vars->img->plane[0], x264_vars->img->i_stride[0],
			x264_vars->img->plane[1], x264_vars->img->i_stride[1],
			x264_vars->img->plane[2], x264_vars->img->i_stride[2],
			params->width, params->height);
	}

	/* Encode image */
	x264_nal_t *nals = 0;
	int i_nals = 0;
	int frame_size = x264_encoder_encode(x264_vars->encoder, &nals, &i_nals,
		&x264_vars->pic_in, &x264_vars->pic_out);
	if (frame_size < 0) {
		printf("encoder failed = %d\n", frame_size);
		return 0;
	}

	x264_vars->nalcount += i_nals;
	x264_vars->bytecount += frame_size;
	printf("nals = %lld bytes = %lld\n", x264_vars->nalcount, x264_vars->bytecount);
	for (int i = 0; i < i_nals; i++) {
		x264_nal_t *nal = nals + i;
		encoder_output_codeddata(params, nal->p_payload, nal->i_payload, 0);
	}

	params->frames_processed++;
	return 1;
}

struct encoder_operations_s x264_ops = 
{
	.type		= EM_X264,
        .name		= "libx264 Encoder",
        .init		= x264_init,
        .set_defaults	= x264_set_defaults,
        .close		= x264_close,
        .encode_frame	= x264_encode_frame,
};
