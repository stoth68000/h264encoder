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

#ifndef ENCODER_H
#define ENCODER_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <va/va_enc_h264.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <x264.h>
#include <libyuv.h>

#include "es2ts.h"
#include "rtp.h"
#include "mxcvpuudp.h"
#include "csc.h"
#include "va_display.h"
#include "encoder-display.h"
#include "main.h"
#include "frames.h"

#include "encoder-display.h"
#include "frames.h"

#define IS_YUY2(p) ((p)->input_fourcc == E_FOURCC_YUY2)
#define IS_BGRX(p) ((p)->input_fourcc == E_FOURCC_BGRX)
#define IS_I420(p) ((p)->input_fourcc == E_FOURCC_I420)

enum fourcc_e {
	E_FOURCC_UNDEFINED = 0,
	E_FOURCC_YUY2,
	E_FOURCC_BGRX,
	E_FOURCC_I420,
};

struct lavc_vars_s {
	AVCodec *codec;
	AVCodecContext *codec_ctx;
	AVFrame *picture;
};

struct x264_vars_s {
	x264_param_t x264_params;
	x264_t *encoder;
        x264_picture_t pic_in, pic_out;
	x264_image_t *img;
	unsigned long long nalcount, bytecount;
};

enum encoder_type_e {
	EM_VAAPI = 0,
	EM_AVCODEC_H264,
	EM_X264,
	EM_MAX
};

struct encoder_params_s
{
	enum encoder_type_e type;
	struct encoder_display_context display_ctx;

	/* Nals to disk */
	char *encoder_nalOutputFilename;
	FILE *nal_fp;

	/* Total bytes output by the encoder */
	unsigned long long coded_size;

	unsigned int width;
	unsigned int height;
	unsigned int enable_osd;
	unsigned int deinterlacemode;
	unsigned int initial_qp;
	unsigned int minimal_qp;
	unsigned int frame_rate;

	unsigned int intra_period;
	unsigned int intra_idr_period;
	unsigned int ip_period;
	VAProfile h264_profile;
	unsigned char level_idc;
	unsigned int h264_entropy_mode;
	int rc_mode;
	unsigned int frame_count;

	unsigned int frame_bitrate; /* bps */
	enum fourcc_e input_fourcc;

	unsigned int hrd_bitrate_multiplier;

	/* libavcodec Specific */
	struct lavc_vars_s lavc_vars;

	/* X264 ENCODER */
	struct x264_vars_s x264_vars;

	unsigned long long frames_processed;

	FILE *csv_fp;
	int quiet_encode;

	/* VAAPI Colorspace Conversion */
	struct csc_ctx_s csc_ctx;
};

int   encoder_string_to_rc(char *str);
char *encoder_rc_to_string(int rcmode);
int   encoder_string_to_profile(char *str);
char *encoder_profile_to_string(int profile);

/* Frame encoders (video) implement these
 * callbacks in their <name>_encoder.c implementation.
 * ONLY the encoder.c core implementation is entitled
 * to query or call these directly.
 * all access to encoders is done through the encoder
 * core encoder_*() calls.
 */
struct encoder_operations_s
{
        unsigned int type;
        char *name;

	enum fourcc_e *supportedColorspaces;

	int  (*init)(struct encoder_params_s *);
	int  (*set_defaults)(struct encoder_params_s *);
	void (*close)(struct encoder_params_s *);
	int  (*encode_frame)(struct encoder_params_s *, unsigned char *);
};

extern struct encoder_operations_s vaapi_ops;

struct encoder_operations_s *getEncoderTarget(unsigned int type);

void encoder_print_input(struct encoder_params_s *p);
int  encoder_output_codeddata(struct encoder_params_s *params, unsigned char *buf, int size, int isIFrame);
int  encoder_create_nal_outfile(struct encoder_params_s *params);
int  encoder_frame_ingested(struct encoder_params_s *params);
void encoder_frame_add_osd(struct encoder_params_s *params, unsigned char *frame);
void encoder_output_console_progress(struct encoder_params_s *params);
int  encoder_pre_encode_checks(struct encoder_params_s *params);
unsigned int encoder_measureElapsedMS(struct timeval *then);
int  encoder_isSupportedColorspace(struct encoder_params_s *params, enum fourcc_e csc);

int  encoder_init(struct encoder_operations_s *ops, struct encoder_params_s *params);
int  encoder_set_defaults(struct encoder_operations_s *ops, struct encoder_params_s *p);
int  encoder_encode_frame(struct encoder_operations_s *ops, struct encoder_params_s *params, unsigned char *inbuf);
void encoder_close(struct encoder_operations_s *ops, struct encoder_params_s *params);

#endif
