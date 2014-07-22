#ifndef ENCODER_H
#define ENCODER_H

#include <stdio.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#define IS_YUY2(p) ((p)->input_fourcc == E_FOURCC_YUY2)
#define IS_BGRX(p) ((p)->input_fourcc == E_FOURCC_BGRX)

#define CHECK_VASTATUS(va_status,func)                                  \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr,"%s:%s (%d) failed, exit %d\n", __func__, func, __LINE__, va_status); \
        exit(1);                                                        \
    }

enum fourcc_e {
	E_FOURCC_UNDEFINED = 0,
	E_FOURCC_YUY2,
	E_FOURCC_BGRX,
};

struct encoder_params_s
{
	unsigned int width;
	unsigned int height;
	unsigned int enable_osd;
	unsigned int deinterlacemode;
	unsigned int initial_qp;
	unsigned int minimal_qp;

	unsigned int intra_period;
	unsigned int idr_period;
	unsigned int ip_period;
	VAProfile h264_profile;
	unsigned int h264_entropy_mode;
	int rc_mode;

	unsigned int frame_bitrate;
	enum fourcc_e input_fourcc;
};

extern FILE *csv_fp;
extern int quiet_encode;

int  encoder_init(struct encoder_params_s *params);
void encoder_param_defaults(struct encoder_params_s *p);
void encoder_close(struct encoder_params_s *params);
int  encoder_encode_frame(struct encoder_params_s *params, unsigned char *inbuf);
int  encoder_string_to_rc(char *str);
char *encoder_rc_to_string(int rcmode);
int  encoder_string_to_profile(char *str);
char *encoder_profile_to_string(int profile);

#endif
