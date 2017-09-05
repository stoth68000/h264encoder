/*
 *  H264 Encoder - Capture YUV, compress via VA-API and stream to RTP.
 *  Original code base was the vaapi h264encode application, with 
 *  significant additions to support capture, transform, compress
 *  and re-containering via libavformat.
 *
 *  Copyright (c) 2014-2017 Steven Toth <stoth@kernellabs.com>
 *  Copyright (c) 2014-2017 Zodiac Inflight Innovations
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "frames.h"

/* TODO: user context required */
static int skt = -1;
static struct sockaddr_in udpsock;
static unsigned int seqno = 0;
static int be_mode = 0;
static int send_mode = 2;
static int interframe_delay = 0;

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

/* Each packet we push to the network contains this header. */
struct nethdr2 {

	/* During testing, this value was always 1 and it worked reliably */

	/* These are a direct clone from encoder.c */
	/* bits 1:0 frame type, FRAME_I etc */
#define FLAG_FRAME_I		0x00
#define FLAG_FRAME_IDR		0x01
#define FLAG_FRAME_B		0x02
#define FLAG_FRAME_P		0x03
	/* bits 5:4 unused */
#define FLAGS_FRAG_START	0x40
#define FLAGS_FRAG_END		0x80
	unsigned char flags;

	/* An incrementing value that represents an entire nal */
	unsigned int seq_no;

	/* Total length of the entire nal (0-large number), not including any nethdr2 structs */
	unsigned int seq_len;

	/* nals can exceed 64KB so we split them into pieces, each new seqno has
	 * a fragno of zero. Fragno increments each time a fragment is broadcast.
	 */
	/* Number of nal bytes included in this transaction (0-65535) */
	unsigned char frag_no;
	unsigned short frag_len;
} __attribute__ ((__packed__));

#if 0
static void dump_nethdr2(struct nethdr2 *h)
{
	printf("seqno: %08x len: %08x fragno: %04x len: %08x flags=%x\n",
		h->seq_no, h->seq_len,
		h->frag_no, h->frag_len,
		h->flags);
}
#endif

static struct nethdr pkt_header;
static struct nethdr2 pkt_header2;

#define ENDIAN_SWAP_U32(n) \
		(((n) & 0xff000000) >> 24) | \
		(((n) & 0x00ff0000) >>  8) | \
		(((n) & 0x0000ff00) <<  8) | \
		(((n) & 0x000000ff) << 24);
#define ENDIAN_SWAP_U16(n) \
		(((n) & 0xff00) >> 8) | \
		(((n) & 0x00ff) << 8);

static void nethdr_to_be(struct nethdr *h)
{
	h->seqno = ENDIAN_SWAP_U32(h->seqno);
	h->iframe = ENDIAN_SWAP_U32(h->iframe);
	h->len = ENDIAN_SWAP_U32(h->len);
}

static void nethdr2_to_be(struct nethdr2 *h)
{
	//h->flags = ENDIAN_SWAP_U32(h->flags);
	h->seq_no  = ENDIAN_SWAP_U32(h->seq_no);
	h->seq_len = ENDIAN_SWAP_U32(h->seq_len);
	//h->frag_no = ENDIAN_SWAP_U32(h->frag_no);
	h->frag_len = ENDIAN_SWAP_U16(h->frag_len);
}

void freeMXCVPUUDPHandler()
{
	if (skt != -1) {
		close(skt);
		skt = -1;
	}
}

int initMXCVPUUDPHandler(char *ipaddress, int port, int dscp, int sendsize, int ifd, int big_endian, int mode)
{
	if (!ipaddress || (port < 1024 || (port > 65535) || (ifd < 0)))
		return -1;

	be_mode = big_endian & 1;
	send_mode = mode;
	interframe_delay = ifd; /* microsecond delay between mode 2 frame transmits */

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

	if (dscp >= 0) {
		/* Apprently we shift this two places, see vincents ffmpeg patch, validated by
		 * other posts on the web also.
		 */
		dscp <<= 2;
		if (setsockopt(skt, IPPROTO_IP, IP_TOS, &dscp, sizeof(dscp)) != 0) {
			fprintf(stderr, "Setting dscp, %s\n", strerror(errno));
			close(skt);
			return -1;
		}
	}

	printf("%s() configured for use.\n", __func__);
	memset(&pkt_header, 0, sizeof(pkt_header));

	return 0;
}

/* Convert encoders id for frame types into generic network representation */
static unsigned char encoderFrameTypeToNetwork(int type)
{
	if (type == FRAME_I)
		return FLAG_FRAME_I;
	if (type == FRAME_IDR)
		return FLAG_FRAME_IDR;
	if (type == FRAME_B)
		return FLAG_FRAME_B;

	return FLAG_FRAME_P;
}

/* Send a full nal, spread across multiple packets as necessary, each with their
 * own header.
 */
static int sendMXCVPUUDPPacket_2(unsigned char *nal, int len, int frame_type)
{
	/* Maximum size + header of a single UDP transmit that we'll support.
	 * Must be less than 64KB else Linux refuses to send it.
	 */
	int fraglen = (32 * 1024);
	fraglen = 1300;

	/* The encoder will feed us regardless, just OK
	 * the transaction if we're not enabled.
	 */
	if (skt == -1)
		return 0;

	unsigned char *buf = malloc(fraglen + sizeof(pkt_header2));
	if (!buf)
		return -1;

	/* Roll the seq no for every major nal, don't roll it when we fragment */
	pkt_header2.seq_no = seqno++;
	pkt_header2.seq_len = len;
	pkt_header2.frag_no = 0;
	pkt_header2.frag_len = 0;

	/* One header per fragment */
	unsigned char *p = buf + sizeof(pkt_header2);
	for (int i = 0; i < len; i += fraglen) {

		if ((i + fraglen) < len) {
			pkt_header2.frag_len = fraglen;
		} else {
			pkt_header2.frag_len = len - i;
		}

		pkt_header2.flags = encoderFrameTypeToNetwork(frame_type);
		if ((i + pkt_header2.frag_len) >= len)
			pkt_header2.flags |= FLAGS_FRAG_END;
		if (pkt_header2.frag_no == 0)
			pkt_header2.flags |= FLAGS_FRAG_START;

		memcpy(buf, &pkt_header2, sizeof(pkt_header2));

		/* Take a fragment slice */
		memcpy(buf + sizeof(pkt_header2), nal + i, pkt_header2.frag_len);

		//dump_nethdr2(&pkt_header2);

		/* Prep endianness prior to xmit */
		if (be_mode)
			nethdr2_to_be((struct nethdr2 *)buf);

		/* Send the header + partial fragment */
		int l = sizeof(pkt_header2) + pkt_header2.frag_len;
		int ret = sendto(skt, buf, l, 0, (struct sockaddr*)&udpsock, sizeof(udpsock));
		if (ret < 0)
			fprintf(stderr, "Sending %d byte hdr msg err, %s\n", l,
				strerror(errno));

		if (interframe_delay)
			usleep(interframe_delay);

		p += pkt_header2.frag_len;
		pkt_header2.frag_no++;
	}
	free(buf);

	return 0;
}

/* Send a header and full nal in a single transaction */
static int sendMXCVPUUDPPacket_1(unsigned char *nal, int len, int isIFrame)
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

int sendMXCVPUUDPPacket(unsigned char *nal, int len, int frame_type)
{
	if (send_mode == 1)
		return sendMXCVPUUDPPacket_1(nal, len, frame_type);
	if (send_mode == 2)
		return sendMXCVPUUDPPacket_2(nal, len, frame_type);

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

