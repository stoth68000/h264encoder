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

#ifndef CAPTURE_H
#define CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "v4l.h"
#include "ipcvideo.h"
#include "fixed.h"
#include "fixed-4k.h"
#include "encoder.h"

enum capture_type_e {
	CM_V4L = 0,
	CM_IPCVIDEO = 1,
	CM_FIXED = 2,
	CM_FIXED_4K = 3,
	CM_DECKLINK = 4,
	CM_MAX
};

struct capture_parameters_s
{
	unsigned int fps;
	unsigned int width;
	unsigned int height;

	enum capture_type_e type;

	struct capture_v4l_params_s v4l;
};

struct capture_operations_s
{
	unsigned int type;
	char *name;

	void (*set_defaults)(struct capture_parameters_s *);
	void (*mainloop)(void);
	void (*stop)(void);
	int  (*start)(struct encoder_operations_s *encoder);
	void (*uninit)(void);
	int  (*init)(struct encoder_params_s *, struct capture_parameters_s *);
	void (*close)(void);
	int  (*open)(void);

	unsigned int default_fps;
};

struct capture_operations_s *getCaptureSource(unsigned int type);

#ifdef __cplusplus
};
#endif

#endif //CAPTURE_H

