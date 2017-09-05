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

#ifndef CSC_H
#define CSC_H

#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <va/va_enc_h264.h>

struct csc_ctx_s
{
	VADisplay   va_dpy;
	VASurfaceID rgb32_surface;
	VAContextID vpp_ctx;
	VABufferID  vpp_pipeline_buf;
};

int csc_alloc(struct csc_ctx_s *ctx,
	VADisplay va_dpy, VAConfigID cfg, VAContextID ctxid,
	int width, int height);

int csc_free(struct csc_ctx_s *ctx);

VAStatus csc_convert_rgbdata_to_yuv(struct csc_ctx_s *ctx,
	unsigned char *data, VASurfaceID yuv_surface_output);

#endif

