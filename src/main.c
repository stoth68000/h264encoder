#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <libes2ts/es2ts.h>

#include "capture.h"
#include "rtp.h"
#include "mxcvpuudp.h"
#include "encoder.h"
#include "es2ts.h"
#include "main.h"

unsigned int capturemode = CM_V4L;
int time_to_quit = 0;

static void signalHandler(int a_Signal)
{
	time_to_quit = 1;
	signal(SIGINT, SIG_DFL);
}

static void signalTermHandler(int a_Signal)
{
	time_to_quit = 1;
	signal(SIGTERM, SIG_DFL);
}

static void usage(struct encoder_operations_s *encoder, int argc, char **argv)
{
	struct encoder_params_s p;

	encoder->set_defaults(&p);

	printf("Usage:\n"
		"\n"
		"Options:\n"
	        "-h, --help                    Print this message\n"
	        "-V, --version                 Display version\n"
	        "-v, --verbose                 Log debugging messages\n"
	        "-q, --quiet                   Don't display progress indicator\n"
		"-b, --bitrate <number>        Encoding bitrate [def: %d]\n"
		"-d, --device=NAME             Video device name [/dev/video0]\n"
		"-o, --output=filename         Record raw nals to output file\n"
		"-O, --csv=filename            Record output frames to a file\n"
		"-i, --ipaddress=a.b.c.d       Remote IP RTP address\n"
		"-p, --ipport=9999             Remote IP RTP port\n"
		"    --mxc_ipaddress=a.b.c.d   Freescale MXC_VPU_TEST UDP IP address\n"
		"    --mxc_ipport=9999         Freescale MXC_CPU_TEST UDP port\n"
		"    --mxc_endian <0,1>        0 = little, 1 = big [def: 1]\n"
		"    --mxc_validate <file>     Scan file and check for any basic header errors\n"
		"    --mxc_sendmode <1,2>      1=single xfer, 2=large-iframe [def: 2]\n"
		"    --dscp=XXX                DSCP class to use 1-63 (for example 26 for AF31)\n"
		"    --packet-size=XXX         Use an alternate packet size\n"
		"    --ifd=N                   Specify an interframe delay in microseconds\n"
		"-f, --v4lframerate=FPS        Framerate [no limit]\n"
		"-n, --v4lnumerator=NUM        Numerator [no limit]\n"
		"    --v4lsyncstall <number>   Stop encoding if V4L input loses sync 0:false 1:true [def: 0]\n"
		"-I, --v4linput <number>       Select video inputnr #0-3 on video device [def: 0]\n"
		"-W, --dev-width <number>      Device width [720]\n"
		"-H, --dev-height <number>     Device height [480]\n"
		"-M, --mode <number>           0=v4l 1=ipcvideo 2=fixedframe 3=fixedframe4k [def: 0]\n"
		"-D, --vppdeinterlace <number> 0=off 1=motionadaptive 2=bob\n"
		"    --compressor <number>     0=vaapi 1=libavcodec/x264 2=x264 [def: 0]\n",
		p.frame_bitrate
		);

	printf( "    --initial_qp <number>     Initial Quantization Parameter [def: %d]\n"
		"    --minimal_qp <number>     Minimum Quantization Parameter [def: %d]\n"
		"    --intra_period <number>   [def: %d]\n"
		"    --idr_period <number>     [def: %d]\n"
		"    --ip_period <number>      [def: %d]\n"
		"    --rcmode <NONE|CBR|VBR|VCM|CQP|VBR_CONSTRAINED> [def: %s]\n"
		"    --entropy <0|1>, 1 means cabac, 0 cavlc [def: %d]\n"
		"    --profile <BP|CBP|MP|HP>  [def: %s]\n"
		"    --payloadmode <0|1>, 0 means RTP/TS, 1 RTP/ES [def: 0]\n"
		"    --level_idc <number>      [def: %d]\n"
		"    --hrd_bitrate_multiplier <number> [def: %d]\n",
			p.initial_qp,
			p.minimal_qp,
			p.intra_period,
			p.intra_idr_period,
			p.ip_period,
			encoder_rc_to_string(p.rc_mode),
			p.h264_entropy_mode,
			encoder_profile_to_string(p.h264_profile),
			p.level_idc,
			p.hrd_bitrate_multiplier
	       );
}

static const char short_options[] = "vqVb:d:i:o:O:p:hmruD:Pf:n:W:H:M:I:Z:D:";

static const struct option long_options[] = {
	{ "verbose", no_argument, NULL, 'v' },
	{ "quiet", no_argument, NULL, 'q' },
	{ "version", no_argument, NULL, 'V' },
	{ "bitrate", required_argument, NULL, 'b' },
	{ "device", required_argument, NULL, 'd' },
	{ "ipaddress", required_argument, NULL, 'i' },
	{ "ipport", required_argument, NULL, 'p' },
	{ "help", no_argument, NULL, 'h' },
	{ "mmap", no_argument, NULL, 'm' },
	{ "output", required_argument, NULL, 'o' },
	{ "csv", required_argument, NULL, 'O' },
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
	{ "intra_period", required_argument, NULL, 1 },
	{ "idr_period", required_argument, NULL, 2 },
	{ "ip_period", required_argument, NULL, 3 },
	{ "rcmode", required_argument, NULL, 4 },
	{ "entropy", required_argument, NULL, 5 },
	{ "profile", required_argument, NULL, 6 },
	{ "initial_qp", required_argument, NULL, 7 },
	{ "minimal_qp", required_argument, NULL, 8 },
	{ "dscp", required_argument, NULL, 9 },
	{ "packet-size", required_argument, NULL, 10 },
	{ "ifd", required_argument, NULL, 11 },
	{ "v4lsyncstall", required_argument, NULL, 12 },
	{ "payloadmode", required_argument, NULL, 13 },
	{ "level_idc", required_argument, NULL, 14 },
	{ "mxc_ipaddress", required_argument, NULL, 15 },
	{ "mxc_ipport", required_argument, NULL, 16 },
	{ "mxc_endian", required_argument, NULL, 17 },
	{ "mxc_validate", required_argument, NULL, 18 },
	{ "mxc_sendmode", required_argument, NULL, 19 },
	{ "hrd_bitrate_multiplier", required_argument, NULL, 20 },
	{ "compressor", required_argument, NULL, 21 },

	{ 0, 0, 0, 0}
};

int main(int argc, char **argv)
{
	struct encoder_params_s encoder_params;
	struct capture_parameters_s capture_params;
	struct capture_operations_s *source = 0;
	struct encoder_operations_s *encoder = 0;

	char *ipaddress = "192.168.0.67";
	int ipport = 0, dscp = 0, pktsize = 0, ifd = 0;
	v4l_dev_name = (char *)"/dev/video0";
	int req_deint_mode = -1;
	int syncstall = 0;
	int width = 720, height = 480;
	int V4LFrameRate = 0;
	int V4LNumerator = 0;
	char *mxc_ipaddress = "192.168.0.67";
	char *mxc_validate_filename = 0;
	int mxc_ipport = 0, mxc_endian = 0, mxc_sendmode = 2;
	enum encoder_type_e compressor = EM_VAAPI;

	enum payloadMode_e {
		PAYLOAD_RTP_TS = 0,
		PAYLOAD_RTP_ES
	} payloadMode = PAYLOAD_RTP_TS;

	memset(&capture_params, 0, sizeof(capture_params));

	/* We currently support a single H.264 encoder type (VAAPI).
	 * Grab an interface to it.
	 */
	encoder = getEncoderTarget(EM_VAAPI);
	if (!encoder) {
		printf("Invalid encoder target, no encoder selected\n");
		exit(1);
	}
	encoder->set_defaults(&encoder_params);

	for (;;) {
		int index;
		int c;

		c = getopt_long(argc, argv, short_options, long_options, &index);

		if (-1 == c)
			break;

		switch (c) {
		case 0:	/* getopt_long() flag */
			break;
		case 'v':
			es2ts_debug = 1;
			break;
		case 'q':
			encoder_params.quiet_encode = 1;
			break;
		case 'V':
			fprintf(stderr, "%s\n", PACKAGE_STRING);
			exit(0);
			break;
		case 'b':
			encoder_params.frame_bitrate = atoi(optarg);
			break;
		case 'd':
			v4l_dev_name = optarg;
			break;
		case 'h':
			usage(encoder, argc, argv);
			exit(0);
		case 'i':
			ipaddress = optarg;
			break;
		case 'I':
			capture_params.v4l.inputnr = atoi(optarg);
			break;
		case 'm':
			io = IO_METHOD_MMAP;
			break;
		case 'M':
			capturemode = atoi(optarg);
			if (capturemode > CM_MAX) {
				usage(encoder, argc, argv);
				exit(0);
			}
			break;
		case 'D':
			req_deint_mode = atoi(optarg);
			if (req_deint_mode > 2) {
				usage(encoder, argc, argv);
				exit(0);
			}
			break;
		case 'o':
			encoder_params.encoder_nalOutputFilename = optarg;
			break;
		case 'O':
			encoder_params.csv_fp = fopen(optarg, "w");
			if (encoder_params.csv_fp == NULL) {
				printf("Error: unable to open %s: %m\n", optarg);
				exit(1);
			}
			break;
		case 1:
			encoder_params.intra_period = atoi(optarg);
			break;
		case 2:
			encoder_params.intra_idr_period = atoi(optarg);
			break;
		case 3:
			encoder_params.ip_period = atoi(optarg);
			break;
		case 4:
			encoder_params.rc_mode = encoder_string_to_rc(optarg);
			if (encoder_params.rc_mode < 0) {
				usage(encoder, argc, argv);
				exit(1);
			}
			break;
		case 5:
			encoder_params.h264_entropy_mode = atoi(optarg) ? 1: 0;
			break;
		case 6:
			encoder_params.h264_profile = encoder_string_to_profile(optarg);
			if (encoder_params.h264_profile < 0) {
				usage(encoder, argc, argv);
				exit(1);
			}
			break;
		case 7:
			encoder_params.initial_qp = atoi(optarg);
			break;
		case 8:
			encoder_params.minimal_qp = atoi(optarg);
			break;
		case 'r':
			io = IO_METHOD_READ;
			break;
		case 'u':
			io = IO_METHOD_USERPTR;
			break;
		case 'f':
			V4LFrameRate = atoi(optarg);
			if (!V4LNumerator)
				V4LNumerator = 1;
			break;
		case 'n':
			V4LNumerator = atoi(optarg);
			break;
		case 'p':
			ipport = atoi(optarg);
			break;
		case 9:
			dscp = atoi(optarg);
			if ((dscp < 0) || (dscp > 63)) {
				usage(encoder, argc, argv);
				exit(1);
			}
			break;
		case 10:
			pktsize = atoi(optarg);
			break;
		case 11:
			ifd = atoi(optarg);
			break;
		case 12:
			syncstall = atoi(optarg);
			break;
		case 13:
			payloadMode = (enum payloadMode_e)atoi(optarg) & 1;
			break;
		case 14:
			encoder_params.level_idc = atoi(optarg);
			break;
		case 15:
			mxc_ipaddress = optarg;
			break;
		case 16:
			mxc_ipport = atoi(optarg);
			break;
		case 17:
			mxc_endian = atoi(optarg) & 1;
			break;
		case 18:
			mxc_validate_filename = optarg;
			break;
		case 19:
			mxc_sendmode = atoi(optarg);
			if (mxc_sendmode < 0)
				mxc_sendmode = 1;
			else if (mxc_sendmode > 2)
				mxc_sendmode = 2;
			break;
		case 20:
			encoder_params.hrd_bitrate_multiplier = atoi(optarg);
			break;
		case 21:
			compressor = (enum encoder_type_e)atoi(optarg);
			if (compressor <= EM_VAAPI)
				compressor = EM_VAAPI;
			if (compressor > EM_MAX)
				compressor = EM_MAX - 1;
	
			encoder = getEncoderTarget(compressor);
			if (!encoder) {
				printf("Invalid encoder target, no encoder selected\n");
				exit(1);
			}
			encoder->set_defaults(&encoder_params);
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
			usage(encoder, argc, argv);
			exit(1);
		}
	}

	/* Utility function, varify a file looks like valid MXC VPU test data */
	if (mxc_validate_filename) {
		if (validateMXCVPUUDPOutput(mxc_validate_filename, mxc_endian) < 0) {
			fprintf(stderr, "file:%s is invalid or contains illegal content\n",
				mxc_validate_filename);
			return -1;
		}
		fprintf(stderr, "file:%s is valid\n", mxc_validate_filename);
		return 0;
	}

	printf("RTP Payload: ");
	if (payloadMode == 0)
		printf("TS\n");
	else
		printf("ES\n");

	source = getCaptureSource(capturemode);
	if (!source) {
		printf("Invalid capture mode, no capture interface defined\n");
		exit(1);
	}
	source->set_defaults(&capture_params);

	/* */
	if (V4LFrameRate == 0) {
		capture_params.v4l.V4LFrameRate = source->default_fps;
		capture_params.v4l.V4LNumerator = 1;
	} else {
		capture_params.v4l.V4LFrameRate = V4LFrameRate;
		capture_params.v4l.V4LNumerator = V4LNumerator;
	}
	capture_params.fps = capture_params.v4l.V4LFrameRate;
	V4LFrameRate = capture_params.fps;

	/* Configure the encoder to match the capture source */
	if ((source->type == CM_FIXED) || (source->type == CM_FIXED_4K))
		encoder_params.enable_osd = 1;

	if (source->type == CM_V4L) {
		if (req_deint_mode == -1 /* UNSET */)
			encoder_params.deinterlacemode = 2;
		else
			encoder_params.deinterlacemode = req_deint_mode;
		capture_params.v4l.syncstall = syncstall;
	}

	if (signal(SIGINT, signalHandler) == SIG_ERR) {
		printf("signal() failed\n");
		time_to_quit = 1;
	}
	if (signal(SIGTERM, signalTermHandler) == SIG_ERR) {
		printf("signal() failed\n");
		time_to_quit = 1;
	}

	if (source->open() < 0) {
		printf("Error: %s capture did not start\n", source->name);
		goto encoder_failed;
	}

	/* Init the capture source. Suggest we want a certain width/height.
	 * Source can/will update the width / height.
	 */
	capture_params.width = width;
	capture_params.height = height;
	source->init(&encoder_params, &capture_params);

	/* Initialize the encoder with the sources mandatory width / height */
	encoder_params.height = capture_params.height;
	encoder_params.width = capture_params.width;
	if (encoder->init(&encoder_params)) {
		printf("Error: Encoder init failed\n");
		goto encoder_failed;
	}

	printf("%s Capture: %dx%d %d/%d [osd: %s] [mxc_streaming: %s]\n",
		source->name,
		encoder_params.width,
		encoder_params.height,
		V4LNumerator, V4LFrameRate,
		encoder_params.enable_osd ? "Enabled" : "Disabled",
		mxc_ipport ? "Enabled" : "Disabled");

#if 0
	/* Open the 'nals via rtp' mechanism if requested, but only for the internal GL demo app */
	if (
		(capturemode == CM_OPENGL) &&
		ipport && (initRTPHandler(ipaddress, ipport, width, height, g_V4LFrameRate) < 0)) {
		printf("Error: RTP init failed\n");
		goto rtp_failed;
	}
#endif

	/* Open the 'nals via freescale UDP proprietary' mechanism if requested */
	if (mxc_ipport && (initMXCVPUUDPHandler(mxc_ipaddress, mxc_ipport, dscp, 4 * 1048576, ifd, mxc_endian, mxc_sendmode) < 0)) {
		printf("Error: MXCVPUUDP init failed\n");
		goto rtp_failed;
	}

	/* RPT/ES , routed out via RTP */
	if ((payloadMode == PAYLOAD_RTP_ES) && ipport) {
	 	if (initRTPHandler(ipaddress, ipport, dscp, pktsize, ifd,
			encoder_params.width, encoder_params.height, V4LFrameRate) < 0) {
			printf("Error: RTP init failed\n");
			goto mxc_failed;
		}
	}

	/* the NAL/es to TS conversion layer, while routes out via RTP */
	if ((payloadMode == PAYLOAD_RTP_TS) && ipport) {
		if (initESHandler(ipaddress, ipport, dscp, pktsize, ifd,
			encoder_params.width, encoder_params.height, V4LFrameRate) < 0) {
			printf("Error: ES2TS init failed\n");
			goto rtp_failed;
		}
	}

	/* Start, capture content and stop the device, the main processing */
	if (source->start(encoder) < 0) {
		printf("Source failed to start\n");
		goto start_failed;
	}

	source->mainloop();
	source->stop();

start_failed:
	encoder->close(&encoder_params);

	freeESHandler();

mxc_failed:
	if (mxc_ipport)
		freeMXCVPUUDPHandler();

rtp_failed:
	if (ipport)
		freeRTPHandler();

encoder_failed:
	source->uninit();
	source->close();

	return 0;
}
