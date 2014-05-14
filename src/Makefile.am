bin_PROGRAMS = h264encoder

h264encoder_CFLAGS = \
	-D_BSD_SOURCE -D_XOPEN_SOURCE \
	-DHAVE_VA_DRM -DHAVE_VA_X11 \
	@PTHREAD_CFLAGS@ \
	@LIBVA_CFLAGS@ \
	@LIBAV_CFLAGS@ \
	@LIBES2TS_CFLAGS@ \
	@LIBIPCVIDEO_CFLAGS@ \
	@X11_CFLAGS@
h264encoder_LDADD = \
	-lm -lyuv \
	@PTHREAD_LIBS@ \
	@LIBVA_LIBS@ \
	@LIBAV_LIBS@ \
	@LIBES2TS_LIBS@ \
	@LIBIPCVIDEO_LIBS@ \
	@X11_LIBS@
h264encoder_SOURCES = \
	encoder.c \
	encoder.h \
	encoder-display.c \
	encoder-display.h \
	es2ts.c \
	es2ts.h \
	ipcvideo.c \
	ipcvideo.h \
	loadsurface.h \
	loadsurface_yuv.h \
	main.c \
	main.h \
	rtp.c \
	rtp.h \
	v4l.c \
	v4l.h \
	fixed.c \
	fixed.h \
	fixed-frame.h \
	va_display.c \
	va_display.h \
	va_display_drm.c \
	va_display_x11.c
