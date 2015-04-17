#include "encoder.h"

static int x264_init(struct encoder_params_s *params)
{
	printf("%s()\n", __func__);

	if (params->deinterlacemode) {
		printf("%s() deinterlacemode not suppported\n", __func__);
		exit(1);
	}

	struct x264_vars_s *x264_vars = &params->x264_vars;
	x264_param_t *x264Param = &params->x264_vars.x264_params;

	x264_param_default_preset(x264Param, "ultrafast", "zerolatency");
	x264Param->i_threads = 1; /* In testing, increasing this for gltest had a negative effect */
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
	x264_vars->img = &x264_vars->pic_in.img;
#if 0
	printf("i_csp = %x\n", x264_vars->pic_in.img.i_csp);
	printf("i_plane = %d\n", x264_vars->pic_in.img.i_plane);
	for (int i = 0; i < x264_vars->img->i_plane; i++) {
		printf("stride[%d] = %d plane = %p\n", i,
			x264_vars->img->i_stride[i],
			x264_vars->img->plane[i]);
	}
#endif

	return 0;
}

static void x264_close(struct encoder_params_s *params)
{
        x264_picture_clean(&params->x264_vars.pic_in);
        x264_encoder_close(params->x264_vars.encoder);
}

static int x264_set_defaults(struct encoder_params_s *p)
{
	/* If required */
	return 0;
}

static int x264_encode_frame(struct encoder_params_s *params, unsigned char *inbuf)
{
	/* Colorspace convert the frame and encode it */
	struct x264_vars_s *x264_vars = &params->x264_vars;

	/* We redirect the picture image plane pointers to reference
	 * our incoming buffer, saving a memcpy. We put those pointers
	 * back later, so to avoid a x264 free'ing related issue and
	 * ensure proper memory handling.
	 */
	unsigned char *x = x264_vars->img->plane[0];
	unsigned char *y = x264_vars->img->plane[1];
	unsigned char *z = x264_vars->img->plane[2];

	if (IS_YUY2(params)) {
		/* Convert YUY2 to I420. */
		YUY2ToI420(inbuf, params->width * 2,
			x264_vars->img->plane[0], x264_vars->img->i_stride[0],
			x264_vars->img->plane[1], x264_vars->img->i_stride[1],
			x264_vars->img->plane[2], x264_vars->img->i_stride[2],
			params->width, params->height);
	} else
	if (IS_BGRX(params)) {
		/* Convert ARGB to I420. */
		ARGBToI420(inbuf, params->width * 4,
			x264_vars->img->plane[0], x264_vars->img->i_stride[0],
			x264_vars->img->plane[1], x264_vars->img->i_stride[1],
			x264_vars->img->plane[2], x264_vars->img->i_stride[2],
			params->width, params->height);
	} else
	if (IS_I420(params)) {
		unsigned char *a = inbuf;
		unsigned char *b = a + (params->width * params->height);
		unsigned char *c = b + (params->width * params->height / 4);

		x264_vars->img->plane[0] = a;
		x264_vars->img->plane[1] = b;
		x264_vars->img->plane[2] = c;
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
#if 0
	printf("nals = %lld bytes = %lld time = %dms(%d)\n",
		x264_vars->nalcount, x264_vars->bytecount, elapsedMS,
		frame_size);
#endif
	for (int i = 0; i < i_nals; i++) {
		x264_nal_t *nal = nals + i;
		encoder_output_codeddata(params, nal->p_payload, nal->i_payload, 0);
	}

	x264_vars->img->plane[0] = x;
	x264_vars->img->plane[1] = y;
	x264_vars->img->plane[2] = z;

	return 1;
}

static enum fourcc_e supportedColorspaces[] = {
        E_FOURCC_YUY2,
        E_FOURCC_BGRX,
        E_FOURCC_I420,
        0, /* terminator */
};

struct encoder_operations_s x264_ops = 
{
	.type		= EM_X264,
        .name		= "libx264 Encoder",
	.supportedColorspaces = &supportedColorspaces[0],
        .init		= x264_init,
        .set_defaults	= x264_set_defaults,
        .close		= x264_close,
        .encode_frame	= x264_encode_frame,
};
