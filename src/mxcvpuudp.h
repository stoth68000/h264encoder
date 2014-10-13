
/* Broadcast Packets specific to the freescale mxc_vpu_test udp test app */

void freeMXCVPUUDPHandler();
int  initMXCVPUUDPHandler(char *ipaddress, int port, int dscp, int sendsize, int ifd, int bigendian, int send_mode);
int  sendMXCVPUUDPPacket(unsigned char *nal, int len, int frame_type);

int  validateMXCVPUUDPOutput(char *filename, int bigendian);
