
#ifndef V4L_H
#define V4L_H

struct capture_v4l_params_s {
	int inputnr;
	unsigned int syncstall;
};

typedef enum {
        IO_METHOD_READ,
        IO_METHOD_MMAP,
        IO_METHOD_USERPTR,
} io_method;

extern io_method io;
extern char *v4l_dev_name;

extern unsigned int width;
extern unsigned int height; 
extern unsigned int g_V4LNumerator;
extern unsigned int g_V4LFrameRate;
extern char *encoder_nalOutputFilename;

extern struct capture_operations_s v4l_ops;

#endif // V4L_H
