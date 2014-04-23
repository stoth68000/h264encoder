#include <stdio.h>
#include <libavformat/avformat.h>

/* libavformat
 * http://stackoverflow.com/questions/10143753/streaming-h-264-over-rtp-with-libavformat
 */
static AVFormatContext *av_ctx = NULL;
static AVOutputFormat *av_fmt = NULL;
static AVStream *av_strm = NULL;

void freeRTPHandler()
{
	if (av_ctx)
		avformat_free_context(av_ctx);
}

int initRTPHandler(char *ipaddress, int port, int w, int h, int fps)
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

	/* try to open the RTP stream */
	snprintf(filename, sizeof(filename), "rtp://%s:%d", ipaddress, port);
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
	c->codec_id = CODEC_ID_H264;
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

	AVPacket p;
	av_init_packet(&p);
	p.data = nal;
	p.size = len;
	p.stream_index = av_strm->index;

	av_write_frame(av_ctx, &p);

	return 0;
}
