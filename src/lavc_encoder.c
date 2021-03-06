/*
 *  H264 Encoder - Capture YUV, compress via VA-API and stream to RTP.
 *  Original code base was the vaapi h264encode application, with 
 *  significant additions to support capture, transform, compress
 *  and re-containering via libavformat.
 *
 *  Copyright (c) 2014-2017 Steven Toth <stoth@kernellabs.com>
 *  Copyright (c) 2014-2017 Zodiac Inflight Innovations
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "encoder.h"

#if 0
static int lavc_init(struct encoder_params_s *params)
{
	/* Init and register all available library codecs */
	// http://stackoverflow.com/questions/3553003/encoding-h-264-with-libavcodec-x264
	//avcodec_init();
	avcodec_register_all();

	struct lavc_vars_s *lavc_vars = &params->lavc_vars;
	lavc_vars->codec = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (!lavc_vars->codec) {
		printf("Failed to find codec AV_CODEC_ID_H264\n");
		exit(1);
	}

	lavc_vars->codec_ctx = avcodec_alloc_context3(NULL);
	avcodec_get_context_defaults3(lavc_vars->codec_ctx, lavc_vars->codec);

#if 1
	lavc_vars->codec_ctx->coder_type = 0; // coder = 1
	lavc_vars->codec_ctx->flags|=CODEC_FLAG_LOOP_FILTER; // flags=+loop
	lavc_vars->codec_ctx->me_cmp|= 1; // cmp=+chroma, where CHROMA = 1
//	lavc_vars->codec_ctx->partitions|=X264_PART_I8X8+X264_PART_I4X4+X264_PART_P8X8+X264_PART_B8X8; // partitions=+parti8x8+parti4x4+partp8x8+partb8x8
	lavc_vars->codec_ctx->me_method=ME_HEX; // me_method=hex
	lavc_vars->codec_ctx->me_subpel_quality = 0; // subq=7
	lavc_vars->codec_ctx->me_range = 16; // me_range=16
	lavc_vars->codec_ctx->gop_size = params->frame_rate * 3; // g=250
	lavc_vars->codec_ctx->keyint_min = 30; // keyint_min=25
	lavc_vars->codec_ctx->scenechange_threshold = 40; // sc_threshold=40
	lavc_vars->codec_ctx->i_quant_factor = 0.71; // i_qfactor=0.71
	lavc_vars->codec_ctx->b_frame_strategy = 1; // b_strategy=1
	lavc_vars->codec_ctx->qcompress = 0.6; // qcomp=0.6
	lavc_vars->codec_ctx->qmin = 0; // qmin=10
	lavc_vars->codec_ctx->qmax = 69; // qmax=51
	lavc_vars->codec_ctx->max_qdiff = 4; // qdiff=4
	lavc_vars->codec_ctx->max_b_frames = 3; // bf=3
	lavc_vars->codec_ctx->refs = 3; // refs=3
//	lavc_vars->codec_ctx->directpred = 1; // directpred=1
	lavc_vars->codec_ctx->trellis = 1; // trellis=1
//	lavc_vars->codec_ctx->flags2|=CODEC_FLAG2_FASTPSKIP; // flags2=+bpyramid+mixed_refs+wpred+dct8x8+fastpskip
//	lavc_vars->codec_ctx->weighted_p_pred = 0; // wpredp=2
	lavc_vars->codec_ctx->bit_rate = 32000;
	lavc_vars->codec_ctx->width = params->width;
	lavc_vars->codec_ctx->height = params->height;
	lavc_vars->codec_ctx->time_base.num = 1;
	lavc_vars->codec_ctx->time_base.den = params->frame_rate;
	lavc_vars->codec_ctx->pix_fmt = PIX_FMT_YUV420P; 
	//lavc_vars->codec_ctx->dsp_mask = (FF_MM_MMX | FF_MM_MMXEXT | FF_MM_SSE);
//	lavc_vars->codec_ctx->rc_lookahead = 0;
	lavc_vars->codec_ctx->max_b_frames = 0;
	lavc_vars->codec_ctx->b_frame_strategy =1;
	lavc_vars->codec_ctx->chromaoffset = 0;
	lavc_vars->codec_ctx->thread_count =1;
	lavc_vars->codec_ctx->bit_rate = (int)(128000.f * 0.80f);
	lavc_vars->codec_ctx->bit_rate_tolerance = (int) (128000.f * 0.20f);
	lavc_vars->codec_ctx->gop_size = params->frame_rate * 3; // Each 3 seconds
#else
	lavc_vars->codec_ctx->width = params->width;
	lavc_vars->codec_ctx->height = params->height;
	lavc_vars->codec_ctx->pix_fmt = PIX_FMT_YUV420P; 
#endif

	if (avcodec_open2(lavc_vars->codec_ctx, lavc_vars->codec, NULL) < 0) {
		printf("%s() failed to open\n", __func__);
		exit(1);
	}

	lavc_vars->picture = av_frame_alloc();
	av_frame_unref(lavc_vars->picture);

	int ret = av_image_alloc(lavc_vars->picture->data,
		lavc_vars->picture->linesize,
		params->width, params->height, PIX_FMT_YUV420P, 32);
	if (ret < 0) {
		printf("%s() failed to image alloc\n", __func__);
		exit(1);
	}

	lavc_vars->picture->format = PIX_FMT_YUV420P;
	lavc_vars->picture->width  = params->width;
	lavc_vars->picture->height = params->height;

	return 0;
}

static void lavc_close(struct encoder_params_s *params)
{
	struct lavc_vars_s *lavc_vars = &params->lavc_vars;

	printf("%s()\n", __func__);

	avcodec_close(lavc_vars->codec_ctx);
	av_free(lavc_vars->codec_ctx);
	av_free(lavc_vars->picture);
}

static int lavc_set_defaults(struct encoder_params_s *p)
{
	/* If required */
	return 0;
}

static int lavc_encode_frame(struct encoder_params_s *params, unsigned char *inbuf)
{
	/* Colorspace convert the frame and encode it */
	struct lavc_vars_s *lavc_vars = &params->lavc_vars;

	/* TODO: Colorspace convert the incoming image into the correctly
	 * formatted libav picture frame (AVPacket).
	 */
#if 0
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
		/* TODO: Add support for this colorspace */
	}
#endif

	/* packet buffer will be allocated by the encoder */
	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	int got_output = 0;
	static int pts = 1;
	lavc_vars->picture->pts = pts++;
	int ret = avcodec_encode_video2(lavc_vars->codec_ctx,
		&pkt, lavc_vars->picture, &got_output);
	if (ret < 0) {
		printf("%s() error encoding frame\n", __func__);
		av_free_packet(&pkt);
	}

	if (got_output) {
		/* Deliver the NAL payload */
		encoder_output_codeddata(params, pkt.data, pkt.size, 0);
		av_free_packet(&pkt);
	}

	/* get the delayed frames */
	for (got_output = 1; got_output;) {
		fflush(stdout);

		ret = avcodec_encode_video2(lavc_vars->codec_ctx,
			&pkt, NULL, &got_output);
		if (ret < 0) {
			fprintf(stderr, "error encoding frame\n");
			exit(1);
		}

		if (got_output) {
			/* Deliver the NAL payload */
			encoder_output_codeddata(params, pkt.data, pkt.size, 0);
			av_free_packet(&pkt);
		}
	}

	return 1;
}

static enum fourcc_e supportedColorspaces[] = {
	E_FOURCC_YUY2,
	E_FOURCC_BGRX,
	//E_FOURCC_I420,
	0, /* terminator */
};

struct encoder_operations_s lavc_ops = 
{
	.type		= EM_AVCODEC_H264,
        .name		= "libavcodec H.264 Encoder",
	.supportedColorspaces = &supportedColorspaces[0],
        .init		= lavc_init,
        .set_defaults	= lavc_set_defaults,
        .close		= lavc_close,
        .encode_frame	= lavc_encode_frame,
};
#endif
