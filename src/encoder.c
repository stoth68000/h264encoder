#include "encoder.h"

#define MEASURE_PERFORMANCE 0

int encoder_init(struct encoder_operations_s *ops, struct encoder_params_s *params)
{
	assert(ops);
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

	if (encoder_isSupportedColorspace(params, params->input_fourcc) == 0) {
		printf("Fatal, unsupported FOURCC (%x)\n", params->input_fourcc);
		exit(1);
	}

	/* store coded data into a file */
	encoder_create_nal_outfile(params);
	encoder_print_input(params);

	int ret = ops->init(params);

	return ret;
}

static void _set_defaults(struct encoder_params_s *p)
{
	memset(p, 0, sizeof(*p));
	p->initial_qp = 26;
	p->minimal_qp = 0;
	p->enable_osd = 0;
	p->intra_period = 30;
	p->intra_idr_period = 60;
	p->ip_period = 1;
	p->h264_profile = VAProfileH264High;
	p->level_idc = 41;
	p->h264_entropy_mode = 1;
	p->rc_mode = VA_RC_VBR;
	p->frame_bitrate = 3000000;
	p->hrd_bitrate_multiplier = 16;
	p->input_fourcc = E_FOURCC_UNDEFINED;
	p->frame_rate = 30;
	p->frame_count = 60;
	p->quiet_encode = 0;

	encoder_display_init(&p->display_ctx);
}

int encoder_set_defaults(struct encoder_operations_s *ops, struct encoder_params_s *params)
{
	assert(ops);
	assert(params);

        _set_defaults(params);
	params->type = ops->type;

	return ops->set_defaults(params);
}

void encoder_close(struct encoder_operations_s *ops, struct encoder_params_s *params)
{
	assert(ops);
	assert(params);

	ops->close(params);
}

/* Core func, all capture sources call us, we call the ops encode frame func and
 * handle param validation, performance measurements etc.
 */
int encoder_encode_frame(struct encoder_operations_s *ops, struct encoder_params_s *params, unsigned char *inbuf)
{
	assert(ops);
	assert(params);
	assert(inbuf);

	if (encoder_isSupportedColorspace(params, params->input_fourcc) == 0) {
		printf("Fatal, unsupported FOURCC\n");
		exit(1);
	}

	/* Etch into the frame the OSD stats before encoding, if required */
	encoder_frame_add_osd(params, inbuf);

#if MEASURE_PERFORMANCE
	unsigned int elapsedMS;
	struct timeval now;
	gettimeofday(&now, 0);
#endif

	int ret = ops->encode_frame(params, inbuf);

#if MEASURE_PERFORMANCE
	elapsedMS = encoder_measureElapsedMS(&now);
	printf("%s() frame encode took %dms\n", __func__, elapsedMS);
#endif

	/* Progress/visual indicator */
	encoder_output_console_progress(params);

	/* Update encoder core statistics */
	encoder_frame_ingested(params);

	return ret;
}

int encoder_isSupportedColorspace(struct encoder_params_s *params, enum fourcc_e csc)
{
	struct encoder_operations_s *ops = getEncoderTarget(params->type);
	assert(ops);

	int i = 0;
	while (ops->supportedColorspaces[i] != 0) {
		if (ops->supportedColorspaces[i] == csc)
			return 1;
		i++;
	}

	return 0;
}

//struct timeval now;
//gettimeofday(&now, 0);
unsigned int encoder_measureElapsedMS(struct timeval *then)
{
	struct timeval now;
	gettimeofday(&now, 0);

	unsigned int elapsedTime = (now.tv_sec - then->tv_sec) * 1000.0; /* sec to ms */
	elapsedTime += (now.tv_usec - then->tv_usec) / 1000.0;  /* us to ms */

	return elapsedTime;
}

void encoder_output_console_progress(struct encoder_params_s *params)
{
	if (!params->quiet_encode) {
		printf("\r      ");	/* return back to startpoint */
		switch (params->frames_processed % 4) {
		case 0:
			printf("|");
			break;
		case 1:
			printf("/");
			break;
		case 2:
			printf("-");
			break;
		case 3:
			printf("\\");
			break;
		}
		printf(" %lld (coded)", params->coded_size);
	}
}

void encoder_frame_add_osd(struct encoder_params_s *params, unsigned char *frame)
{
	if (IS_YUY2(params) && params->enable_osd) {
		/* Warning: We're going to directly modify the input pixels. In fixed
		 * frame encoding we'll continuiously overwrite and alter the static
		 * image. If for any reason our OSD strings below begin to shorten,
		 * we'll leave old pixel data in the source image.
		 * This is intensional and saves an additional frame copy.
		 */
		encoder_display_render_reset(&params->display_ctx, frame, params->width * 2);

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
}

int encoder_frame_ingested(struct encoder_params_s *params)
{
	params->frames_processed++;
	return 1;
}

int encoder_create_nal_outfile(struct encoder_params_s *params)
{
	/* store coded data into a file */
	if (params->encoder_nalOutputFilename) {
		params->nal_fp = fopen(params->encoder_nalOutputFilename, "w+");
		if (params->nal_fp == NULL) {
			printf("Open file %s failed, exit\n", params->encoder_nalOutputFilename);
			exit(1);
		}
	}
	return 0;
}

int encoder_output_codeddata(struct encoder_params_s *params, unsigned char *buf, int size, int isIFrame)
{
	int s;

	if (params->nal_fp) {
		s = fwrite(buf, 1, size, params->nal_fp);
		fflush(params->nal_fp);
	} else
		s = size;

/* TODO: Urgh, hardcoded list of callbacks. Put them in a list
 * and enumerate and call.
 */
	/* ... will drop the packet if ES2TS was not requested */
	sendESPacket(buf, size, isIFrame);

	/* ... will drop the packet if RTP was not requested */
	sendRTPPacket(buf, size, isIFrame);

	/* ... will drop the packet if MXC_VPU_UDP was not requested */
	sendMXCVPUUDPPacket(buf, size, isIFrame);

	params->coded_size += s;
	return s;
}

void encoder_print_input(struct encoder_params_s *params)
{
	printf("\n\nINPUT:Try to encode H264...\n");
	printf("INPUT: RateControl  : %s\n", encoder_rc_to_string(params->rc_mode));
	printf("INPUT: Resolution   : %dx%d, %d frames\n",
	       params->width, params->height, params->frame_count);
	printf("INPUT: FrameRate    : %d\n", params->frame_rate);
	printf("INPUT: Bitrate      : %d\n", params->frame_bitrate);
	printf("INPUT: IntraPeriod  : %d\n", params->intra_period);
	printf("INPUT: IDRPeriod    : %d\n", params->intra_idr_period);
	printf("INPUT: IpPeriod     : %d\n", params->ip_period);
	printf("INPUT: Initial QP   : %d\n", params->initial_qp);
	printf("INPUT: Min QP       : %d\n", params->minimal_qp);
	printf("INPUT: Level IDC    : %d\n", params->level_idc);
	printf("INPUT: Coded Clip   : %s\n", params->encoder_nalOutputFilename ?
		params->encoder_nalOutputFilename : "N/A");
	printf("INPUT: HRD BR/Multi : %d\n", params->hrd_bitrate_multiplier);
	printf("\n\n");		/* return back to startpoint */
}

extern struct encoder_operations_s vaapi_ops;
extern struct encoder_operations_s x264_ops;
extern struct encoder_operations_s lavc_ops;

static struct encoder_module_s {
	unsigned int type;
	struct encoder_operations_s *operations;
} encoder_modules[] = {
	{ EM_VAAPI,		&vaapi_ops },
	{ EM_X264,		&x264_ops },
#if 0
	{ EM_AVCODEC_H264,	&lavc_ops },
#endif
};

struct encoder_operations_s *getEncoderTarget(unsigned int type)
{
	for (unsigned int i = 0; i < (sizeof(encoder_modules) / sizeof(struct encoder_module_s)); i++) {
		if (type == encoder_modules[i].type)
			return encoder_modules[i].operations;
	}

	return 0;
}

