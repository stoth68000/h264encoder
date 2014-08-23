#include <stdio.h>
#include <libes2ts/es2ts.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

/* Compatibility with older versions of ffmpeg */
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(54,59,100)
# define AV_CODEC_ID_MPEG2TS CODEC_ID_MPEG2TS
#endif

/* A combined ES to TS layer, where we convert nals into TS packets
 * using libes2ts then push the ts buffers out to RTP.
 */
static struct es2ts_context_s *es2ts_ctx = 0;
static AVFormatContext *tsav_ctx = NULL;
static AVOutputFormat *tsav_fmt = NULL;
static AVStream *tsav_strm = NULL;
static int tsav_ifd = 0;

static int es2ts_initRTPHandler(char *ipaddress, int port, int dscp, int pktsize, int ifd, int w, int h, int fps)
{
	char filename[64];

	avformat_network_init();
	av_register_all();

	tsav_ctx = avformat_alloc_context();
	if (!tsav_ctx)
		return -1;

	tsav_fmt = av_guess_format("rtp", NULL, NULL);
	if (!tsav_ctx) {
		avformat_free_context(tsav_ctx);
		return -1;
	}

	tsav_ctx->oformat = tsav_fmt;
	tsav_ifd = ifd;

	/* try to open the RTP stream */
	snprintf(filename, sizeof(filename), "rtp://%s:%d?dscp=%d&pkt_size=%d", ipaddress, port,
		 dscp, pktsize?pktsize:-1);
	printf("Streaming to %s\n", filename);
	if (avio_open(&(tsav_ctx->pb), filename, AVIO_FLAG_WRITE) < 0) {
		printf("Couldn't open RTP output stream\n");
		avformat_free_context(tsav_ctx);
		return -1;
	}

	/* add an H.264 stream */
	tsav_strm = avformat_new_stream(tsav_ctx, NULL);
	if (!tsav_strm) {
		printf("Couldn't allocate H.264 stream\n");
		avformat_free_context(tsav_ctx);
		return -1;
	}

	/* initalize codec */
	AVCodecContext* c = tsav_strm->codec;
	c->codec_id = AV_CODEC_ID_MPEG2TS;
	c->codec_type = AVMEDIA_TYPE_VIDEO;
	c->bit_rate = 3000000;
	c->width = w;
	c->height = h;
	c->time_base.den = fps;
	c->time_base.num = 1;

	/* write the header */
	if (avformat_write_header(tsav_ctx, NULL) != 0)
		printf("%s() error write header\n", __func__);
	
	return 0;
}

static int es2ts_sendTSBufferAsRTP(unsigned char *tsbuf, int len)
{
	if (tsav_ctx == NULL)
		return 0;
	if (tsav_ifd > 0) {
		av_opt_set_int(tsav_ctx, "ifd", tsav_ifd, AV_OPT_SEARCH_CHILDREN);
		tsav_ifd = 0;
	}
#if 1
	AVPacket p;
	av_init_packet(&p);
	p.data = tsbuf;
	p.size = len;
	p.stream_index = tsav_strm->index;

	av_write_frame(tsav_ctx, &p);
#else
	int rem = len;
	int idx = 0;
	while (rem > 0) {
		int cnt = rem;
		if ((7 * 188) < rem)
			cnt = 7 * 188;

		AVPacket p;
		av_init_packet(&p);
		p.data = tsbuf + idx;
		p.size = cnt;
		p.stream_index = tsav_strm->index;

		av_write_frame(tsav_ctx, &p);

		idx += cnt;
		rem -= cnt;
	}
#endif
	return 0;
}

/* The es2ts library calls us back with buffers of TS packets .... */
/* Callbacks are guaranteed to be exact multiples of 188 bytes */
int downstream_callback(struct es2ts_context_s *ctx, unsigned char *buf, int len)
{
	if (len % 188) {
		printf("%s(%p, %p, bytes %d) [ %02x %02x %02x %02x] %d/%d\n",
			__func__, ctx, buf, len,
			*(buf + 0),
			*(buf + 1),
			*(buf + 2),
			*(buf + 3),
			len / 188, len % 188
			);
	}
#if 0
	static FILE *fh = 0;
	if (fh == 0) {
		fh = fopen("output.ts", "wb");
		if (!fh) {
			fprintf(stderr, "could not open output file\n");
			exit(1);
		}
	}

	fwrite(buf, 1, len, fh);
#endif

	es2ts_sendTSBufferAsRTP(buf, len);

	return ES2TS_OK;
}

int sendESPacket(unsigned char *nal, int len)
{
	if ((es2ts_ctx == NULL) || (!nal))
		return 0; /* Success */

	/* Upstream application pushed data into the library */
	int ret = es2ts_data_enqueue(es2ts_ctx, nal, len);
	if (ES2TS_FAILED(ret)) {
		return -1;
	}

	return 0;
}

int initESHandler(char *ipaddress, int port, int dscp, int pktsize, int ifd, int w, int h, int fps)
{
	int ret;

	ret = es2ts_initRTPHandler(ipaddress, port, dscp, pktsize, ifd, w, h, fps);
	if (ret < 0)
		return -1;

	ret = es2ts_alloc(&es2ts_ctx);
	if (ES2TS_FAILED(ret))
		return -1;

	printf("Allocated a context %p\n", es2ts_ctx);
	ret = es2ts_callback_register(es2ts_ctx, &downstream_callback);
	if (ES2TS_FAILED(ret))
		return -1;

	printf("Callback registered\n");

	ret = es2ts_process_start(es2ts_ctx);
	if (ES2TS_FAILED(ret))
		return -1;

	printf("Process Started\n");

	return 0;
}

void freeESHandler()
{
	if (tsav_ctx)
		avformat_free_context(tsav_ctx);

	if (es2ts_ctx == NULL)
		return;

	printf("Process ending\n");
	int ret = es2ts_process_end(es2ts_ctx);
	if (ES2TS_FAILED(ret)) {
		fprintf(stderr, "%s() failed to terminate\n", __func__);
		return;
	}

	printf("Process ended\n");
	es2ts_callback_unregister(es2ts_ctx);
	es2ts_free(es2ts_ctx);
	es2ts_ctx = 0;
}

