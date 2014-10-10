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
static int send_mode = 1;

/* Freeslace - custom header, taken from mxc_vpu_test/utils.c */
/* No concept of endian, no concept of the size of an int.
 * Generally considered bad form to put a raw struct to/from the
 * network.
 */
struct nethdr {
	int seqno;
	int iframe;
	int len;
} __attribute__ ((__packed__));

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

int initMXCVPUUDPHandler(char *ipaddress, int port, int sendsize, int big_endian, int mode)
{
	if (!ipaddress || (port < 1024 || (port > 65535)))
		return -1;

	be_mode = big_endian & 1;
	send_mode = mode;

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

/* Send a header and full nal in a single transaction */
static int sendMXCVPUUDPPacket_1(unsigned char *nal, int len)
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

	/* Roll the seq no for every major nal, don't roll it when we fragment */
	pkt_header.seqno = seqno++;
	pkt_header.iframe = 1;
	pkt_header.len = len;

	/* Construct a proprietary network frame, in whichever
	 * format freescale prefers, big or little endian.
	 */
	memcpy(buf, &pkt_header, sizeof(pkt_header));
	if (be_mode)
		nethdr_to_be((struct nethdr *)buf);
	memcpy(buf + sizeof(pkt_header), nal, len);

	/* Header and nal in one complete write */
	int ret = sendto(skt, buf, sendlen, 0, (struct sockaddr*)&udpsock, sizeof(udpsock));
	if (ret < 0) {
		fprintf(stderr, "Sending %d byte datagram message error, %s\n", sendlen, strerror(errno));
	}
	free(buf);

	return 0;
}

/* Send nal fragments with each fragment having a header */
static int sendMXCVPUUDPPacket_2(unsigned char *nal, int len)
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

	/* Roll the seq no for every major nal, don't roll it when we fragment */
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

	/* One header per fragment */
	int netlen = 24 * 1024;
	unsigned char *p = buf + sizeof(pkt_header);
	for (int i = 0; i < len; i += netlen) {

		int sl;
		if ((i + netlen) < len)
			sl = netlen;
		else
			sl = len - i;

		pkt_header.len = sl;
		memcpy(buf, &pkt_header, sizeof(pkt_header));
		if (be_mode)
			nethdr_to_be((struct nethdr *)buf);

		/* Send the nal header */
		int ret = sendto(skt, buf, sizeof(pkt_header), 0, (struct sockaddr*)&udpsock, sizeof(udpsock));
		if (ret < 0) {
			fprintf(stderr, "Sending %d byte header message error, %s\n", sendlen, strerror(errno));
		}

		/* Send the nal data fragment */
		ret = sendto(skt, p, sl, 0, (struct sockaddr*)&udpsock, sizeof(udpsock));
		if (ret < 0) {
			fprintf(stderr, "Sending %d byte datagram message error, %s\n", sl, strerror(errno));
		}

		p += sl;
	}
	free(buf);

	return 0;
}

/* Send a single header then lots of nal fragments without a header */
static int sendMXCVPUUDPPacket_3(unsigned char *nal, int len)
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

	/* Roll the seq no for every major nal, don't roll it when we fragment */
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

	/* single header, multi-fragment */
	/* Send the nal header */
	int ret = sendto(skt, buf, sizeof(pkt_header), 0, (struct sockaddr*)&udpsock, sizeof(udpsock));
	if (ret < 0) {
		fprintf(stderr, "Sending %d byte header message error, %s\n", sendlen, strerror(errno));
	}

	int netlen = 24 * 1024;
	unsigned char *p = buf + sizeof(pkt_header);
	for (int i = 0; i < len; i += netlen) {

		int sl;
		if ((i + netlen) < len)
			sl = netlen;
		else
			sl = len - i;

		/* Send the nal data fragment */
		ret = sendto(skt, p, sl, 0, (struct sockaddr*)&udpsock, sizeof(udpsock));
		if (ret < 0) {
			fprintf(stderr, "Sending %d byte datagram message error, %s\n", sl, strerror(errno));
		}

		p += sl;
	}
	free(buf);

	return 0;
}

int sendMXCVPUUDPPacket(unsigned char *nal, int len)
{
	if (send_mode == 1)
		return sendMXCVPUUDPPacket_1(nal, len);
	if (send_mode == 2)
		return sendMXCVPUUDPPacket_2(nal, len);
	if (send_mode == 3)
		return sendMXCVPUUDPPacket_3(nal, len);

	return 0;
}

int validateMXCVPUUDPOutput(char *filename, int big_endian)
{
	int isok = 1;
	int old_seqno;
	pkt_header.seqno = -1;

	if (!filename)
		return -1;

	be_mode = big_endian & 1;
	FILE *fh = fopen(filename, "rb");

	while (!feof(fh)) {
		old_seqno = pkt_header.seqno;
		int l = fread(&pkt_header, sizeof(pkt_header), 1, fh);
		if (feof(fh))
			break;

		if (l != 1) {
			isok = 0;
			break;
		}

		if (be_mode)
			nethdr_to_be(&pkt_header);

		printf("seq:%08d iframe:%1d len:%08d[0x%08x] ...\n",
			pkt_header.seqno, 
			pkt_header.iframe, 
			pkt_header.len, pkt_header.len);

		if (old_seqno + 1 != pkt_header.seqno) {
			printf("Illegal sequence number\n");
			isok = 0;
			break;
		}

		unsigned char *buf = malloc(pkt_header.len);
		if (!buf) {	
			isok = 0;
			break;
		}
		l = fread(buf, 1, pkt_header.len, fh); 
		if (feof(fh))
			break;
		if (l != pkt_header.len) {
			isok = 0;
			break;
		}

		free(buf);
	}

	return isok;
}

