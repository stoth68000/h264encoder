int sendESPacket(unsigned char *nal, int len);
int initESHandler(char *ipaddress, int port, int dscp, int pktsize, int w, int h, int fps);
void freeESHandler();
