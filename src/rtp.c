#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

/* Compatibility with older versions of ffmpeg */
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(54,59,100)
# define AV_CODEC_ID_H264 CODEC_ID_H264
#endif

/* libavformat
 * http://stackoverflow.com/questions/10143753/streaming-h-264-over-rtp-with-libavformat
 */
static AVFormatContext *av_ctx = NULL;
static AVOutputFormat *av_fmt = NULL;
static AVStream *av_strm = NULL;
static int av_ifd = 0;

void freeRTPHandler()
{
	if (av_ctx)
		avformat_free_context(av_ctx);
}

int initRTPHandler(char *ipaddress, int port, int dscp, int pktsize, int ifd, int w, int h, int fps)
{
	char filename[64];

	avformat_network_init();
	av_register_all();

	av_ctx = avformat_alloc_context();
	if (!av_ctx)
		return -1;

	av_fmt = av_guess_format("rtp", NULL, NULL);
	if (!av_ctx) {
		avformat_free_context(av_ctx);
		return -1;
	}

	av_ctx->oformat = av_fmt;
	av_ifd = ifd;

	/* try to open the RTP stream */
	snprintf(filename, sizeof(filename), "rtp://%s:%d?dscp=%d&pkt_size=%d", ipaddress, port,
		 dscp, pktsize?pktsize:-1);
	printf("Streaming to %s\n", filename);
	if (avio_open(&(av_ctx->pb), filename, AVIO_FLAG_WRITE) < 0) {
		printf("Couldn't open RTP output stream\n");
		avformat_free_context(av_ctx);
		return -1;
	}

	/* add an H.264 stream */
	av_strm = avformat_new_stream(av_ctx, NULL);
	if (!av_strm) {
		printf("Couldn't allocate H.264 stream\n");
		avformat_free_context(av_ctx);
		return -1;
	}

	/* initalize codec */
	AVCodecContext* c = av_strm->codec;
	c->codec_id = AV_CODEC_ID_H264;
	c->codec_type = AVMEDIA_TYPE_VIDEO;
	c->bit_rate = 3000000;
	c->width = w;
	c->height = h;
	c->time_base.den = fps;
	c->time_base.num = 1;

	/* write the header */
	if (avformat_write_header(av_ctx, NULL) != 0)
		printf("%s() error write header\n", __func__);
	
	return 0;
}

int sendRTPPacket(unsigned char *nal, int len)
{
	if (av_ctx == NULL)
		return 0;
	if (av_ifd > 0) {
		av_opt_set_int(av_ctx, "ifd", av_ifd, AV_OPT_SEARCH_CHILDREN);
		av_ifd = 0;
	}

	AVPacket p;
	av_init_packet(&p);
	p.data = nal;
	p.size = len;
	p.stream_index = av_strm->index;

	av_write_frame(av_ctx, &p);

	return 0;
}
