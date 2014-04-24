#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <libipcvideo/ipcvideo.h>
#include "encoder.h"
#include "main.h"

static int ipcScreenW = 0;
static int ipcScreenH = 0;
static int ipcFPS = 30;
static int ipcResubmitTimeoutMS = 0;

static struct ipcvideo_s *ctx = 0;

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
	ssize_t src_frame_size = ((ipcScreenW * 2) * ipcScreenH); /* YUY2 */
	if (size != src_frame_size) {
		printf("wrong buffer size: %zu expect %zu\n", size, src_frame_size);
		return;
	}

	if (!encoder_encode_frame((unsigned char *)p))
		time_to_quit = 1;
}

void ipcvideo_mainloop(void)
{
	struct ipcvideo_buffer_s *buf = 0, *lastBuffer = 0;
	unsigned char *pixels;
	unsigned int length;
	unsigned int elapsedms;
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
}

void ipcvideo_stop_capturing(void)
{
}

void ipcvideo_start_capturing(void)
{
}

void ipcvideo_uninit_device(void)
{
	int ret = ipcvideo_context_destroy(ctx);
	if (KLAPI_FAILED(ret)) {
		printf("Failed to destroy a context\n");
		return;
	}
}

void ipcvideo_init_device(int width, int height, int fps)
{
	ipcScreenW = width;
	ipcScreenH = height;
	ipcFPS = fps;

	/* Lets give the timeout a small amount of headroom for a frame to arrive (3ms) */
	/* 30fps input creates a timeout of 36ms or as low as 27.7 fps, before we resumbit a prior frame. */
	ipcResubmitTimeoutMS = (1000 / ipcFPS) + 3;
	printf("%s(%d, %d, %d timeout=%d)\n", __func__, ipcScreenW, ipcScreenH, ipcFPS, ipcResubmitTimeoutMS);
}

void ipcvideo_close_device(void)
{
	int ret = ipcvideo_context_detach(ctx);
	if (KLAPI_FAILED(ret)) {
		printf("Unable to detach err = %d, aborting.\n", ret);
		return;
	}

	ipcvideo_dump_context(ctx);
}

void ipcvideo_open_device()
{
	int ret = ipcvideo_context_create(&ctx);
	if (KLAPI_FAILED(ret)) {
		printf("Failed to create a context\n");
		return;
	}

	ipcvideo_dump_context(ctx);

	/* Attach to a segment, get its working dimensions */
	struct ipcvideo_dimensions_s d;
	ret = ipcvideo_context_attach(ctx, 1999, "/tmp", &d);
	if (KLAPI_FAILED(ret)) {
		printf("Unable to attach to segment, aborting.\n");
		return;
	}

	printf("Attached to ipcvideo segment, dimensions %dx%d fourcc: %08x\n",
		d.width, d.height, d.fourcc);

	ipcvideo_dump_context(ctx);
	ipcvideo_dump_metadata(ctx);
	ipcvideo_dump_buffers(ctx);
}

