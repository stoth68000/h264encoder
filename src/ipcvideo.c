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
#include <sys/time.h>
#include <libipcvideo/ipcvideo.h>
#include "encoder.h"
#include "capture.h"
#include "main.h"

static int ipcFPS = 30;
static int ipcResubmitTimeoutMS = 0;

static struct encoder_operations_s *encoder = 0;
static struct ipcvideo_s *ctx = 0;
static struct ipcvideo_dimensions_s ipc_dimensions;
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
static void ipcvideo_process_image(const void *p, ssize_t size)
{
	if (IS_YUY2(encoder_params)) {
		ssize_t src_frame_size = ((ipc_dimensions.width * 2) * ipc_dimensions.height); /* YUY2 */
		if (size != src_frame_size) {
			printf("wrong buffer size: %zu expect %zu\n", size, src_frame_size);
			return;
		}
	} else
	if (IS_BGRX(encoder_params)) {
		ssize_t src_frame_size = ((ipc_dimensions.width * 4) * ipc_dimensions.height); /* YUY2 */
		if (size != src_frame_size) {
			printf("wrong buffer size: %zu expect %zu\n", size, src_frame_size);
			return;
		}
	} else
	if (IS_I420(encoder_params)) {
		ssize_t src_frame_size =
			(ipc_dimensions.width * ipc_dimensions.height)		+ /* Y */
			((ipc_dimensions.width * ipc_dimensions.height) / 4)	+ /* U */
			((ipc_dimensions.width * ipc_dimensions.height) / 4)	; /* V */
			
		if (size != src_frame_size) {
			printf("wrong buffer size: %zu expect %zu\n", size, src_frame_size);
			return;
		}
	}

	if (!encoder_encode_frame(encoder, encoder_params, (unsigned char *)p))
		time_to_quit = 1;
}

void ipcvideo_mainloop(void)
{
	struct ipcvideo_buffer_s *buf = 0, *lastBuffer = 0;
	unsigned char *pixels;
	unsigned int length;
	int elapsedms;
	int ret;

	while (!time_to_quit) {

		buf = 0;
#define MEASURE_TIMEOUTS 1

#if MEASURE_TIMEOUTS
		struct timeval now;
		gettimeofday(&now, 0);
#endif
		ret = ipcvideo_list_busy_timedwait(ctx, ipcResubmitTimeoutMS);
#if MEASURE_TIMEOUTS
		elapsedms = measureElapsedMS(&now);
		if (elapsedms > ipcResubmitTimeoutMS)
			printf("Requested %d got %d\n", ipcResubmitTimeoutMS, elapsedms);
#endif
		if (ret == KLAPI_TIMEOUT) {
			/* Pull the previous frame */
			if (!lastBuffer)
				continue;

			buf = lastBuffer;
			//printf("%s() re-using last buffer %p\n", __func__, lastBuffer);
		} else {
			ret = ipcvideo_list_busy_dequeue(ctx, &buf);
			if (KLAPI_FAILED(ret))
				continue;
		}

		if (buf && lastBuffer && (buf != lastBuffer)) {
			/* requeue our last buffer */
			/* pop the used frame back on the free list */
			ipcvideo_list_free_enqueue(ctx, lastBuffer);
			lastBuffer = 0;
		}

		ret = ipcvideo_buffer_get_data(ctx, buf, &pixels, &length);
		if (KLAPI_FAILED(ret)) {
			ipcvideo_list_busy_enqueue(ctx, buf);
			continue;
		}
#if 0
		printf("%s() Got busy buffer %p [%p] length %x [ %02x %02x %02x %02x ]\n",
			__func__, buf, 
			pixels, length,
			*(pixels + 0),
			*(pixels + 1),
			*(pixels + 2),
			*(pixels + 3));
#endif
		/* Push the frame into the encoder */
		ipcvideo_process_image(pixels, length);

		lastBuffer = buf;
	}
	if (lastBuffer) {
		/* pop the used frame back on the free list, else we lose it on closedown. */
		ipcvideo_list_free_enqueue(ctx, lastBuffer);
	}
}

static void ipcvideo_stop_capturing(void)
{
}

static int ipcvideo_start_capturing(struct encoder_operations_s *e)
{
	if (!e)
		return -1;

	encoder = e;
	return 0;
}

static void ipcvideo_uninit_device(void)
{
	int ret = ipcvideo_context_destroy(ctx);
	if (KLAPI_FAILED(ret)) {
		printf("Failed to destroy a context\n");
		return;
	}
}

static int ipcvideo_init_device(struct encoder_params_s *p, struct capture_parameters_s *c)
{
	c->width = ipc_dimensions.width;
	c->height = ipc_dimensions.height;
	ipcFPS = c->fps;
	encoder_params = p;

	/* Lets give the timeout a small amount of headroom for a frame to arrive (3ms) */
	/* 30fps input creates a timeout of 36ms or as low as 27.7 fps, before we resumbit a prior frame. */
	ipcResubmitTimeoutMS = (1000 / ipcFPS) + 3;
	printf("%s(%d, %d, %d timeout=%d)\n", __func__,
		ipc_dimensions.width,
		ipc_dimensions.height,
		ipcFPS, ipcResubmitTimeoutMS);

	if (ipc_dimensions.fourcc == IPCFOURCC_YUYV)
		encoder_params->input_fourcc = E_FOURCC_YUY2;
	else
	if (ipc_dimensions.fourcc == IPCFOURCC_BGRX)
		encoder_params->input_fourcc = E_FOURCC_BGRX;
	else
	if (ipc_dimensions.fourcc == IPCFOURCC_I420)
		encoder_params->input_fourcc = E_FOURCC_I420;
	else
		encoder_params->input_fourcc = E_FOURCC_UNDEFINED;

	return 0;
}

static void ipcvideo_close_device(void)
{
	int ret = ipcvideo_context_detach(ctx);
	if (KLAPI_FAILED(ret)) {
		printf("Unable to detach err = %d, aborting.\n", ret);
		return;
	}

	ipcvideo_dump_context(ctx);
}

static int ipcvideo_open_device()
{
	int ret = ipcvideo_context_create(&ctx);
	if (KLAPI_FAILED(ret)) {
		printf("Failed to create a context\n");
		return -1;
	}

	ipcvideo_dump_context(ctx);

	/* Attach to a segment, get its working dimensions */
	ret = ipcvideo_context_attach(ctx, 1999, "/tmp", &ipc_dimensions);
	if (KLAPI_FAILED(ret)) {
		printf("Unable to attach to segment, aborting.\n");
		return -1;
	}

	printf("Attached to ipcvideo segment, dimensions %dx%d fourcc: %08x\n",
		ipc_dimensions.width,
		ipc_dimensions.height,
		ipc_dimensions.fourcc);

	ipcvideo_dump_context(ctx);
	ipcvideo_dump_metadata(ctx);
	ipcvideo_dump_buffers(ctx);

	return 0;
}

static void ipcvideo_set_defaults(struct capture_parameters_s *c)
{
	c->type = CM_IPCVIDEO;
}

struct capture_operations_s ipcvideo_ops =
{
	.type		= CM_IPCVIDEO,
	.name		= "IPCVideo pipeline",
	.set_defaults	= ipcvideo_set_defaults,
	.mainloop	= ipcvideo_mainloop,
	.stop		= ipcvideo_stop_capturing,
	.start		= ipcvideo_start_capturing,
	.uninit		= ipcvideo_uninit_device,
	.init		= ipcvideo_init_device,
	.close		= ipcvideo_close_device,
	.open		= ipcvideo_open_device,
	.default_fps	= 30,
};

