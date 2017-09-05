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

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include "encoder.h"
#include "capture.h"
#include "fixed-frame.h"
#include "main.h"

static struct encoder_operations_s *encoder = 0;
static int ipcFPS = 30;
static int ipcResubmitTimeoutMS = 0;
static int fixedWidth;
static int fixedHeight;
static struct encoder_params_s *encoder_params = 0;

static unsigned int measureElapsedMS(struct timeval *then)
{
	struct timeval now;
	gettimeofday(&now, 0);

	unsigned int elapsedTime = (now.tv_sec - then->tv_sec) * 1000.0; /* sec to ms */
	elapsedTime += (now.tv_usec - then->tv_usec) / 1000.0;  /* us to ms */

	return elapsedTime;
}

/* Frame arrived from capture hardware, convert and
 * send to the hardware H264 compressor.
 */
static void fixed_process_image(const void *p, ssize_t size)
{
	ssize_t src_frame_size = (fixedWidth * 2) * fixedHeight; /* YUY2 */
	if (size != src_frame_size) {
		printf("wrong buffer size: %zu expect %zu\n", size, src_frame_size);
		return;
	}

	if (!encoder_encode_frame(encoder, encoder_params, (unsigned char *)p))
		time_to_quit = 1;
}

static void fixed_mainloop(void)
{
	/* This doesn't guarantee 60fps but its reasonably close for
	 * non-realtime environments with low cpu load.
	 */
	struct timeval now;
	unsigned int p = 0;

	/* Imagemagik did a half-assed job at converting BMP to yuyv,
	 * do a byte re-order to fix the colorspace.
	 */
	unsigned char a;
	for (unsigned int i = 0; i < sizeof(fixedframe); i += 2) {
		a = fixedframe[i];
		fixedframe[i] = fixedframe[i + 1];
		fixedframe[i + 1] = a;
	}

	/* calculate the frame processing time and attempt
	 * to sleep for the correct interval before pushing a
	 * new frame.
	 */
	while (!time_to_quit) {
		if (p > 33)
			p = 33;

		usleep((33 - p) * 1000); /* 33.3ms - 30fps +- */

		gettimeofday(&now, 0);
		fixed_process_image(fixedframe, sizeof(fixedframe));
		p = measureElapsedMS(&now);
	}
}

static void fixed_stop_capturing(void)
{
}

static int fixed_start_capturing(struct encoder_operations_s *e)
{
	if (!e)
		return -1;

	encoder = e;
	return 0;
}

static void fixed_uninit_device(void)
{
}

static int fixed_init_device(struct encoder_params_s *p, struct capture_parameters_s *c)
{
	c->width = fixedWidth;
	c->height = fixedHeight;
	ipcFPS = c->fps;
	encoder_params = p;

	/* Lets give the timeout a small amount of headroom for a frame to arrive (3ms) */
	/* 30fps input creates a timeout of 36ms or as low as 27.7 fps, before we resumbit a prior frame. */
	printf("%s(%d, %d, %d timeout=%d)\n", __func__,
		c->width, c->height,
		ipcFPS, ipcResubmitTimeoutMS);
	ipcResubmitTimeoutMS = (1000 / ipcFPS) + 3;

	encoder_params->input_fourcc = E_FOURCC_YUY2;

	return 0;
}

static void fixed_close_device(void)
{
}

static int fixed_open_device()
{
	fixedWidth = fixedframeWidth;
	fixedHeight = fixedframeHeight;
	if (((fixedWidth * 2) * fixedHeight) != sizeof(fixedframe)) {
		fprintf(stderr, "fixed frame size miss-match\n");
		exit(1);
	}

	return 0;
}

static void fixed_set_defaults(struct capture_parameters_s *c)
{
	c->type = CM_FIXED;
}

struct capture_operations_s fixed_ops =
{
	.type		= CM_FIXED,
	.name		= "Fixed Video Image (SD)",
	.set_defaults	= fixed_set_defaults,
	.mainloop	= fixed_mainloop,
	.stop		= fixed_stop_capturing,
	.start		= fixed_start_capturing,
	.uninit		= fixed_uninit_device,
	.init		= fixed_init_device,
	.close		= fixed_close_device,
	.open		= fixed_open_device,
	.default_fps	= 30,
};

