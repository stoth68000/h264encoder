#ifndef CAPTURE_H
#define CAPTURE_H

#include "v4l.h"
#include "ipcvideo.h"
#include "fixed.h"
#include "fixed-4k.h"
#include "encoder.h"

struct capture_parameters_s
{
	enum {
		CM_V4L = 0,
		CM_IPCVIDEO = 1,
		CM_FIXED = 2,
		CM_FIXED_4K = 3,
		CM_MAX
	} type;

	unsigned int fps;
	unsigned int width;
	unsigned int height;

	struct capture_v4l_params_s v4l;
};

struct capture_operations_s
{
	unsigned int type;
	char *name;

	void (*set_defaults)(struct capture_parameters_s *);
	void (*mainloop)(void);
	void (*stop)(void);
	int  (*start)(struct encoder_operations_s *encoder);
	void (*uninit)(void);
	int  (*init)(struct encoder_params_s *, struct capture_parameters_s *);
	void (*close)(void);
	int  (*open)(void);

	unsigned int default_fps;
};

struct capture_operations_s *getCaptureSource(unsigned int type);

#endif //CAPTURE_H

