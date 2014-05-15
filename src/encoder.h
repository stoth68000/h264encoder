
struct encoder_params_s
{
	unsigned int width;
	unsigned int height;
	unsigned int enable_osd;
	unsigned int deinterlacemode;
	unsigned int initial_qp;
};

int  encoder_init(struct encoder_params_s *params);
void encoder_close();
int  encoder_encode_frame(unsigned char *inbuf);


