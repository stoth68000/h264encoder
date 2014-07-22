/*
 * This example is based on: http://linuxtv.org/downloads/v4l-dvb-apis/capture-example.html
 *  V4L2 video capture example
 *      This program is provided with the V4L2 API
 * see http://linuxtv.org/docs.php for more information
*/

#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <malloc.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "v4l.h"
#include "encoder.h"
#include "main.h"
#include "capture.h"

#include <linux/videodev2.h>

#define CLEAR(x) memset (&(x), 0, sizeof (x))

struct buffer {
        void *start;
        size_t length;
};

io_method io = IO_METHOD_MMAP;
char *v4l_dev_name = NULL;
char *encoder_nalOutputFilename = NULL;

unsigned int width = 720;
unsigned int height = 480;
unsigned int g_V4LNumerator = 0;
unsigned int g_V4LFrameRate = 0;
static unsigned int g_inputnr = 0;
static unsigned int g_signalCount = 0;
static unsigned int g_signalLocked = 1;
static unsigned int g_syncStall;

static struct buffer *buffers = NULL;
static unsigned int n_buffers = 0;
static char device_settings_buffer[255];
static char *device_settings = NULL;
static int fd = -1;
static unsigned int pixelformat = V4L2_PIX_FMT_YUYV;
static struct encoder_params_s *encoder_params = 0;

#define EXIT_FAILURE 1

/* ************************************************ */
static void errno_exit(const char *s)
{
	printf("error %s\n", strerror(errno));

	exit(EXIT_FAILURE);
}

static int xioctl(int fd, int request, void *arg)
{
	int r;
	do
		r = ioctl(fd, request, arg);
	while (-1 == r && EINTR == errno);
	return r;
}

/* Frame arrived from capture hardware, convert and
 * send to the hardware H264 compressor.
 */
static void v4l_process_image(const void *p, ssize_t size)
{
	ssize_t src_frame_size = ((width * 2) * height);
	if (size != src_frame_size) {
		printf("wrong buffer size: %zu expect %zu\n", size,
		       src_frame_size);
		return;
	}
	if (!encoder_encode_frame(encoder_params, (unsigned char *)p))
		time_to_quit = 1;
}

static int read_frame(void)
{
	struct v4l2_buffer buf;
	unsigned int i;

	/* Periodically check the signal status, every 2 seconds or so */
	if (g_syncStall && (g_signalCount++ == 60)) {
		g_signalCount = 0;

		struct v4l2_input i;
		i.index = g_inputnr;
		if (0 == xioctl(fd, VIDIOC_ENUMINPUT, &i)) {
			if ((g_signalLocked == 1) && (i.status & V4L2_IN_ST_NO_SIGNAL)) {
				g_signalLocked = 0;
				printf("V4L signal unlocked\n");
			} else
			if ((g_signalLocked == 0) && ((i.status & V4L2_IN_ST_NO_SIGNAL) == 0)) {
				g_signalLocked = 1;
				printf("V4L signal locked\n");
			}
		}
	}

	if (g_syncStall && (!g_signalLocked)) {
		/* 30ms sleep if we're not locked.
		 * prevent constant queries from absorbing all the cpu
		 */
		usleep(30 * 1000);
		return 0;
	}

	switch (io) {
	case IO_METHOD_READ:
		if (-1 == read(fd, buffers[0].start, buffers[0].length)) {
			switch (errno) {
			case EAGAIN:
				return 0;
			case EIO:
				/* Could ignore EIO, see spec. */
				/* fall through */
			default:
				errno_exit("read");
			}
		}
		v4l_process_image(buffers[0].start, buffers[0].length);
		break;

	case IO_METHOD_MMAP:
		CLEAR(buf);

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
			switch (errno) {
			case EAGAIN:
				return 0;

			case EIO:
				/* Could ignore EIO, see spec. */
				/* fall through */
			default:
				errno_exit("VIDIOC_DQBUF");
			}
		}

		assert(buf.index < n_buffers);

		v4l_process_image(buffers[buf.index].start, buf.length);

		if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
			errno_exit("VIDIOC_QBUF");

		break;

	case IO_METHOD_USERPTR:
		CLEAR(buf);

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_USERPTR;

		if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
			switch (errno) {
			case EAGAIN:
				return 0;

			case EIO:
				/* Could ignore EIO, see spec. */
				/* fall through */
			default:
				errno_exit("VIDIOC_DQBUF");
			}
		}

		for (i = 0; i < n_buffers; ++i)
			if (buf.m.userptr == (unsigned long)buffers[i].start
			    && buf.length == buffers[i].length)
				break;

		assert(i < n_buffers);
		v4l_process_image((void *)buf.m.userptr, buf.length);
		if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
			errno_exit("VIDIOC_QBUF");

		break;
	}

	return 1;
}

static void v4l_mainloop(void)
{
	while (!time_to_quit) {
		for (;;) {
			fd_set fds;
			struct timeval tv;
			int r;

			FD_ZERO(&fds);
			FD_SET(fd, &fds);

			/* Timeout. */
			tv.tv_sec = 5;
			tv.tv_usec = 0;

			r = select(fd + 1, &fds, NULL, NULL, &tv);

			if (-1 == r) {
				if (EINTR == errno)
					continue;

				errno_exit("select");
			}

			if (0 == r) {
				printf("select timeout\n");
				exit(EXIT_FAILURE);
			}

			if (read_frame())
				break;

			/* EAGAIN - continue select loop. */
		}
	}
}

static void v4l_stop_capturing(void)
{
	enum v4l2_buf_type type;

	switch (io) {
	case IO_METHOD_READ:
		/* Nothing to do. */
		break;

	case IO_METHOD_MMAP:
	case IO_METHOD_USERPTR:
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
			errno_exit("VIDIOC_STREAMOFF");

		break;
	}
}

static void v4l_start_capturing(void)
{
	unsigned int i;
	enum v4l2_buf_type type;

	switch (io) {
	case IO_METHOD_READ:
		/* Nothing to do. */
		break;

	case IO_METHOD_MMAP:
		for (i = 0; i < n_buffers; ++i) {
			struct v4l2_buffer buf;

			CLEAR(buf);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index = i;

			if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
				errno_exit("VIDIOC_QBUF");
		}

		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
			errno_exit("VIDIOC_STREAMON");

		break;

	case IO_METHOD_USERPTR:
		for (i = 0; i < n_buffers; ++i) {
			struct v4l2_buffer buf;

			CLEAR(buf);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_USERPTR;
			buf.index = i;
			buf.m.userptr = (unsigned long)buffers[i].start;
			buf.length = buffers[i].length;

			if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
				errno_exit("VIDIOC_QBUF");
		}

		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
			errno_exit("VIDIOC_STREAMON");

		break;
	}
}

static void v4l_uninit_device(void)
{
	unsigned int i;

	switch (io) {
	case IO_METHOD_READ:
		free(buffers[0].start);
		break;

	case IO_METHOD_MMAP:
		for (i = 0; i < n_buffers; ++i)
			if (-1 == munmap(buffers[i].start, buffers[i].length))
				errno_exit("munmap");
		break;

	case IO_METHOD_USERPTR:
		for (i = 0; i < n_buffers; ++i)
			free(buffers[i].start);
		break;
	}

	free(buffers);
}

static void init_read(unsigned int buffer_size)
{
	buffers = (struct buffer *)calloc(1, sizeof(*buffers));

	if (!buffers) {
		printf("Out of memory\n");
		exit(EXIT_FAILURE);
	}

	buffers[0].length = buffer_size;
	buffers[0].start = malloc(buffer_size);

	if (!buffers[0].start) {
		printf("Out of memory\n");
		exit(EXIT_FAILURE);
	}
}

static void init_mmap(void)
{
	struct v4l2_requestbuffers req;

	CLEAR(req);

	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			printf(" does not support memory mapping\n");
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_REQBUFS");
		}
	}

	if (req.count < 2) {
		printf("Insufficient buffer memory\n");
		exit(EXIT_FAILURE);
	}

	buffers = (struct buffer *)calloc(req.count, sizeof(*buffers));

	if (!buffers) {
		printf("Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
		struct v4l2_buffer buf;
		CLEAR(buf);
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = n_buffers;
		if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
			errno_exit("VIDIOC_QUERYBUF");
		buffers[n_buffers].length = buf.length;
		buffers[n_buffers].start = mmap(NULL /* start anywhere */ ,
						buf.length,
						PROT_READ | PROT_WRITE
						/* required */ ,
						MAP_SHARED /* recommended */ ,
						fd, buf.m.offset);

		if (MAP_FAILED == buffers[n_buffers].start)
			errno_exit("mmap");
	}
}

static void init_userp(unsigned int buffer_size)
{
	struct v4l2_requestbuffers req;
	unsigned int page_size;
	page_size = getpagesize();
	buffer_size = (buffer_size + page_size - 1) & ~(page_size - 1);
	CLEAR(req);
	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_USERPTR;
	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			printf(" does not support user pointer i/o\n");
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_REQBUFS");
		}
	}
	buffers = (struct buffer *)calloc(4, sizeof(*buffers));
	if (!buffers) {
		printf("Out of memory\n");
		exit(EXIT_FAILURE);
	}
	for (n_buffers = 0; n_buffers < 4; ++n_buffers) {
		buffers[n_buffers].length = buffer_size;
		buffers[n_buffers].start = memalign( /* boundary */ page_size,
						    buffer_size);

		if (!buffers[n_buffers].start) {
			printf("Out of memory\n");
			exit(EXIT_FAILURE);
		}
	}
}

static int v4l_init_device(struct encoder_params_s *p, struct capture_parameters_s *c)
{
	struct v4l2_capability cap;
	struct v4l2_cropcap cropcap;
	struct v4l2_crop crop;
	struct v4l2_format fmt;
	unsigned int min;
	int i, k, l;

	encoder_params = p;
	encoder_params->input_fourcc = E_FOURCC_YUY2;
	c->width = 720;
	c->height = 480;

	g_syncStall = c->v4l.syncstall;

	if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
		if (EINVAL == errno) {
			printf("is no V4L2 device\n");
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_QUERYCAP");
		}
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		printf(" is no video capture device\n");
		exit(EXIT_FAILURE);
	}

	if (-1 == xioctl(fd, VIDIOC_S_INPUT, &c->v4l.inputnr)) {
		printf(" set input failed\n");
		exit(EXIT_FAILURE);
	}
	g_inputnr = c->v4l.inputnr;

	switch (io) {
	case IO_METHOD_READ:
		if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
			printf("does not support read i/o\n");
			exit(EXIT_FAILURE);
		}

		break;
	case IO_METHOD_MMAP:
	case IO_METHOD_USERPTR:
		if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
			printf(" does not support streaming i/o\n");
			exit(EXIT_FAILURE);
		}
		break;
	}
	/* Select video input, video standard and tune here. */
	CLEAR(cropcap);
	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect;	/* reset to default */
		if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
			switch (errno) {
			case EINVAL:
				/* Cropping not supported. */
				break;
			default:
				/* Errors ignored. */
				break;
			}
		}
	} else {
		/* Errors ignored. */
	}
	struct v4l2_fmtdesc fmtdesc;
	printf("video capture\n");
	for (i = 0;; i++) {
		memset(&fmtdesc, 0, sizeof(fmtdesc));
		fmtdesc.index = i;
		fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (-1 == xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc))
			break;
		printf("    VIDIOC_ENUM_FMT(%d,VIDEO_CAPTURE)\n", i);
		printf("pfmt: 0x%x %s\n", fmtdesc.pixelformat,
		       fmtdesc.description);
		if (fmtdesc.pixelformat != 0x56595559) {
			printf("   => don't list not supported format\n");
			continue;
		}
		for (k = 0;; k++) {
			struct v4l2_frmsizeenum frmsize;
			memset(&frmsize, 0, sizeof(frmsize));
			frmsize.index = k;
			frmsize.pixel_format = fmtdesc.pixelformat;
			if (-1 == xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize))
				break;
			if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
				printf
				    ("       VIDIOC_ENUM_FRAMESIZES(%d,0x%x) %dx%d  @",
				     k, frmsize.type, frmsize.discrete.width,
				     frmsize.discrete.height);
				for (l = 0;; l++) {
					struct v4l2_frmivalenum frmrate;
					memset(&frmrate, 0, sizeof(frmrate));
					frmrate.index = l;
					frmrate.pixel_format =
					    fmtdesc.pixelformat;
					frmrate.width = frmsize.discrete.width;
					frmrate.height =
					    frmsize.discrete.height;
					if (-1 ==
					    xioctl(fd,
						   VIDIOC_ENUM_FRAMEINTERVALS,
						   &frmrate))
						break;
					if (frmrate.type ==
					    V4L2_FRMIVAL_TYPE_DISCRETE) {
						printf(" %u/%u ",
						       frmrate.discrete.
						       numerator,
						       frmrate.discrete.
						       denominator);
					}
				}
				printf("\n");
			}
		}
	}
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
		errno_exit("VIDIOC_G_FMT");
	printf("video: %dx%d; fourcc:0x%x\n", fmt.fmt.pix.width,
	       fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);
	if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height
	    || (fmt.fmt.pix.pixelformat != pixelformat)) {
		struct v4l2_pix_format def_format;
		memcpy(&def_format, &fmt.fmt.pix,
		       sizeof(struct v4l2_pix_format));
		fmt.fmt.pix.width = width;
		fmt.fmt.pix.height = height;
		fmt.fmt.pix.pixelformat = pixelformat;
		if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt)) {
			printf("failed to set resolution %d x %d\n",
			       fmt.fmt.pix.width, fmt.fmt.pix.height);
			errno_exit("VIDIOC_S_FMT");
		}
	}
	if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
		errno_exit("VIDIOC_S_FMT");

	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
		errno_exit("VIDIOC_G_FMT");
	printf("video: %dx%d; fourcc:0x%x\n", fmt.fmt.pix.width,
	       fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);
	if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height
	    || fmt.fmt.pix.pixelformat != pixelformat) {
		errno_exit("VIDIOC_S_FMT not set !");
	}

	/* Buggy driver paranoia. */
	min = fmt.fmt.pix.width * 2;
	if (fmt.fmt.pix.bytesperline < min)
		fmt.fmt.pix.bytesperline = min;
	min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
	if (fmt.fmt.pix.sizeimage < min)
		fmt.fmt.pix.sizeimage = min;
	struct v4l2_streamparm capp;
	CLEAR(capp);
	capp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (g_V4LFrameRate) {
		if (-1 == (xioctl(fd, VIDIOC_G_PARM, &capp) == -1)) {
			errno_exit("VIDIOC_G_PARM");
		}
		printf("vidioc_s_parm called frate=%d/%d\n",
		       capp.parm.capture.timeperframe.numerator,
		       capp.parm.capture.timeperframe.denominator);
		capp.parm.capture.timeperframe.numerator = g_V4LNumerator;
		capp.parm.capture.timeperframe.denominator = g_V4LFrameRate;
		printf("vidioc_s_parm set: frate=%d/%d\n",
		       capp.parm.capture.timeperframe.numerator,
		       capp.parm.capture.timeperframe.denominator);
#if 0
		/* KL: Disabled
		 * CX23885 set framerate call fails, driver doesn't support anything
		 * other than its default, and the API isn't implemented.
		 */
		if (-1 == (xioctl(fd, VIDIOC_S_PARM, &capp) == -1)) {
			errno_exit("VIDIOC_S_PARM");
		}
		if (-1 == (xioctl(fd, VIDIOC_G_PARM, &capp) == -1)) {
			errno_exit("VIDIOC_G_PARM");
		}
		if ((capp.parm.capture.timeperframe.numerator != g_V4LNumerator)
		    || (capp.parm.capture.timeperframe.denominator !=
			g_V4LFrameRate)) {
			errno_exit("VIDIOC_S_PARM NOT SET");
		}
#endif
	} else {
		if (-1 == (xioctl(fd, VIDIOC_G_PARM, &capp) == -1)) {
			errno_exit("VIDIOC_G_PARM");
		}
	}
	sprintf(device_settings_buffer, "%dx%d@%d/%d", width, height,
		capp.parm.capture.timeperframe.numerator,
		capp.parm.capture.timeperframe.denominator);
	device_settings = &device_settings_buffer[0];
	printf("INFO: %s\n", device_settings);

	switch (io) {
	case IO_METHOD_READ:
		init_read(fmt.fmt.pix.sizeimage);
		break;

	case IO_METHOD_MMAP:
		init_mmap();
		break;

	case IO_METHOD_USERPTR:
		init_userp(fmt.fmt.pix.sizeimage);
		break;
	}

	return 0;
}

static void v4l_close_device(void)
{
	if (-1 == close(fd))
		errno_exit("close");

	fd = -1;
}

static int v4l_open_device(void)
{
	struct stat st;

	if (-1 == stat(v4l_dev_name, &st)) {
		printf("Cannot identify [%s]\n", v4l_dev_name);
		exit(EXIT_FAILURE);
	}

	if (!S_ISCHR(st.st_mode)) {
		printf("is no device\n");
		exit(EXIT_FAILURE);
	}

	fd = open(v4l_dev_name, O_RDWR /* required */  | O_NONBLOCK, 0);

	if (-1 == fd) {
		printf("Cannot open v4l_dev_name\n");
		exit(EXIT_FAILURE);
	}

	return 0;
}

static void v4l_set_defaults(struct capture_parameters_s *c)
{
	c->type = CM_V4L;
	c->v4l.inputnr = 0;
	c->v4l.syncstall = 0;
}

struct capture_operations_s v4l_ops = 
{
	.type		= CM_V4L,
	.name		= "V4L Device",
	.set_defaults	= v4l_set_defaults,
	.mainloop	= v4l_mainloop,
	.stop		= v4l_stop_capturing,
	.start		= v4l_start_capturing,
	.uninit		= v4l_uninit_device,
	.init		= v4l_init_device,
	.close		= v4l_close_device,
	.open		= v4l_open_device,
	.default_fps	= 60,
};

