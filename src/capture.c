#include "capture.h"

static struct capture_module_s {
	unsigned int type;
	struct capture_operations_s *operations;
} capture_modules[] = {
	{ CM_V4L,	&v4l_ops },
	{ CM_IPCVIDEO,	&ipcvideo_ops },
	{ CM_FIXED,	&fixed_ops },
	{ CM_FIXED_4K,	&fixed_4k_ops },
};

struct capture_operations_s *getCaptureSource(unsigned int type)
{
	for (unsigned int i = 0; i < (sizeof(capture_modules) / sizeof(struct capture_module_s)); i++) {
		if (type == capture_modules[i].type)
			return capture_modules[i].operations;
	}

	return 0;
}

