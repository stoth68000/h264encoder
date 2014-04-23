
CC=gcc

CFLAGS  = -std=c99
CFLAGS  = -Wall
CFLAGS += -g
CFLAGS += -D_BSD_SOURCE
CFLAGS += -D_XOPEN_SOURCE
CFLAGS += -DHAVE_VA_DRM -DHAVE_VA_X11

INCFLAGS  = -I/KL/libyuv-read-only/include
INCFLAGS += -I/KL/libva-1.2.1/root/include

LDFLAGS += -lpthread -lm -lavformat -L/KL/libyuv-read-only -lyuv -lavcodec -lX11
LDFLAGS += -lipcvideo
LDFLAGS += -les2ts -lavutil
LDFLAGS += -L/KL/libva-1.2.1/root/lib -lva -lva-drm -lva-x11

SOURCES=es2ts.c ipcvideo.c rtp.c encoder.c v4l.c main.c \
	va_display.c va_display_x11.c va_display_drm.c

OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=h264encoder

all:	objdir $(SOURCES) $(EXECUTABLE)

objdir:
	mkdir -p obj

.c.o:
	$(CC) $(CFLAGS) $(INCFLAGS) -c $< -o obj/$(@)

$(EXECUTABLE): $(OBJECTS) 
	$(CC) obj/*.o $(LDFLAGS) -o $@

clean:
	rm -f $(EXECUTABLE)
	rm -rf obj

tarball:
	cd .. && tar zcf IMSCO-h264encoder-$(shell date +%Y%m%d-%H%M%S).tgz --exclude-vcs ./IMSCO-h264encoder

