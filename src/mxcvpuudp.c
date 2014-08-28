#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* TODO: user context required */
static int skt = -1;
static struct sockaddr_in udpsock;
static unsigned int seqno = 0;
static int be_mode = 0;

/* Freeslace - custom header, taken from mxc_vpu_test/utils.c */
/* No concept of endian, no concept of the size of an int.
 * Generally considered bad form to put a raw struct to/from the
 * network.
 */
struct nethdr {
	int seqno;
	int iframe;
	int len;
};

static struct nethdr pkt_header;

#define ENDIAN_SWAP(n) \
		(((n) & 0xff000000) >> 24) | \
		(((n) & 0x00ff0000) >>  8) | \
		(((n) & 0x0000ff00) <<  8) | \
		(((n) & 0x000000ff) << 24);

static void nethdr_to_be(struct nethdr *h)
{
	h->seqno = ENDIAN_SWAP(h->seqno);
	h->iframe = ENDIAN_SWAP(h->iframe);
	h->len = ENDIAN_SWAP(h->len);
}

void freeMXCVPUUDPHandler()
{
	if (skt != -1) {
		close(skt);
		skt = -1;
	}
}

int initMXCVPUUDPHandler(char *ipaddress, int port, int sendsize, int big_endian)
{
	if (!ipaddress || (port < 1024 || (port > 65535)))
		return -1;

	be_mode = big_endian & 1;

	skt = socket(AF_INET, SOCK_DGRAM, 0);
	if (skt < 0) {
		fprintf(stderr, "socket() failed\n");
		return -2;
	}

	memset((char *) &udpsock, 0, sizeof(udpsock));
	udpsock.sin_family = AF_INET;
	udpsock.sin_addr.s_addr = inet_addr(ipaddress);
	udpsock.sin_port = htons(port);
   
	if (inet_aton(ipaddress, &udpsock.sin_addr) == 0) {
		fprintf(stderr, "inet_aton() failed\n");
		close(skt);
		return -2;
	}

	int s = sendsize;
	if (setsockopt(skt, SOL_SOCKET, SO_SNDBUF, (char *)&s, (int)sizeof(s)) < 0) {
		fprintf(stderr, "Setting local interface error, %s\n", strerror(errno));
		close(skt);
		return -1;
        }

	printf("%s() configured for use.\n", __func__);
	memset(&pkt_header, 0, sizeof(pkt_header));

	return 0;
}

int sendMXCVPUUDPPacket(unsigned char *nal, int len)
{
	/* The encoder will feed us regardless, just OK
	 * the transaction if we're not enabled.
	 */
	if (skt == -1)
		return 0;

	int sendlen = len + sizeof(pkt_header);
	unsigned char *buf = malloc(sendlen);
	if (!buf)
		return -1;

	pkt_header.seqno = seqno++;
	pkt_header.iframe = 0;
	pkt_header.len = len;

	/* Construct a proprietary network frame, in whichever
	 * format freescale prefers, big or little endian.
	 */
	memcpy(buf, &pkt_header, sizeof(pkt_header));
	if (be_mode)
		nethdr_to_be((struct nethdr *)buf);
	memcpy(buf + sizeof(pkt_header), nal, len);

	int ret = sendto(skt, buf, sendlen, 0, (struct sockaddr*)&udpsock, sizeof(udpsock));
	if (ret < 0) {
		fprintf(stderr, "Sending %d byte datagram message error, %s\n", sendlen, strerror(errno));
	}
	free(buf);

	return 0;
}
