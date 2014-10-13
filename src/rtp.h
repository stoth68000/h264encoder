void freeRTPHandler();
int initRTPHandler(char *ipaddress, int port, int dscp, int pktsize, int ifd, int w, int h, int fps);
int sendRTPPacket(unsigned char *nal, int len, int frame_type);
