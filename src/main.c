#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "rtp.h"
#include "v4l.h"
#include "ipcvideo.h"
#include "fixed.h"
#include "fixed-4k.h"
#include "encoder.h"
#include "es2ts.h"
#include "main.h"

/* main.c */
#define CM_V4L		0
#define CM_IPCVIDEO	1
#define CM_FIXED	2
#define CM_FIXED_4K	3
#define CM_MAX		CM_FIXED_4K

unsigned int capturemode = CM_V4L;
int time_to_quit = 0;

static void signalHandler(int a_Signal)
{
	time_to_quit = 1;
	signal(SIGINT, SIG_DFL);
}

static void usage(int argc, char **argv)
{
	printf("Usage:\n"
	       "\n"
	       "Options:\n"
	       "-h, --help               Print this message\n"
	       "-b, --bitrate=bps        Encoding bitrate [3000000]\n"
	       "-d, --device=NAME        Video device name [/dev/video0]\n"
	       "-o, --output=filename    Record raw nals to output file\n"
	       "-i, --ipaddress=a.b.c.d  Remote IP RTP address\n"
	       "-p, --ipport=9999        Remote IP RTP port\n"
	       "-f, --v4lframerate=FPS   Framerate [no limit]\n"
	       "-n, --v4lnumerator=NUM   Numerator [no limit]\n"
	       "-I, --v4linput=nr        Select video inputnr #N on video device [0]\n"
	       "-W, --dev-width=WIDTH    Device width [720]\n"
	       "-H, --dev-height=HEIGHT  Device height [480]\n"
	       "-M, --mode=NUM           0=v4l 1=ipcvideo 2=fixedframe 3=fixedframe4k\n"
	       "-D, --vppdeinterlace=NUM 0=off 1=motionadaptive 2=bob\n"
	       "-q, --initial_qp=NUM     Initial Quantization Parameter [def: 26]\n"
	       "-Q  --minimal_qp=NUM     Minimum Quantization Parameter [def: 0]\n"
	       );
}

static const char short_options[] = "b:d:i:o:p:hmruD:Pf:n:W:H:M:I:Z:D:q:Q:";

static const struct option long_options[] = {
	{ "bitrate", required_argument, NULL, 'b' },
	{ "device", required_argument, NULL, 'd' },
	{ "ipaddress", required_argument, NULL, 'i' },
	{ "ipport", required_argument, NULL, 'p' },
	{ "help", no_argument, NULL, 'h' },
	{ "mmap", no_argument, NULL, 'm' },
	{ "output", required_argument, NULL, 'o' },
	{ "read", no_argument, NULL, 'r' },
	{ "userp", no_argument, NULL, 'u' },
	{ "progressive", no_argument, NULL, 'P' },
	{ "v4lframerate", required_argument, NULL, 'f' },
	{ "v4lnumerator", required_argument, NULL, 'n' },
	{ "v4linput", required_argument, NULL, 'I' },
	{ "dev-width", required_argument, NULL, 'W' },
	{ "dev-height", required_argument, NULL, 'H' },
	{ "mode", required_argument, NULL, 'M' },
	{ "vppdeinterlace", required_argument, NULL, 'D' },
	{ "initial_qp", required_argument, NULL, 'q' },
	{ "minimal_qp", required_argument, NULL, 'Q' },
	{ 0, 0, 0, 0}
};

int main(int argc, char **argv)
{
	struct encoder_params_s encoder_params;
	char *ipaddress = "192.168.0.67";
	int ipport = 0;
	int videoinputnr = 0;
	v4l_dev_name = (char *)"/dev/video0";
	int req_deint_mode = -1;

	memset(&encoder_params, 0, sizeof(encoder_params));
	encoder_params.initial_qp = 26;
	encoder_params.minimal_qp = 0;
	encoder_params.enable_osd = 0;

	for (;;) {
		int index;
		int c;

		c = getopt_long(argc, argv, short_options, long_options, &index);

		if (-1 == c)
			break;

		switch (c) {
		case 0:	/* getopt_long() flag */
			break;
		case 'b':
			encoder_frame_bitrate = atoi(optarg);
			break;
		case 'd':
			v4l_dev_name = optarg;
			break;
		case 'h':
			usage(argc, argv);
			exit(0);
		case 'i':
			ipaddress = optarg;
			break;
		case 'I':
			videoinputnr = atoi(optarg);
			break;
		case 'm':
			io = IO_METHOD_MMAP;
			break;
		case 'M':
			capturemode = atoi(optarg);
			if (capturemode > CM_MAX) {
				usage(argc, argv);
				exit(0);
			}
			break;
		case 'D':
			req_deint_mode = atoi(optarg);
			if (req_deint_mode > 2) {
				usage(argc, argv);
				exit(0);
			}
			break;
		case 'o':
			encoder_nalOutputFilename = optarg;
			break;
		case 'q':
			encoder_params.initial_qp = atoi(optarg);
			break;
		case 'Q':
			encoder_params.minimal_qp = atoi(optarg);
			break;
		case 'r':
			io = IO_METHOD_READ;
			break;
		case 'u':
			io = IO_METHOD_USERPTR;
			break;
		case 'f':
			g_V4LFrameRate = atoi(optarg);
			if (!g_V4LNumerator)
				g_V4LNumerator = 1;
			break;
		case 'n':
			g_V4LNumerator = atoi(optarg);
			break;
		case 'p':
			ipport = atoi(optarg);
			break;
		case 'W':
			width = atoi(optarg);
			break;
		case 'H':
			height = atoi(optarg);
			break;
		case 'Z':
			encoder_params.enable_osd = atoi(optarg);
			break;
		default:
			usage(argc, argv);
			exit(1);
		}
	}

	if ((capturemode == CM_FIXED) || (capturemode == CM_FIXED_4K))
		encoder_params.enable_osd = 1;

	if (capturemode == CM_V4L) {
		if (req_deint_mode == -1 /* UNSET */)
			encoder_params.deinterlacemode = 2;
		else
			encoder_params.deinterlacemode = req_deint_mode;

		printf("V4L Capture: %dx%d %d/%d [input: %d]\n", width, height,
			g_V4LNumerator, g_V4LFrameRate, videoinputnr);
	}

	if (capturemode == CM_IPCVIDEO)
		printf("IPC Video Capture: %dx%d %d/%d\n", width, height, g_V4LNumerator, g_V4LFrameRate);

	if (capturemode == CM_FIXED)
		printf("Fixed Frame Capture: %d/%d [osd: %s]\n", g_V4LNumerator, g_V4LFrameRate,
			encoder_params.enable_osd ? "Enabled" : "Disabled");

	if (capturemode == CM_FIXED_4K)
		printf("Fixed Frame (4k) Capture: %d/%d [osd: %s]\n", g_V4LNumerator, g_V4LFrameRate,
			encoder_params.enable_osd ? "Enabled" : "Disabled");

	if (signal(SIGINT, signalHandler) == SIG_ERR) {
		printf("signal() failed\n");
		time_to_quit = 1;
	}

	if (capturemode == CM_V4L) {
		open_v4l_device();
		init_v4l_device(videoinputnr);
		if (g_V4LFrameRate == 0)
			g_V4LFrameRate = 60;
	}
	if (capturemode == CM_IPCVIDEO) {
		if (ipcvideo_open_device() < 0) {
			printf("Error: IPCVIDEO pipeline producer is not running\n");
			goto encoder_failed;
		}
		if (g_V4LFrameRate == 0)
			g_V4LFrameRate = 30;

		/* Init the ipc video pipe and extract resolution details */
		ipcvideo_init_device(&width, &height, g_V4LFrameRate);
	}
	if (capturemode == CM_FIXED) {
		fixed_open_device();
		if (g_V4LFrameRate == 0)
			g_V4LFrameRate = 30;
		fixed_init_device(&width, &height, g_V4LFrameRate);
	}
	if (capturemode == CM_FIXED_4K) {
		fixed_4k_open_device();
		if (g_V4LFrameRate == 0)
			g_V4LFrameRate = 30;
		fixed_4k_init_device(&width, &height, g_V4LFrameRate);
	}

	printf("Using frame resolution: %dx%d\n", width, height);

	encoder_params.height = height;
	encoder_params.width = width;
	if (encoder_init(&encoder_params)) {
		printf("Error: Encoder init failed\n");
		goto encoder_failed;
	}

	/* Open the 'nals via rtp' mechanism if requested, but only for the internal GL demo app */
#if 0
	if (
		(capturemode == CM_OPENGL) &&
		ipport && (initRTPHandler(ipaddress, ipport, width, height, g_V4LFrameRate) < 0)) {
		printf("Error: RTP init failed\n");
		goto rtp_failed;
	}
#endif

	/* the NAL/es to TS conversion layer, while routes out via RTP */
	if (((capturemode == CM_V4L) || (capturemode == CM_IPCVIDEO) || (capturemode == CM_FIXED) || (capturemode == CM_FIXED_4K)) &&
		ipport && (initESHandler(ipaddress, ipport, width, height, g_V4LFrameRate) < 0)) {
		printf("Error: ES2TS init failed\n");
		goto rtp_failed;
	}

	if (capturemode == CM_V4L) {
		start_v4l_capturing();
		v4l_mainloop();
		stop_v4l_capturing();
	}

	if (capturemode == CM_IPCVIDEO) {
		ipcvideo_start_capturing();
		ipcvideo_mainloop();
		ipcvideo_stop_capturing();
	}

	if (capturemode == CM_FIXED) {
		fixed_start_capturing();
		fixed_mainloop();
		fixed_stop_capturing();
	}

	if (capturemode == CM_FIXED_4K) {
		fixed_4k_start_capturing();
		fixed_4k_mainloop();
		fixed_4k_stop_capturing();
	}

	encoder_close();

	freeESHandler();

rtp_failed:
	if (ipport)
		freeRTPHandler();

encoder_failed:
	if (capturemode == CM_V4L) {
		uninit_v4l_device();
		close_v4l_device();
	}
	if (capturemode == CM_IPCVIDEO) {
		ipcvideo_uninit_device();
		ipcvideo_close_device();
	}
	if (capturemode == CM_FIXED) {
		fixed_uninit_device();
		fixed_close_device();
	}
	if (capturemode == CM_FIXED_4K) {
		fixed_4k_uninit_device();
		fixed_4k_close_device();
	}

	return 0;
}
