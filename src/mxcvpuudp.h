
/* Broadcast Packets specific to the freescale mxc_vpu_test udp test app */

void freeMXCVPUUDPHandler();
int  initMXCVPUUDPHandler(char *ipaddress, int port, int sendsize, int bigendian, int send_mode);
int  sendMXCVPUUDPPacket(unsigned char *nal, int len);

int  validateMXCVPUUDPOutput(char *filename, int bigendian);