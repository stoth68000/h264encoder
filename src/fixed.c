#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include "encoder.h"
#include "fixed-frame.h"
#include "main.h"

static int ipcFPS = 30;
static int ipcResubmitTimeoutMS = 0;
static int fixedWidth;
static int fixedHeight;

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

	if (!encoder_encode_frame((unsigned char *)p))
		time_to_quit = 1;
}

void fixed_mainloop(void)
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

void fixed_stop_capturing(void)
{
}

void fixed_start_capturing(void)
{
}

void fixed_uninit_device(void)
{
}

void fixed_init_device(unsigned int *width, unsigned int *height, int fps)
{
	*width = fixedWidth;
	*height = fixedHeight;
	ipcFPS = fps;

	/* Lets give the timeout a small amount of headroom for a frame to arrive (3ms) */
	/* 30fps input creates a timeout of 36ms or as low as 27.7 fps, before we resumbit a prior frame. */
	ipcResubmitTimeoutMS = (1000 / ipcFPS) + 3;
	printf("%s(%d, %d, %d timeout=%d)\n", __func__,
		*width, *height,
		ipcFPS, ipcResubmitTimeoutMS);

}

void fixed_close_device(void)
{
}

int fixed_open_device()
{
	fixedWidth = fixedframeWidth;
	fixedHeight = fixedframeHeight;
	if (((fixedWidth * 2) * fixedHeight) != sizeof(fixedframe)) {
		fprintf(stderr, "fixed frame size miss-match\n");
		exit(1);
	}

	return 0;
}
