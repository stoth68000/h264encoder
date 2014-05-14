
#ifndef ENCODERDISPLAY_H
#define ENCODERDISPLAY_H

/* Copyright 2014 Kernel Labs Inc. All Rights Reserved. */

struct encoder_display_context
{
	int plotwidth;
	int plotheight;
	int currx;
	int curry;
	int stride;

	unsigned char *ptr;
	unsigned char *frame;

	unsigned char bg[2], fg[2];
};

int encoder_display_init(struct encoder_display_context *ctx);
int encoder_display_render_string(struct encoder_display_context *ctx, unsigned char *s, unsigned int len, unsigned int x, unsigned int y);
int encoder_display_render_reset(struct encoder_display_context *ctx, unsigned char *ptr, unsigned int stride);

#endif
