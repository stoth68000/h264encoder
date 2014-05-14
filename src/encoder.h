int  encoder_init(int width, int height, int enable_osd, int deinterlacemode);
void encoder_close();
int  encoder_encode_frame(unsigned char *inbuf);
