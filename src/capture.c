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

#include "capture.h"

extern struct capture_operations_s decklink_ops;

static struct capture_module_s {
	unsigned int type;
	struct capture_operations_s *operations;
} capture_modules[] = {
	{ CM_V4L,	&v4l_ops },
	{ CM_IPCVIDEO,	&ipcvideo_ops },
	{ CM_FIXED,	&fixed_ops },
	{ CM_FIXED_4K,	&fixed_4k_ops },
	{ CM_DECKLINK,	&decklink_ops },
};

struct capture_operations_s *getCaptureSource(unsigned int type)
{
	for (unsigned int i = 0; i < (sizeof(capture_modules) / sizeof(struct capture_module_s)); i++) {
		if (type == capture_modules[i].type)
			return capture_modules[i].operations;
	}

	return 0;
}

