#include "encoder.h"

void ipcvideo_mainloop(void);
void ipcvideo_stop_capturing(void);
void ipcvideo_start_capturing(void);
void ipcvideo_uninit_device(void);
int ipcvideo_init_device(struct encoder_params_s *p, unsigned int *width, unsigned int *height, int fps);
void ipcvideo_close_device(void);
int  ipcvideo_open_device();
