#include "encoder.h"

void fixed_mainloop(void);
void fixed_stop_capturing(void);
void fixed_start_capturing(void);
void fixed_uninit_device(void);
int fixed_init_device(struct encoder_params_s *params, unsigned int *width, unsigned int *height, int fps);
void fixed_close_device(void);
int  fixed_open_device();
