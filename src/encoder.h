
#include <va/va.h>

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
};

extern FILE *csv_fp;
extern int quiet_encode;

int  encoder_init(struct encoder_params_s *params);
void encoder_param_defaults(struct encoder_params_s *p);
void encoder_close();
int  encoder_encode_frame(unsigned char *inbuf);
int  encoder_string_to_rc(char *str);
char *encoder_rc_to_string(int rcmode);
int  encoder_string_to_profile(char *str);
char *encoder_profile_to_string(int profile);

