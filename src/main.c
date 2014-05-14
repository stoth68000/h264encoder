#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>

#include "rtp.h"
#include "v4l.h"
#include "ipcvideo.h"
#include "fixed.h"
#include "encoder.h"
#include "es2ts.h"
#include "main.h"

#define DEF_PNGPATH "/usr/local/bin"

/* main.c */
#define CM_V4L		0
#define CM_IPCVIDEO	1
#define CM_FIXED	2
#define CM_MAX		CM_FIXED

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
	       "-?, --help               Print this message\n"
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
	       "-M, --mode=NUM           0=v4l 1=ipcvideo 2=fixedframe\n"
	       "-D, --vppdeinterlace=NUM 0=off 1=motionadaptive 2=bob\n"
	       );
}

static const char short_options[] = "b:d:i:o:p:?mruD:Pf:n:W:H:M:I:Z:D:";

static const struct option long_options[] = {
	{ "bitrate", required_argument, NULL, 'b' },
	{ "device", required_argument, NULL, 'd' },
	{ "ipaddress", required_argument, NULL, 'i' },
	{ "ipport", required_argument, NULL, 'p' },
	{ "help", no_argument, NULL, '?' },
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
	{ 0, 0, 0, 0}
};

int main(int argc, char **argv)
{
	char *ipaddress = "192.168.0.67";
	int ipport = 0;
	int videoinputnr = 0;
	int enable_osd = 0;
	int deinterlace = 0;
	v4l_dev_name = (char *)"/dev/video0";

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
		case '?':
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
			deinterlace = atoi(optarg);
			if (deinterlace > 2) {
				usage(argc, argv);
				exit(0);
			}
			break;
		case 'o':
			encoder_nalOutputFilename = optarg;
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
			enable_osd = atoi(optarg);
			break;
		default:
			usage(argc, argv);
			exit(1);
		}
	}

	if (capturemode == CM_FIXED)
		enable_osd = 1;

	if (capturemode == CM_V4L)
		printf("V4L Capture: %dx%d %d/%d [input: %d]\n", width, height,
			g_V4LNumerator, g_V4LFrameRate, videoinputnr);

	if (capturemode == CM_IPCVIDEO)
		printf("IPC Video Capture: %dx%d %d/%d\n", width, height, g_V4LNumerator, g_V4LFrameRate);

	if (capturemode == CM_FIXED)
		printf("Fixed Frame Capture: %d/%d [osd: %s]\n", g_V4LNumerator, g_V4LFrameRate,
			enable_osd ? "Enabled" : "Disabled");

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

	printf("Using frame resolution: %dx%d\n", width, height);

	if (encoder_init(width, height, enable_osd, deinterlace)) {
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
	if (((capturemode == CM_V4L) || (capturemode == CM_IPCVIDEO) || (capturemode == CM_FIXED)) &&
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

	return 0;
}
