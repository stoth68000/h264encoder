
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
extern unsigned int encoder_frame_bitrate;


void v4l_mainloop(void);
void stop_v4l_capturing(void);
void start_v4l_capturing(void);
void uninit_v4l_device(void);
void init_v4l_device(void);
void close_v4l_device(void);
void open_v4l_device(void);

