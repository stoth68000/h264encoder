void freeRTPHandler();
int initRTPHandler(char *ipaddress, int port, int w, int h, int fps);
int sendRTPPacket(unsigned char *nal, int len);
