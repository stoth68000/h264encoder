/*
 * Copyright (c) 2007-2013 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <va/va.h>
#include <va/va_vpp.h>
#include <va/va_enc_h264.h>

#include "encoder.h"
#include "es2ts.h"
#include "rtp.h"
#include "va_display.h"
#include "encoder-display.h"
#include "main.h"

extern char *encoder_nalOutputFilename;

#define CHECK_VASTATUS(va_status,func)                                  \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr,"%s:%s (%d) failed,exit\n", __func__, func, __LINE__); \
        exit(1);                                                        \
    }

#define VAEntrypointMax		10

#define NAL_REF_IDC_NONE        0
#define NAL_REF_IDC_LOW         1
#define NAL_REF_IDC_MEDIUM      2
#define NAL_REF_IDC_HIGH        3

#define NAL_NON_IDR             1
#define NAL_IDR                 5
#define NAL_SPS                 7
#define NAL_PPS                 8
#define NAL_SEI			6

#define SLICE_TYPE_P            0
#define SLICE_TYPE_B            1
#define SLICE_TYPE_I            2

#define ENTROPY_MODE_CAVLC      0
#define ENTROPY_MODE_CABAC      1

#define PROFILE_IDC_BASELINE    66
#define PROFILE_IDC_MAIN        77
#define PROFILE_IDC_HIGH        100

#define BITSTREAM_ALLOCATE_STEPPING     4096

#define SURFACE_NUM 16		/* 16 surfaces for source YUV */
#define SURFACE_NUM 16		/* 16 surfaces for reference */

static VADisplay va_dpy;
static VAProfile h264_profile = ~0;
static VAConfigAttrib attrib[VAConfigAttribTypeMax];
static VAConfigAttrib config_attrib[VAConfigAttribTypeMax];
static int config_attrib_num = 0;
static VASurfaceID src_surface[SURFACE_NUM];
static VABufferID coded_buf[SURFACE_NUM];
static VASurfaceID ref_surface[SURFACE_NUM];
static VAConfigID config_id;
static VAContextID context_id;
static VAEncSequenceParameterBufferH264 seq_param;
static VAEncPictureParameterBufferH264 pic_param;
static VAEncSliceParameterBufferH264 slice_param;
static VAPictureH264 CurrentCurrPic;
static VAPictureH264 ReferenceFrames[16], RefPicList0_P[32], RefPicList0_B[32], RefPicList1_B[32];

/* VPP */
static VABufferID vpp_filter_bufs[VAProcFilterCount];
static unsigned int vpp_num_filter_bufs = 0;
static VAConfigID vpp_config = VA_INVALID_ID;
static VAContextID vpp_context = VA_INVALID_ID;
static VAProcPipelineCaps vpp_pipeline_caps;
static VASurfaceID *vpp_forward_references;
static unsigned int vpp_num_forward_references;
static VASurfaceID *vpp_backward_references;
static unsigned int vpp_num_backward_references;
static unsigned int vpp_deinterlace_mode; /* 0 = off, 1 = ma, 2 = bob */
static VARectangle vpp_output_region;
static VABufferID vpp_pipeline_buf;
static VAProcPipelineParameterBuffer *vpp_pipeline_param;

static unsigned int MaxFrameNum = (2 << 16);
static unsigned int MaxPicOrderCntLsb = (2 << 8);
static unsigned int Log2MaxFrameNum = 16;
static unsigned int Log2MaxPicOrderCntLsb = 8;

static unsigned int num_ref_frames = 2;
static unsigned int numShortTerm = 0;
static int constraint_set_flag = 0;
static int h264_packedheader = 0;	/* support pack header? */
static int h264_maxref = (1 << 16 | 1);
static int h264_entropy_mode = 1;	/* cabac */

static FILE *nal_fp = NULL;

static int frame_width = 176;
static int frame_height = 144;
static int frame_osd = 0;
static int frame_osd_length = 0;
static int frame_width_mbaligned;
static int frame_height_mbaligned;
static int frame_rate = 30;
static unsigned int frame_count = 60;
static unsigned long long frames_processed = 0;
extern unsigned int encoder_frame_bitrate;
static unsigned int frame_slices = 1;
static double frame_size = 0;
static int initial_qp = 26;
static int minimal_qp = 0;
static int intra_period = 30;
static int intra_idr_period = 60;
static int ip_period = 1;
static int rc_mode = VA_RC_VBR;
static unsigned long long current_frame_encoding = 0;
static unsigned long long current_frame_display = 0;
static unsigned long long current_IDR_display = 0;
static unsigned int current_frame_num = 0;
static int current_frame_type;

#define current_slot (current_frame_display % SURFACE_NUM)
#define next_slot ((current_frame_display + 1) % SURFACE_NUM)

static int misc_priv_type = 0;
static int misc_priv_value = 0;

#define MIN(a, b) ((a)>(b)?(b):(a))
#define MAX(a, b) ((a)>(b)?(a):(b))

/* thread to save coded data/upload source YUV */
struct storage_task_t {
	void *next;
	unsigned long long display_order;
	unsigned long long encode_order;
};
static struct storage_task_t *storage_task_header = NULL, *storage_task_tail = NULL;

#define SRC_SURFACE_IN_ENCODING 0
#define SRC_SURFACE_IN_STORAGE  1

static int srcsurface_status[SURFACE_NUM];
static int encode_syncmode = 0;
static pthread_mutex_t encode_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t encode_cond = PTHREAD_COND_INITIALIZER;
static pthread_t encode_thread = -1;

static struct encoder_display_context display_ctx;

struct __bitstream {
	unsigned int *buffer;
	int bit_offset;
	int max_size_in_dword;
};
typedef struct __bitstream bitstream;

static unsigned int va_swap32(unsigned int val)
{
	unsigned char *pval = (unsigned char *)&val;

	return ((pval[0] << 24) |
		(pval[1] << 16) | (pval[2] << 8) | (pval[3] << 0));
}

static void bitstream_start(bitstream * bs)
{
	bs->max_size_in_dword = BITSTREAM_ALLOCATE_STEPPING;
	bs->buffer = calloc(bs->max_size_in_dword * sizeof(int), 1);
	bs->bit_offset = 0;
}

static void bitstream_end(bitstream * bs)
{
	int pos = (bs->bit_offset >> 5);
	int bit_offset = (bs->bit_offset & 0x1f);
	int bit_left = 32 - bit_offset;

	if (bit_offset) {
		bs->buffer[pos] = va_swap32((bs->buffer[pos] << bit_left));
	}
}

static void bitstream_put_ui(bitstream * bs, unsigned int val, int size_in_bits)
{
	int pos = (bs->bit_offset >> 5);
	int bit_offset = (bs->bit_offset & 0x1f);
	int bit_left = 32 - bit_offset;

	if (!size_in_bits)
		return;

	bs->bit_offset += size_in_bits;

	if (bit_left > size_in_bits) {
		bs->buffer[pos] = (bs->buffer[pos] << size_in_bits | val);
	} else {
		size_in_bits -= bit_left;
		bs->buffer[pos] =
		    (bs->buffer[pos] << bit_left) | (val >> size_in_bits);
		bs->buffer[pos] = va_swap32(bs->buffer[pos]);

		if (pos + 1 == bs->max_size_in_dword) {
			bs->max_size_in_dword += BITSTREAM_ALLOCATE_STEPPING;
			bs->buffer =
			    realloc(bs->buffer,
				    bs->max_size_in_dword *
				    sizeof(unsigned int));
		}

		bs->buffer[pos + 1] = val;
	}
}

static void bitstream_put_ue(bitstream * bs, unsigned int val)
{
	int size_in_bits = 0;
	int tmp_val = ++val;

	while (tmp_val) {
		tmp_val >>= 1;
		size_in_bits++;
	}

	bitstream_put_ui(bs, 0, size_in_bits - 1);	// leading zero
	bitstream_put_ui(bs, val, size_in_bits);
}

static void bitstream_put_se(bitstream * bs, int val)
{
	unsigned int new_val;

	if (val <= 0)
		new_val = -2 * val;
	else
		new_val = 2 * val - 1;

	bitstream_put_ue(bs, new_val);
}

static void bitstream_byte_aligning(bitstream * bs, int bit)
{
	int bit_offset = (bs->bit_offset & 0x7);
	int bit_left = 8 - bit_offset;
	int new_val;

	if (!bit_offset)
		return;

	assert(bit == 0 || bit == 1);

	if (bit)
		new_val = (1 << bit_left) - 1;
	else
		new_val = 0;

	bitstream_put_ui(bs, new_val, bit_left);
}

static void rbsp_trailing_bits(bitstream * bs)
{
	bitstream_put_ui(bs, 1, 1);
	bitstream_byte_aligning(bs, 0);
}

static void nal_start_code_prefix(bitstream * bs)
{
	bitstream_put_ui(bs, 0x00000001, 32);
}

static void nal_header(bitstream * bs, int nal_ref_idc, int nal_unit_type)
{
	bitstream_put_ui(bs, 0, 1);	/* forbidden_zero_bit: 0 */
	bitstream_put_ui(bs, nal_ref_idc, 2);
	bitstream_put_ui(bs, nal_unit_type, 5);
}

static void sps_rbsp(bitstream * bs)
{
	int profile_idc = PROFILE_IDC_BASELINE;

	if (h264_profile == VAProfileH264High)
		profile_idc = PROFILE_IDC_HIGH;
	else if (h264_profile == VAProfileH264Main)
		profile_idc = PROFILE_IDC_MAIN;

	bitstream_put_ui(bs, profile_idc, 8);	/* profile_idc */
	bitstream_put_ui(bs, ! !(constraint_set_flag & 1), 1);	/* constraint_set0_flag */
	bitstream_put_ui(bs, ! !(constraint_set_flag & 2), 1);	/* constraint_set1_flag */
	bitstream_put_ui(bs, ! !(constraint_set_flag & 4), 1);	/* constraint_set2_flag */
	bitstream_put_ui(bs, ! !(constraint_set_flag & 8), 1);	/* constraint_set3_flag */
	bitstream_put_ui(bs, 0, 4);	/* reserved_zero_4bits */
	bitstream_put_ui(bs, seq_param.level_idc, 8);	/* level_idc */
	bitstream_put_ue(bs, seq_param.seq_parameter_set_id);	/* seq_parameter_set_id */

	if (profile_idc == PROFILE_IDC_HIGH) {
		bitstream_put_ue(bs, 1);	/* chroma_format_idc = 1, 4:2:0 */
		bitstream_put_ue(bs, 0);	/* bit_depth_luma_minus8 */
		bitstream_put_ue(bs, 0);	/* bit_depth_chroma_minus8 */
		bitstream_put_ui(bs, 0, 1);	/* qpprime_y_zero_transform_bypass_flag */
		bitstream_put_ui(bs, 0, 1);	/* seq_scaling_matrix_present_flag */
	}

	bitstream_put_ue(bs, seq_param.seq_fields.bits.log2_max_frame_num_minus4);	/* log2_max_frame_num_minus4 */
	bitstream_put_ue(bs, seq_param.seq_fields.bits.pic_order_cnt_type);	/* pic_order_cnt_type */

	if (seq_param.seq_fields.bits.pic_order_cnt_type == 0)
		bitstream_put_ue(bs, seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);	/* log2_max_pic_order_cnt_lsb_minus4 */
	else {
		assert(0);
	}

	bitstream_put_ue(bs, seq_param.max_num_ref_frames);	/* num_ref_frames */
	bitstream_put_ui(bs, 0, 1);	/* gaps_in_frame_num_value_allowed_flag */

	bitstream_put_ue(bs, seq_param.picture_width_in_mbs - 1);	/* pic_width_in_mbs_minus1 */
	bitstream_put_ue(bs, seq_param.picture_height_in_mbs - 1);	/* pic_height_in_map_units_minus1 */
	bitstream_put_ui(bs, seq_param.seq_fields.bits.frame_mbs_only_flag, 1);	/* frame_mbs_only_flag */

	if (!seq_param.seq_fields.bits.frame_mbs_only_flag) {
		assert(0);
	}

	bitstream_put_ui(bs, seq_param.seq_fields.bits.direct_8x8_inference_flag, 1);	/* direct_8x8_inference_flag */
	bitstream_put_ui(bs, seq_param.frame_cropping_flag, 1);	/* frame_cropping_flag */

	if (seq_param.frame_cropping_flag) {
		bitstream_put_ue(bs, seq_param.frame_crop_left_offset);	/* frame_crop_left_offset */
		bitstream_put_ue(bs, seq_param.frame_crop_right_offset);	/* frame_crop_right_offset */
		bitstream_put_ue(bs, seq_param.frame_crop_top_offset);	/* frame_crop_top_offset */
		bitstream_put_ue(bs, seq_param.frame_crop_bottom_offset);	/* frame_crop_bottom_offset */
	}
	//if ( frame_bit_rate < 0 ) { //TODO EW: the vui header isn't correct
	if (1) {
		bitstream_put_ui(bs, 0, 1);	/* vui_parameters_present_flag */
	} else {
		bitstream_put_ui(bs, 1, 1);	/* vui_parameters_present_flag */
		bitstream_put_ui(bs, 0, 1);	/* aspect_ratio_info_present_flag */
		bitstream_put_ui(bs, 0, 1);	/* overscan_info_present_flag */
		bitstream_put_ui(bs, 0, 1);	/* video_signal_type_present_flag */
		bitstream_put_ui(bs, 0, 1);	/* chroma_loc_info_present_flag */
		bitstream_put_ui(bs, 1, 1);	/* timing_info_present_flag */
		{
			bitstream_put_ui(bs, 15, 32);
			bitstream_put_ui(bs, 900, 32);
			bitstream_put_ui(bs, 1, 1);
		}
		bitstream_put_ui(bs, 1, 1);	/* nal_hrd_parameters_present_flag */
		{
			// hrd_parameters 
			bitstream_put_ue(bs, 0);	/* cpb_cnt_minus1 */
			bitstream_put_ui(bs, 4, 4);	/* bit_rate_scale */
			bitstream_put_ui(bs, 6, 4);	/* cpb_size_scale */

			bitstream_put_ue(bs, encoder_frame_bitrate - 1);	/* bit_rate_value_minus1[0] */
			bitstream_put_ue(bs, encoder_frame_bitrate * 8 - 1);	/* cpb_size_value_minus1[0] */
			bitstream_put_ui(bs, 1, 1);	/* cbr_flag[0] */

			bitstream_put_ui(bs, 23, 5);	/* initial_cpb_removal_delay_length_minus1 */
			bitstream_put_ui(bs, 23, 5);	/* cpb_removal_delay_length_minus1 */
			bitstream_put_ui(bs, 23, 5);	/* dpb_output_delay_length_minus1 */
			bitstream_put_ui(bs, 23, 5);	/* time_offset_length  */
		}
		bitstream_put_ui(bs, 0, 1);	/* vcl_hrd_parameters_present_flag */
		bitstream_put_ui(bs, 0, 1);	/* low_delay_hrd_flag */

		bitstream_put_ui(bs, 0, 1);	/* pic_struct_present_flag */
		bitstream_put_ui(bs, 0, 1);	/* bitstream_restriction_flag */
	}

	rbsp_trailing_bits(bs);	/* rbsp_trailing_bits */
}

static void pps_rbsp(bitstream * bs)
{
	bitstream_put_ue(bs, pic_param.pic_parameter_set_id);	/* pic_parameter_set_id */
	bitstream_put_ue(bs, pic_param.seq_parameter_set_id);	/* seq_parameter_set_id */

	bitstream_put_ui(bs, pic_param.pic_fields.bits.entropy_coding_mode_flag, 1);	/* entropy_coding_mode_flag */

	bitstream_put_ui(bs, 0, 1);	/* pic_order_present_flag: 0 */

	bitstream_put_ue(bs, 0);	/* num_slice_groups_minus1 */

	bitstream_put_ue(bs, pic_param.num_ref_idx_l0_active_minus1);	/* num_ref_idx_l0_active_minus1 */
	bitstream_put_ue(bs, pic_param.num_ref_idx_l1_active_minus1);	/* num_ref_idx_l1_active_minus1 1 */

	bitstream_put_ui(bs, pic_param.pic_fields.bits.weighted_pred_flag, 1);	/* weighted_pred_flag: 0 */
	bitstream_put_ui(bs, pic_param.pic_fields.bits.weighted_bipred_idc, 2);	/* weighted_bipred_idc: 0 */

	bitstream_put_se(bs, pic_param.pic_init_qp - 26);	/* pic_init_qp_minus26 */
	bitstream_put_se(bs, 0);	/* pic_init_qs_minus26 */
	bitstream_put_se(bs, 0);	/* chroma_qp_index_offset */

	bitstream_put_ui(bs, pic_param.pic_fields.bits.deblocking_filter_control_present_flag, 1);	/* deblocking_filter_control_present_flag */
	bitstream_put_ui(bs, 0, 1);	/* constrained_intra_pred_flag */
	bitstream_put_ui(bs, 0, 1);	/* redundant_pic_cnt_present_flag */

	/* more_rbsp_data */
	bitstream_put_ui(bs, pic_param.pic_fields.bits.transform_8x8_mode_flag, 1);	/*transform_8x8_mode_flag */
	bitstream_put_ui(bs, 0, 1);	/* pic_scaling_matrix_present_flag */
	bitstream_put_se(bs, pic_param.second_chroma_qp_index_offset);	/*second_chroma_qp_index_offset */

	rbsp_trailing_bits(bs);
}

static int build_packed_pic_buffer(unsigned char **header_buffer)
{
	bitstream bs;

	bitstream_start(&bs);
	nal_start_code_prefix(&bs);
	nal_header(&bs, NAL_REF_IDC_HIGH, NAL_PPS);
	pps_rbsp(&bs);
	bitstream_end(&bs);

	*header_buffer = (unsigned char *)bs.buffer;
	return bs.bit_offset;
}

static int build_packed_seq_buffer(unsigned char **header_buffer)
{
	bitstream bs;

	bitstream_start(&bs);
	nal_start_code_prefix(&bs);
	nal_header(&bs, NAL_REF_IDC_HIGH, NAL_SPS);
	sps_rbsp(&bs);
	bitstream_end(&bs);

	*header_buffer = (unsigned char *)bs.buffer;
	return bs.bit_offset;
}

static int build_packed_sei_buffer_timing(unsigned int init_cpb_removal_length,
       unsigned int init_cpb_removal_delay,
       unsigned int init_cpb_removal_delay_offset,
       unsigned int cpb_removal_length,
       unsigned int cpb_removal_delay,
       unsigned int dpb_output_length,
       unsigned int dpb_output_delay,
       unsigned char **sei_buffer)
{
	unsigned char *byte_buf;
	int bp_byte_size, i, pic_byte_size;

	bitstream nal_bs;
	bitstream sei_bp_bs, sei_pic_bs;

	bitstream_start(&sei_bp_bs);
	bitstream_put_ue(&sei_bp_bs, 0);	/*seq_parameter_set_id */
	bitstream_put_ui(&sei_bp_bs, init_cpb_removal_delay,
			 cpb_removal_length);
	bitstream_put_ui(&sei_bp_bs, init_cpb_removal_delay_offset,
			 cpb_removal_length);
	if (sei_bp_bs.bit_offset & 0x7) {
		bitstream_put_ui(&sei_bp_bs, 1, 1);
	}
	bitstream_end(&sei_bp_bs);
	bp_byte_size = (sei_bp_bs.bit_offset + 7) / 8;

	bitstream_start(&sei_pic_bs);
	bitstream_put_ui(&sei_pic_bs, cpb_removal_delay, cpb_removal_length);
	bitstream_put_ui(&sei_pic_bs, dpb_output_delay, dpb_output_length);
	if (sei_pic_bs.bit_offset & 0x7) {
		bitstream_put_ui(&sei_pic_bs, 1, 1);
	}
	bitstream_end(&sei_pic_bs);
	pic_byte_size = (sei_pic_bs.bit_offset + 7) / 8;

	bitstream_start(&nal_bs);
	nal_start_code_prefix(&nal_bs);
	nal_header(&nal_bs, NAL_REF_IDC_NONE, NAL_SEI);

	/* Write the SEI buffer period data */
	bitstream_put_ui(&nal_bs, 0, 8);
	bitstream_put_ui(&nal_bs, bp_byte_size, 8);

	byte_buf = (unsigned char *)sei_bp_bs.buffer;
	for (i = 0; i < bp_byte_size; i++) {
		bitstream_put_ui(&nal_bs, byte_buf[i], 8);
	}
	free(byte_buf);
	/* write the SEI timing data */
	bitstream_put_ui(&nal_bs, 0x01, 8);
	bitstream_put_ui(&nal_bs, pic_byte_size, 8);

	byte_buf = (unsigned char *)sei_pic_bs.buffer;
	for (i = 0; i < pic_byte_size; i++) {
		bitstream_put_ui(&nal_bs, byte_buf[i], 8);
	}
	free(byte_buf);

	rbsp_trailing_bits(&nal_bs);
	bitstream_end(&nal_bs);

	*sei_buffer = (unsigned char *)nal_bs.buffer;

	return nal_bs.bit_offset;
}

/*
  Assume frame sequence is: Frame#0,#1,#2,...,#M,...,#X,... (encoding order)
  1) period between Frame #X and Frame #N = #X - #N
  2) 0 means infinite for intra_period/intra_idr_period, and 0 is invalid for ip_period
  3) intra_idr_period % intra_period (intra_period > 0) and intra_period % ip_period must be 0
  4) intra_period and intra_idr_period take precedence over ip_period
  5) if ip_period > 1, intra_period and intra_idr_period are not  the strict periods 
     of I/IDR frames, see bellow examples
  -------------------------------------------------------------------
  intra_period intra_idr_period ip_period frame sequence (intra_period/intra_idr_period/ip_period)
  0            ignored          1          IDRPPPPPPP ...     (No IDR/I any more)
  0            ignored        >=2          IDR(PBB)(PBB)...   (No IDR/I any more)
  1            0                ignored    IDRIIIIIII...      (No IDR any more)
  1            1                ignored    IDR IDR IDR IDR...
  1            >=2              ignored    IDRII IDRII IDR... (1/3/ignore)
  >=2          0                1          IDRPPP IPPP I...   (3/0/1)
  >=2          0              >=2          IDR(PBB)(PBB)(IBB) (6/0/3)
                                              (PBB)(IBB)(PBB)(IBB)... 
  >=2          >=2              1          IDRPPPPP IPPPPP IPPPPP (6/18/1)
                                           IDRPPPPP IPPPPP IPPPPP...
  >=2          >=2              >=2        {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)} (6/18/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)(IBB)(PBB)}           (6/12/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)}                     (6/6/3)
                                           {IDR(PBB)(PBB)}.
*/

/*
 * Return displaying order with specified periods and encoding order
 * displaying_order: displaying order
 * frame_type: frame type 
 */
#define FRAME_P 0
#define FRAME_B 1
#define FRAME_I 2
#define FRAME_IDR 7
void encoding2display_order(unsigned long long encoding_order, int intra_period,
			    int intra_idr_period, int ip_period,
			    unsigned long long *displaying_order,
			    int *frame_type)
{
	int encoding_order_gop = 0;

	if (intra_period == 1) {	/* all are I/IDR frames */
		*displaying_order = encoding_order;
		if (intra_idr_period == 0)
			*frame_type =
			    (encoding_order == 0) ? FRAME_IDR : FRAME_I;
		else
			*frame_type =
			    (encoding_order % intra_idr_period ==
			     0) ? FRAME_IDR : FRAME_I;
		return;
	}

	if (intra_period == 0)
		intra_idr_period = 0;

	/* new sequence like
	 * IDR PPPPP IPPPPP
	 * IDR (PBB)(PBB)(IBB)(PBB)
	 */
	encoding_order_gop = (intra_idr_period == 0) ? encoding_order :
	    (encoding_order % (intra_idr_period + ((ip_period == 1) ? 0 : 1)));

	if (encoding_order_gop == 0) {	/* the first frame */
		*frame_type = FRAME_IDR;
		*displaying_order = encoding_order;
	} else if (((encoding_order_gop - 1) % ip_period) != 0) {	/* B frames */
		*frame_type = FRAME_B;
		*displaying_order = encoding_order - 1;
	} else if ((intra_period != 0) &&	/* have I frames */
		   (encoding_order_gop >= 2) && ((ip_period == 1 && encoding_order_gop % intra_period == 0) ||	/* for IDR PPPPP IPPPP */
						 /* for IDR (PBB)(PBB)(IBB) */
						 (ip_period >= 2
						  && ((encoding_order_gop - 1) /
						      ip_period %
						      (intra_period /
						       ip_period)) == 0))) {
		*frame_type = FRAME_I;
		*displaying_order = encoding_order + ip_period - 1;
	} else {
		*frame_type = FRAME_P;
		*displaying_order = encoding_order + ip_period - 1;
	}
}

char *fourcc_to_string(int fourcc)
{
	switch (fourcc) {
	case VA_FOURCC_NV12:
		return "NV12";
	case VA_FOURCC_IYUV:
		return "IYUV";
	case VA_FOURCC_YV12:
		return "YV12";
	case VA_FOURCC_UYVY:
		return "UYVY";
	default:
		return "Unknown";
	}
}

int string_to_fourcc(char *str)
{
	int fourcc;

	if (!strncmp(str, "NV12", 4))
		fourcc = VA_FOURCC_NV12;
	else if (!strncmp(str, "IYUV", 4))
		fourcc = VA_FOURCC_IYUV;
	else if (!strncmp(str, "YV12", 4))
		fourcc = VA_FOURCC_YV12;
	else if (!strncmp(str, "UYVY", 4))
		fourcc = VA_FOURCC_UYVY;
	else {
		printf("Unknow FOURCC\n");
		fourcc = -1;
	}
	return fourcc;
}

static char *rc_to_string(int rcmode)
{
	switch (rc_mode) {
	case VA_RC_NONE:
		return "NONE";
	case VA_RC_CBR:
		return "CBR";
	case VA_RC_VBR:
		return "VBR";
	case VA_RC_VCM:
		return "VCM";
	case VA_RC_CQP:
		return "CQP";
	case VA_RC_VBR_CONSTRAINED:
		return "VBR_CONSTRAINED";
	default:
		return "Unknown";
	}
}

int string_to_rc(char *str)
{
	int rc_mode;

	if (!strncmp(str, "NONE", 4))
		rc_mode = VA_RC_NONE;
	else if (!strncmp(str, "CBR", 3))
		rc_mode = VA_RC_CBR;
	else if (!strncmp(str, "VBR", 3))
		rc_mode = VA_RC_VBR;
	else if (!strncmp(str, "VCM", 3))
		rc_mode = VA_RC_VCM;
	else if (!strncmp(str, "CQP", 3))
		rc_mode = VA_RC_CQP;
	else if (!strncmp(str, "VBR_CONSTRAINED", 15))
		rc_mode = VA_RC_VBR_CONSTRAINED;
	else {
		printf("Unknown RC mode\n");
		rc_mode = -1;
	}
	return rc_mode;
}

static char * vpp_filter_string(VAProcFilterType filter)
{
	switch (filter) {
	case VAProcFilterNone:return "VAProcFilterNone";
	case VAProcFilterNoiseReduction:return "VAProcFilterNoiseReduction";
	case VAProcFilterDeinterlacing:return "VAProcFilterDeinterlacing";
	case VAProcFilterSharpening:return "VAProcFilterSharpening";
	case VAProcFilterColorBalance:return "VAProcFilterColorBalance";
	default:
		break;
	}
	return "<unknown filter>";
}

static char * vpp_deinterlace_string(VAProcDeinterlacingType deinterlace)
{
	switch (deinterlace) {
	case VAProcDeinterlacingNone:return "VAProcDeinterlacingNone";
	case VAProcDeinterlacingBob:return "VAProcDeinterlacingBob";
	case VAProcDeinterlacingWeave:return "VAProcDeinterlacingWeave";
	case VAProcDeinterlacingMotionAdaptive:return "VAProcDeinterlacingMotionAdaptive";
	case VAProcDeinterlacingMotionCompensated:return "VAProcDeinterlacingMotionCompensated";
	default:
		break;
	}
	return "<unknown deinterlace capability>";
}

/* Display all supported VPP deinterlaced modes to console */
static void vpp_enumerate_deinterlace(VADisplay va_dpy)
{
	VAProcFilterType filters[VAProcFilterCount];
	unsigned int num_filters = VAProcFilterCount;
	VAStatus va_status;
	unsigned int i, j;

	va_status = vaQueryVideoProcFilters(va_dpy, vpp_context, &filters[0], &num_filters);
	CHECK_VASTATUS(va_status, "vaQueryVideoProcFilters");

	for (i = 0; i < num_filters; i++) {
		printf("%s\n", vpp_filter_string(filters[i]));
		if (filters[i] == VAProcFilterDeinterlacing) {
			VAProcDeinterlacingType deinterlacing_caps[VAProcDeinterlacingCount];
			unsigned int num_deinterlacing_caps = VAProcDeinterlacingCount;

			vaQueryVideoProcFilterCaps(va_dpy, vpp_context,
				VAProcFilterDeinterlacing, &deinterlacing_caps, &num_deinterlacing_caps);
			for (j = 0; j < num_deinterlacing_caps; j++) {
				printf("\t%s\n", vpp_deinterlace_string(deinterlacing_caps[j]));
			}
		}
	}
}

/* Confirm if a specific VPP deinterlace mode is supported */
static int vpp_supports_deinterlace(VADisplay va_dpy, VAProcDeinterlacingType dtype)
{
	VAProcFilterType filters[VAProcFilterCount];
	unsigned int num_filters = VAProcFilterCount;
	VAStatus va_status;
	unsigned int i, j;
	unsigned int found = 0;

	va_status = vaQueryVideoProcFilters(va_dpy, vpp_context, &filters[0], &num_filters);
	CHECK_VASTATUS(va_status, "vaQueryVideoProcFilters");

	for (i = 0; i < num_filters; i++) {
		printf("%s\n", vpp_filter_string(filters[i]));
		if (filters[i] == VAProcFilterDeinterlacing) {
			VAProcDeinterlacingType deinterlacing_caps[VAProcDeinterlacingCount];
			unsigned int num_deinterlacing_caps = VAProcDeinterlacingCount;

			vaQueryVideoProcFilterCaps(va_dpy, vpp_context,
				VAProcFilterDeinterlacing, &deinterlacing_caps, &num_deinterlacing_caps);
			for (j = 0; j < num_deinterlacing_caps; j++) {
				if (deinterlacing_caps[j] == dtype)
					found = 1;
			}
		}
	}

	return found;
}

static int vpp_perform_deinterlace(VASurfaceID surface, unsigned int w, unsigned int h, VASurfaceID forward_reference)
{
	VAStatus va_status;

	vaBeginPicture(va_dpy, vpp_context, surface);

	va_status = vaMapBuffer(va_dpy, vpp_pipeline_buf, (void *)&vpp_pipeline_param);
	CHECK_VASTATUS(va_status, "vaMapBuffer");
	vpp_pipeline_param->surface              = surface;
	vpp_pipeline_param->surface_region       = NULL;
	vpp_pipeline_param->output_region        = &vpp_output_region;
	vpp_pipeline_param->output_background_color = 0;
	vpp_pipeline_param->filter_flags         = VA_FILTER_SCALING_HQ;
	vpp_pipeline_param->filters              = vpp_filter_bufs;
	vpp_pipeline_param->num_filters          = vpp_num_filter_bufs;
	va_status = vaUnmapBuffer(va_dpy, vpp_pipeline_buf);
	CHECK_VASTATUS(va_status, "vaUnmapBuffer");

	// Update reference frames for deinterlacing, if necessary
	vpp_forward_references[0] = forward_reference;
	vpp_pipeline_param->forward_references      = vpp_forward_references;
	vpp_pipeline_param->num_forward_references  = vpp_num_forward_references;
	vpp_pipeline_param->backward_references     = vpp_backward_references;
	vpp_pipeline_param->num_backward_references = vpp_num_backward_references;

	// Apply filters
	va_status = vaRenderPicture(va_dpy, vpp_context, &vpp_pipeline_buf, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture");

	vaEndPicture(va_dpy, vpp_context);

	return 0;
}

static int prior_slot(void)
{
	int slot = current_frame_display % SURFACE_NUM;
	slot -= 1;
	if (slot < 0)
		slot = SURFACE_NUM + slot;

	return slot;
}

static void deinit_vpp()
{
	for (unsigned int i = 0; i < vpp_num_filter_bufs; i++) {
		vaDestroyBuffer(va_dpy, vpp_filter_bufs[i]);
		vpp_filter_bufs[i] = VA_INVALID_ID;
	}

	vaDestroyBuffer(va_dpy, vpp_pipeline_buf);

	if (vpp_context != VA_INVALID_ID)
		vaDestroyContext(va_dpy, vpp_context);
	if (vpp_config != VA_INVALID_ID)
		vaDestroyConfig(va_dpy, vpp_config);

	vpp_context = VA_INVALID_ID;
	vpp_config = VA_INVALID_ID;
}

/* Initialize VPP, create the VPP deinterlacer processing pipeline */
static int init_vpp()
{
	VAEntrypoint entrypoints[VAEntrypointMax] = { 0 };
	int i, num_entrypoints, supportsVideoProcessing = 0;
	VAStatus va_status;

	vaQueryConfigEntrypoints(va_dpy, VAProfileNone, entrypoints, &num_entrypoints);

	for (i = 0; !supportsVideoProcessing && i < num_entrypoints; i++) {
		if (entrypoints[i] == VAEntrypointVideoProc)
			supportsVideoProcessing = 1;
	}

	printf("Platform supports LIBVA VideoPostProcessing? : %s\n", supportsVideoProcessing ? "True" : "False");
	if (!supportsVideoProcessing || (vpp_deinterlace_mode == 0))
		return -1;

	/* one-time - Config/context creation for VPP interaction */
	for (int i = 0; i < VAProcFilterCount; i++) {
		vpp_filter_bufs[i] = VA_INVALID_ID;
	}

        vpp_config = VA_INVALID_ID;
	va_status = vaCreateConfig(va_dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &vpp_config);
	CHECK_VASTATUS(va_status, "vaCreateConfig");

	vpp_context = VA_INVALID_ID;
	va_status = vaCreateContext(va_dpy, vpp_config, 0, 0, 0, NULL, 0, &vpp_context);
	CHECK_VASTATUS(va_status, "vaCreateContext");

	/* Show all supported deinterlace modes */
	vpp_enumerate_deinterlace(va_dpy);

	/* Check for our preferred mode */
	VAProcDeinterlacingType deint_mode = VAProcDeinterlacingNone;
	if ((vpp_deinterlace_mode == 1) && vpp_supports_deinterlace(va_dpy, VAProcDeinterlacingMotionAdaptive)) {
		printf("Yay, motion adaptive!\n");
		deint_mode = VAProcDeinterlacingMotionAdaptive;
	} else 
	if ((vpp_deinterlace_mode == 2) && vpp_supports_deinterlace(va_dpy, VAProcDeinterlacingBob)) {
		printf("boo, bob support!\n");
		deint_mode = VAProcDeinterlacingBob;
	}

	if (deint_mode != VAProcDeinterlacingNone) {

		/* Create a VPP Deinterlace pipeline */
		VABufferID deint_filter = VA_INVALID_ID;
		VAProcFilterParameterBufferDeinterlacing deint;
		deint.type = VAProcFilterDeinterlacing;
		deint.algorithm = deint_mode;
		va_status = vaCreateBuffer(va_dpy, vpp_context, VAProcFilterParameterBufferType, sizeof(deint), 1, &deint, &deint_filter);
		CHECK_VASTATUS(va_status, "vaCreateBuffer");

		vpp_filter_bufs[vpp_num_filter_bufs++] = deint_filter;

		// Create filters
		//VAProcColorStandardType in_color_standards[VAProcColorStandardCount];
		//VAProcColorStandardType out_color_standards[VAProcColorStandardCount];

		vpp_pipeline_caps.input_color_standards      = NULL;
		//pipeline_caps.num_input_color_standards  = ARRAY_ELEMS(in_color_standards);
		vpp_pipeline_caps.num_input_color_standards  = 0;
		vpp_pipeline_caps.output_color_standards     = NULL;
		//pipeline_caps.num_output_color_standards = ARRAY_ELEMS(out_color_standards);
		vpp_pipeline_caps.num_output_color_standards = 0;
		vaQueryVideoProcPipelineCaps(va_dpy, vpp_context,
				vpp_filter_bufs, vpp_num_filter_bufs,
				&vpp_pipeline_caps);

		vpp_num_forward_references  = vpp_pipeline_caps.num_forward_references;
		vpp_forward_references      = malloc(vpp_num_forward_references * sizeof(VASurfaceID));
		vpp_num_backward_references = vpp_pipeline_caps.num_backward_references;
		vpp_backward_references     = malloc(vpp_num_backward_references * sizeof(VASurfaceID));

		printf("vpp_num_forward_references = %d\n", vpp_num_forward_references);
		printf("vpp_num_backward_references = %d\n", vpp_num_backward_references);
	}

	va_status = vaCreateBuffer(va_dpy, vpp_context,
		VAProcPipelineParameterBufferType, sizeof(*vpp_pipeline_param), 1,
		NULL, &vpp_pipeline_buf);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");

	// Setup output region for this surface
	// e.g. upper left corner for the first surface
	vpp_output_region.x      = 0;
	vpp_output_region.y      = 0;
	vpp_output_region.width  = frame_width;
	vpp_output_region.height = frame_height;

	return 0;
}

static int init_va(void)
{
	VAProfile profile_list[] =
	    { VAProfileH264High, VAProfileH264Main, VAProfileH264Baseline, VAProfileH264ConstrainedBaseline };

	VAEntrypoint entrypoints[VAEntrypointMax] = { 0 };
	int num_entrypoints, slice_entrypoint;
	int support_encode = 0;
	int major_ver, minor_ver;
	VAStatus va_status;
	unsigned int i;

	va_dpy = va_open_display();
	va_status = vaInitialize(va_dpy, &major_ver, &minor_ver);
	CHECK_VASTATUS(va_status, "vaInitialize");

	/* use the highest profile */
	for (i = 0; i < sizeof(profile_list) / sizeof(profile_list[0]); i++) {
		if ((h264_profile != ~0) && h264_profile != profile_list[i])
			continue;

		h264_profile = profile_list[i];
		vaQueryConfigEntrypoints(va_dpy, h264_profile, entrypoints,
					 &num_entrypoints);
		for (slice_entrypoint = 0; slice_entrypoint < num_entrypoints;
		     slice_entrypoint++) {
			if (entrypoints[slice_entrypoint] ==
			    VAEntrypointEncSlice) {
				support_encode = 1;
				break;
			}
		}
		if (support_encode == 1)
			break;
	}

	if (support_encode == 0) {
		printf("Can't find VAEntrypointEncSlice for H264 profiles\n");
		exit(1);
	} else {
		switch (h264_profile) {
		case VAProfileH264Baseline:
			printf("Use profile VAProfileH264Baseline\n");
			ip_period = 1;
			constraint_set_flag |= (1 << 0);	/* Annex A.2.1 */
			h264_entropy_mode = 0;
			break;
		case VAProfileH264ConstrainedBaseline:
			printf
			    ("Use profile VAProfileH264ConstrainedBaseline\n");
			constraint_set_flag |= (1 << 0 | 1 << 1);	/* Annex A.2.2 */
			ip_period = 1;
			break;

		case VAProfileH264Main:
			printf("Use profile VAProfileH264Main\n");
			constraint_set_flag |= (1 << 1);	/* Annex A.2.2 */
			break;

		case VAProfileH264High:
			constraint_set_flag |= (1 << 3);	/* Annex A.2.4 */
			printf("Use profile VAProfileH264High\n");
			break;
		default:
			printf("unknown profile. Set to Baseline");
			h264_profile = VAProfileH264Baseline;
			ip_period = 1;
			constraint_set_flag |= (1 << 0);	/* Annex A.2.1 */
			break;
		}
	}

	/* find out the format for the render target, and rate control mode */
	for (i = 0; i < VAConfigAttribTypeMax; i++)
		attrib[i].type = i;

	va_status =
	    vaGetConfigAttributes(va_dpy, h264_profile, VAEntrypointEncSlice,
				  &attrib[0], VAConfigAttribTypeMax);
	CHECK_VASTATUS(va_status, "vaGetConfigAttributes");
	/* check the interested configattrib */
	if ((attrib[VAConfigAttribRTFormat].value & VA_RT_FORMAT_YUV420) == 0) {
		printf("Not find desired YUV420 RT format\n");
		exit(1);
	} else {
		config_attrib[config_attrib_num].type = VAConfigAttribRTFormat;
		config_attrib[config_attrib_num].value = VA_RT_FORMAT_YUV420;
		config_attrib_num++;
	}

	if (attrib[VAConfigAttribRateControl].value != VA_ATTRIB_NOT_SUPPORTED) {
		int tmp = attrib[VAConfigAttribRateControl].value;

		printf("Support rate control mode (0x%x):", tmp);

		if (tmp & VA_RC_NONE)
			printf("NONE ");
		if (tmp & VA_RC_CBR)
			printf("CBR ");
		if (tmp & VA_RC_VBR)
			printf("VBR ");
		if (tmp & VA_RC_VCM)
			printf("VCM ");
		if (tmp & VA_RC_CQP)
			printf("CQP ");
		if (tmp & VA_RC_VBR_CONSTRAINED)
			printf("VBR_CONSTRAINED ");

		printf("\n");

		/* need to check if support rc_mode */
		config_attrib[config_attrib_num].type =
		    VAConfigAttribRateControl;
		config_attrib[config_attrib_num].value = rc_mode;
		config_attrib_num++;
	}

	if (attrib[VAConfigAttribEncPackedHeaders].value !=
	    VA_ATTRIB_NOT_SUPPORTED) {
		int tmp = attrib[VAConfigAttribEncPackedHeaders].value;

		printf("Support VAConfigAttribEncPackedHeaders\n");

		h264_packedheader = 1;
		config_attrib[config_attrib_num].type =
		    VAConfigAttribEncPackedHeaders;
		config_attrib[config_attrib_num].value =
		    VA_ENC_PACKED_HEADER_NONE;

		if (tmp & VA_ENC_PACKED_HEADER_SEQUENCE) {
			printf("Support packed sequence headers\n");
			config_attrib[config_attrib_num].value |=
			    VA_ENC_PACKED_HEADER_SEQUENCE;
		}

		if (tmp & VA_ENC_PACKED_HEADER_PICTURE) {
			printf("Support packed picture headers\n");
			config_attrib[config_attrib_num].value |=
			    VA_ENC_PACKED_HEADER_PICTURE;
		}

		if (tmp & VA_ENC_PACKED_HEADER_SLICE) {
			printf("Support packed slice headers\n");
			config_attrib[config_attrib_num].value |=
			    VA_ENC_PACKED_HEADER_SLICE;
		}

		if (tmp & VA_ENC_PACKED_HEADER_MISC) {
			printf("Support packed misc headers\n");
			config_attrib[config_attrib_num].value |=
			    VA_ENC_PACKED_HEADER_MISC;
		}

		config_attrib_num++;
	}

	if (attrib[VAConfigAttribEncInterlaced].value !=
	    VA_ATTRIB_NOT_SUPPORTED) {
		int tmp = attrib[VAConfigAttribEncInterlaced].value;

		printf("Support VAConfigAttribEncInterlaced\n");

		if (tmp & VA_ENC_INTERLACED_FRAME)
			printf("support VA_ENC_INTERLACED_FRAME\n");
		if (tmp & VA_ENC_INTERLACED_FIELD)
			printf("Support VA_ENC_INTERLACED_FIELD\n");
		if (tmp & VA_ENC_INTERLACED_MBAFF)
			printf("Support VA_ENC_INTERLACED_MBAFF\n");
		if (tmp & VA_ENC_INTERLACED_PAFF)
			printf("Support VA_ENC_INTERLACED_PAFF\n");

		config_attrib[config_attrib_num].type =
		    VAConfigAttribEncInterlaced;
		config_attrib[config_attrib_num].value =
		    VA_ENC_PACKED_HEADER_NONE;
		config_attrib_num++;
	}

	if (attrib[VAConfigAttribEncMaxRefFrames].value !=
	    VA_ATTRIB_NOT_SUPPORTED) {
		h264_maxref = attrib[VAConfigAttribEncMaxRefFrames].value;

		printf("Support %d RefPicList0 and %d RefPicList1\n",
		       h264_maxref & 0xffff, (h264_maxref >> 16) & 0xffff);
	}

	if (attrib[VAConfigAttribEncMaxSlices].value != VA_ATTRIB_NOT_SUPPORTED)
		printf("Support %d slices\n",
		       attrib[VAConfigAttribEncMaxSlices].value);

	if (attrib[VAConfigAttribEncSliceStructure].value !=
	    VA_ATTRIB_NOT_SUPPORTED) {
		int tmp = attrib[VAConfigAttribEncSliceStructure].value;

		printf("Support VAConfigAttribEncSliceStructure\n");

		if (tmp & VA_ENC_SLICE_STRUCTURE_ARBITRARY_ROWS)
			printf
			    ("Support VA_ENC_SLICE_STRUCTURE_ARBITRARY_ROWS\n");
		if (tmp & VA_ENC_SLICE_STRUCTURE_POWER_OF_TWO_ROWS)
			printf
			    ("Support VA_ENC_SLICE_STRUCTURE_POWER_OF_TWO_ROWS\n");
		if (tmp & VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS)
			printf
			    ("Support VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS\n");
	}
	if (attrib[VAConfigAttribEncMacroblockInfo].value !=
	    VA_ATTRIB_NOT_SUPPORTED) {
		printf("Support VAConfigAttribEncMacroblockInfo\n");
	}

	return 0;
}

static int setup_encode()
{
	VAStatus va_status;
	VASurfaceID *tmp_surfaceid;
	int codedbuf_size, i;

	va_status = vaCreateConfig(va_dpy, h264_profile, VAEntrypointEncSlice,
				   &config_attrib[0], config_attrib_num,
				   &config_id);
	CHECK_VASTATUS(va_status, "vaCreateConfig");

	/* create source surfaces */
	va_status = vaCreateSurfaces(va_dpy,
				     VA_RT_FORMAT_YUV420, frame_width_mbaligned,
				     frame_height_mbaligned, &src_surface[0],
				     SURFACE_NUM, NULL, 0);
	CHECK_VASTATUS(va_status, "vaCreateSurfaces");

	/* create reference surfaces */
	va_status = vaCreateSurfaces(va_dpy,
				     VA_RT_FORMAT_YUV420, frame_width_mbaligned,
				     frame_height_mbaligned, &ref_surface[0],
				     SURFACE_NUM, NULL, 0);
	CHECK_VASTATUS(va_status, "vaCreateSurfaces");

	tmp_surfaceid = calloc(2 * SURFACE_NUM, sizeof(VASurfaceID));
	memcpy(tmp_surfaceid, src_surface, SURFACE_NUM * sizeof(VASurfaceID));
	memcpy(tmp_surfaceid + SURFACE_NUM, ref_surface,
	       SURFACE_NUM * sizeof(VASurfaceID));

	/* Create a context for this encode pipe, reference all the src and ref surfaces */
	va_status = vaCreateContext(va_dpy, config_id,
				    frame_width_mbaligned,
				    frame_height_mbaligned, VA_PROGRESSIVE,
				    tmp_surfaceid, 2 * SURFACE_NUM,
				    &context_id);
	CHECK_VASTATUS(va_status, "vaCreateContext");
	free(tmp_surfaceid);

	codedbuf_size =
	    (frame_width_mbaligned * frame_height_mbaligned * 400) / (16 * 16);

	for (i = 0; i < SURFACE_NUM; i++) {
		/* create coded buffer once for all
		 * other VA buffers which won't be used again after vaRenderPicture.
		 * so APP can always vaCreateBuffer for every frame
		 * but coded buffer need to be mapped and accessed after vaRenderPicture/vaEndPicture
		 * so VA won't maintain the coded buffer
		 */
		va_status =
		    vaCreateBuffer(va_dpy, context_id, VAEncCodedBufferType,
				   codedbuf_size, 1, NULL, &coded_buf[i]);
		CHECK_VASTATUS(va_status, "vaCreateBuffer");
	}

	return 0;
}

#define partition(ref, field, key, ascending)   \
    while (i <= j) {                            \
        if (ascending) {                        \
            while (ref[i].field < key)          \
                i++;                            \
            while (ref[j].field > key)          \
                j--;                            \
        } else {                                \
            while (ref[i].field > key)          \
                i++;                            \
            while (ref[j].field < key)          \
                j--;                            \
        }                                       \
        if (i <= j) {                           \
            tmp = ref[i];                       \
            ref[i] = ref[j];                    \
            ref[j] = tmp;                       \
            i++;                                \
            j--;                                \
        }                                       \
    }                                           \

static void sort_one(VAPictureH264 ref[], int left, int right,
		     int ascending, int frame_idx)
{
	int i = left, j = right;
	unsigned int key;
	VAPictureH264 tmp;

	if (frame_idx) {
		key = ref[(left + right) / 2].frame_idx;
		partition(ref, frame_idx, key, ascending);
	} else {
		key = ref[(left + right) / 2].TopFieldOrderCnt;
		partition(ref, TopFieldOrderCnt, (signed int)key, ascending);
	}

	/* recursion */
	if (left < j)
		sort_one(ref, left, j, ascending, frame_idx);

	if (i < right)
		sort_one(ref, i, right, ascending, frame_idx);
}

static void sort_two(VAPictureH264 ref[], int left, int right, unsigned int key,
		     unsigned int frame_idx, int partition_ascending,
		     int list0_ascending, int list1_ascending)
{
	int i = left, j = right;
	VAPictureH264 tmp;

	if (frame_idx) {
		partition(ref, frame_idx, key, partition_ascending);
	} else {
		partition(ref, TopFieldOrderCnt, (signed int)key,
			  partition_ascending);
	}

	sort_one(ref, left, i - 1, list0_ascending, frame_idx);
	sort_one(ref, j + 1, right, list1_ascending, frame_idx);
}

static int update_ReferenceFrames(void)
{
	int i;

	if (current_frame_type == FRAME_B)
		return 0;

	CurrentCurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
	numShortTerm++;
	if (numShortTerm > num_ref_frames)
		numShortTerm = num_ref_frames;
	for (i = numShortTerm - 1; i > 0; i--)
		ReferenceFrames[i] = ReferenceFrames[i - 1];
	ReferenceFrames[0] = CurrentCurrPic;

	if (current_frame_type != FRAME_B)
		current_frame_num++;
	if (current_frame_num > MaxFrameNum)
		current_frame_num = 0;

	return 0;
}

static int update_RefPicList(void)
{
	unsigned int current_poc = CurrentCurrPic.TopFieldOrderCnt;

	if (current_frame_type == FRAME_P) {
		memcpy(RefPicList0_P, ReferenceFrames,
		       numShortTerm * sizeof(VAPictureH264));
		sort_one(RefPicList0_P, 0, numShortTerm - 1, 0, 1);
	}

	if (current_frame_type == FRAME_B) {
		memcpy(RefPicList0_B, ReferenceFrames,
		       numShortTerm * sizeof(VAPictureH264));
		sort_two(RefPicList0_B, 0, numShortTerm - 1, current_poc, 0, 1,
			 0, 1);

		memcpy(RefPicList1_B, ReferenceFrames,
		       numShortTerm * sizeof(VAPictureH264));
		sort_two(RefPicList1_B, 0, numShortTerm - 1, current_poc, 0, 0,
			 1, 0);
	}

	return 0;
}

static int render_sequence(void)
{
	VABufferID seq_param_buf, rc_param_buf, misc_param_tmpbuf, render_id[2];
	VAStatus va_status;
	VAEncMiscParameterBuffer *misc_param, *misc_param_tmp;
	VAEncMiscParameterRateControl *misc_rate_ctrl;

	seq_param.level_idc = 41 /*SH_LEVEL_3 */ ;
	seq_param.picture_width_in_mbs = frame_width_mbaligned / 16;
	seq_param.picture_height_in_mbs = frame_height_mbaligned / 16;
	seq_param.bits_per_second = encoder_frame_bitrate;

	seq_param.intra_period = intra_period;
	seq_param.intra_idr_period = intra_idr_period;
	seq_param.ip_period = ip_period;

	seq_param.max_num_ref_frames = num_ref_frames;
	seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
	seq_param.time_scale = 900;
	seq_param.num_units_in_tick = 15;	/* Tc = num_units_in_tick / time_sacle */
	seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 =
	    Log2MaxPicOrderCntLsb - 4;
	seq_param.seq_fields.bits.log2_max_frame_num_minus4 =
	    Log2MaxFrameNum - 4;;
	seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
	seq_param.seq_fields.bits.chroma_format_idc = 1;
	seq_param.seq_fields.bits.direct_8x8_inference_flag = 1;

	if (frame_width != frame_width_mbaligned ||
	    frame_height != frame_height_mbaligned) {
		seq_param.frame_cropping_flag = 1;
		seq_param.frame_crop_left_offset = 0;
		seq_param.frame_crop_right_offset =
		    (frame_width_mbaligned - frame_width) / 2;
		seq_param.frame_crop_top_offset = 0;
		seq_param.frame_crop_bottom_offset =
		    (frame_height_mbaligned - frame_height) / 2;
	}

	va_status = vaCreateBuffer(va_dpy, context_id,
				   VAEncSequenceParameterBufferType,
				   sizeof(seq_param), 1, &seq_param,
				   &seq_param_buf);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");

	va_status = vaCreateBuffer(va_dpy, context_id,
				   VAEncMiscParameterBufferType,
				   sizeof(VAEncMiscParameterBuffer) +
				   sizeof(VAEncMiscParameterRateControl), 1,
				   NULL, &rc_param_buf);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");

	vaMapBuffer(va_dpy, rc_param_buf, (void **)&misc_param);
	misc_param->type = VAEncMiscParameterTypeRateControl;
	misc_rate_ctrl = (VAEncMiscParameterRateControl *) misc_param->data;
	memset(misc_rate_ctrl, 0, sizeof(*misc_rate_ctrl));
	misc_rate_ctrl->bits_per_second = encoder_frame_bitrate;
	misc_rate_ctrl->target_percentage = 66;
	misc_rate_ctrl->window_size = 1000;
	misc_rate_ctrl->initial_qp = initial_qp;
	misc_rate_ctrl->min_qp = minimal_qp;
	misc_rate_ctrl->basic_unit_size = 0;
	vaUnmapBuffer(va_dpy, rc_param_buf);

	render_id[0] = seq_param_buf;
	render_id[1] = rc_param_buf;

	va_status = vaRenderPicture(va_dpy, context_id, &render_id[0], 2);
	CHECK_VASTATUS(va_status, "vaRenderPicture");;

	if (misc_priv_type != 0) {
		va_status = vaCreateBuffer(va_dpy, context_id,
					   VAEncMiscParameterBufferType,
					   sizeof(VAEncMiscParameterBuffer),
					   1, NULL, &misc_param_tmpbuf);
		CHECK_VASTATUS(va_status, "vaCreateBuffer");
		vaMapBuffer(va_dpy, misc_param_tmpbuf,
			    (void **)&misc_param_tmp);
		misc_param_tmp->type = misc_priv_type;
		misc_param_tmp->data[0] = misc_priv_value;
		vaUnmapBuffer(va_dpy, misc_param_tmpbuf);

		va_status =
		    vaRenderPicture(va_dpy, context_id, &misc_param_tmpbuf, 1);

		vaDestroyBuffer(va_dpy, misc_param_tmpbuf);
	}
	vaDestroyBuffer(va_dpy, seq_param_buf);
	vaDestroyBuffer(va_dpy, rc_param_buf);

	return 0;
}

static int calc_poc(int pic_order_cnt_lsb)
{
	static int PicOrderCntMsb_ref = 0, pic_order_cnt_lsb_ref = 0;
	int prevPicOrderCntMsb, prevPicOrderCntLsb;
	int PicOrderCntMsb, TopFieldOrderCnt;

	if (current_frame_type == FRAME_IDR)
		prevPicOrderCntMsb = prevPicOrderCntLsb = 0;
	else {
		prevPicOrderCntMsb = PicOrderCntMsb_ref;
		prevPicOrderCntLsb = pic_order_cnt_lsb_ref;
	}

	if ((pic_order_cnt_lsb < prevPicOrderCntLsb) &&
	    ((prevPicOrderCntLsb - pic_order_cnt_lsb) >=
	     (int)(MaxPicOrderCntLsb / 2)))
		PicOrderCntMsb = prevPicOrderCntMsb + MaxPicOrderCntLsb;
	else if ((pic_order_cnt_lsb > prevPicOrderCntLsb) &&
		 ((pic_order_cnt_lsb - prevPicOrderCntLsb) >
		  (int)(MaxPicOrderCntLsb / 2)))
		PicOrderCntMsb = prevPicOrderCntMsb - MaxPicOrderCntLsb;
	else
		PicOrderCntMsb = prevPicOrderCntMsb;

	TopFieldOrderCnt = PicOrderCntMsb + pic_order_cnt_lsb;

	if (current_frame_type != FRAME_B) {
		PicOrderCntMsb_ref = PicOrderCntMsb;
		pic_order_cnt_lsb_ref = pic_order_cnt_lsb;
	}

	return TopFieldOrderCnt;
}

static int render_picture(void)
{
	VABufferID pic_param_buf;
	VAStatus va_status;
	int i = 0;

	pic_param.CurrPic.picture_id = ref_surface[current_slot];
	pic_param.CurrPic.frame_idx = current_frame_num;
	pic_param.CurrPic.flags = 0;
	pic_param.CurrPic.TopFieldOrderCnt =
	    calc_poc((current_frame_display -
		      current_IDR_display) % MaxPicOrderCntLsb);
	pic_param.CurrPic.BottomFieldOrderCnt =
	    pic_param.CurrPic.TopFieldOrderCnt;
	CurrentCurrPic = pic_param.CurrPic;

	if (getenv("TO_DEL")) {	/* set RefPicList into ReferenceFrames */
		update_RefPicList();	/* calc RefPicList */
		memset(pic_param.ReferenceFrames, 0xff, 16 * sizeof(VAPictureH264));	/* invalid all */
		if (current_frame_type == FRAME_P) {
			pic_param.ReferenceFrames[0] = RefPicList0_P[0];
		} else if (current_frame_type == FRAME_B) {
			pic_param.ReferenceFrames[0] = RefPicList0_B[0];
			pic_param.ReferenceFrames[1] = RefPicList1_B[0];
		}
	} else {
		memcpy(pic_param.ReferenceFrames, ReferenceFrames,
		       numShortTerm * sizeof(VAPictureH264));
		for (i = numShortTerm; i < SURFACE_NUM; i++) {
			pic_param.ReferenceFrames[i].picture_id =
			    VA_INVALID_SURFACE;
			pic_param.ReferenceFrames[i].flags =
			    VA_PICTURE_H264_INVALID;
		}
	}

	pic_param.pic_fields.bits.idr_pic_flag =
	    (current_frame_type == FRAME_IDR);
	pic_param.pic_fields.bits.reference_pic_flag =
	    (current_frame_type != FRAME_B);
	pic_param.pic_fields.bits.entropy_coding_mode_flag = h264_entropy_mode;
	pic_param.pic_fields.bits.deblocking_filter_control_present_flag = 1;
	pic_param.frame_num = current_frame_num;
	pic_param.coded_buf = coded_buf[current_slot];
	pic_param.last_picture = (current_frame_encoding == frame_count);
	pic_param.pic_init_qp = initial_qp;

	va_status =
	    vaCreateBuffer(va_dpy, context_id, VAEncPictureParameterBufferType,
			   sizeof(pic_param), 1, &pic_param, &pic_param_buf);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");;

	va_status = vaRenderPicture(va_dpy, context_id, &pic_param_buf, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture");

	vaDestroyBuffer(va_dpy, pic_param_buf);

	return 0;
}

static int render_packedsequence(void)
{
	VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
	VABufferID packedseq_para_bufid, packedseq_data_bufid, render_id[2];
	unsigned int length_in_bits;
	unsigned char *packedseq_buffer = NULL;
	VAStatus va_status;

	length_in_bits = build_packed_seq_buffer(&packedseq_buffer);

	packedheader_param_buffer.type = VAEncPackedHeaderSequence;

	packedheader_param_buffer.bit_length = length_in_bits;	/*length_in_bits */
	packedheader_param_buffer.has_emulation_bytes = 0;
	va_status = vaCreateBuffer(va_dpy,
				   context_id,
				   VAEncPackedHeaderParameterBufferType,
				   sizeof(packedheader_param_buffer), 1,
				   &packedheader_param_buffer,
				   &packedseq_para_bufid);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");

	va_status = vaCreateBuffer(va_dpy,
				   context_id,
				   VAEncPackedHeaderDataBufferType,
				   (length_in_bits + 7) / 8, 1,
				   packedseq_buffer, &packedseq_data_bufid);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");

	render_id[0] = packedseq_para_bufid;
	render_id[1] = packedseq_data_bufid;
	va_status = vaRenderPicture(va_dpy, context_id, render_id, 2);
	CHECK_VASTATUS(va_status, "vaRenderPicture");

	free(packedseq_buffer);

	vaDestroyBuffer(va_dpy, packedseq_para_bufid);
	vaDestroyBuffer(va_dpy, packedseq_data_bufid);

	return 0;
}

static int render_packedpicture(void)
{
	VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
	VABufferID packedpic_para_bufid, packedpic_data_bufid, render_id[2];
	unsigned int length_in_bits;
	unsigned char *packedpic_buffer = NULL;
	VAStatus va_status;

	length_in_bits = build_packed_pic_buffer(&packedpic_buffer);
	packedheader_param_buffer.type = VAEncPackedHeaderPicture;
	packedheader_param_buffer.bit_length = length_in_bits;
	packedheader_param_buffer.has_emulation_bytes = 0;

	va_status = vaCreateBuffer(va_dpy,
				   context_id,
				   VAEncPackedHeaderParameterBufferType,
				   sizeof(packedheader_param_buffer), 1,
				   &packedheader_param_buffer,
				   &packedpic_para_bufid);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");

	va_status = vaCreateBuffer(va_dpy,
				   context_id,
				   VAEncPackedHeaderDataBufferType,
				   (length_in_bits + 7) / 8, 1,
				   packedpic_buffer, &packedpic_data_bufid);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");

	render_id[0] = packedpic_para_bufid;
	render_id[1] = packedpic_data_bufid;
	va_status = vaRenderPicture(va_dpy, context_id, render_id, 2);
	CHECK_VASTATUS(va_status, "vaRenderPicture");

	free(packedpic_buffer);

	vaDestroyBuffer(va_dpy, packedpic_para_bufid);
	vaDestroyBuffer(va_dpy, packedpic_data_bufid);

	return 0;
}

#if 1
static void render_packedsei(void)
{
	VAEncPackedHeaderParameterBuffer packed_header_param_buffer;
	VABufferID packed_sei_header_param_buf_id, packed_sei_buf_id,
	    render_id[2];
	unsigned int length_in_bits /*offset_in_bytes */ ;
	unsigned char *packed_sei_buffer = NULL;
	VAStatus va_status;
	int init_cpb_size, target_bit_rate, i_initial_cpb_removal_delay_length,
	    i_initial_cpb_removal_delay;
	int i_cpb_removal_delay, i_dpb_output_delay_length,
	    i_cpb_removal_delay_length;

	/* it comes for the bps defined in SPS */
	target_bit_rate = encoder_frame_bitrate;
	init_cpb_size = (target_bit_rate * 8) >> 10;
	i_initial_cpb_removal_delay =
	    init_cpb_size * 0.5 * 1024 / target_bit_rate * 90000;

	i_cpb_removal_delay = 2;
	i_initial_cpb_removal_delay_length = 24;
	i_cpb_removal_delay_length = 24;
	i_dpb_output_delay_length = 24;

	length_in_bits =
	    build_packed_sei_buffer_timing(i_initial_cpb_removal_delay_length,
					   i_initial_cpb_removal_delay, 0,
					   i_cpb_removal_delay_length,
					   i_cpb_removal_delay *
					   current_frame_encoding,
					   i_dpb_output_delay_length, 0,
					   &packed_sei_buffer);

	//offset_in_bytes = 0;
	packed_header_param_buffer.type = VAEncPackedHeaderH264_SEI;
	packed_header_param_buffer.bit_length = length_in_bits;
	packed_header_param_buffer.has_emulation_bytes = 0;

	va_status = vaCreateBuffer(va_dpy,
				   context_id,
				   VAEncPackedHeaderParameterBufferType,
				   sizeof(packed_header_param_buffer), 1,
				   &packed_header_param_buffer,
				   &packed_sei_header_param_buf_id);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");

	va_status = vaCreateBuffer(va_dpy,
				   context_id,
				   VAEncPackedHeaderDataBufferType,
				   (length_in_bits + 7) / 8, 1,
				   packed_sei_buffer, &packed_sei_buf_id);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");

	render_id[0] = packed_sei_header_param_buf_id;
	render_id[1] = packed_sei_buf_id;
	va_status = vaRenderPicture(va_dpy, context_id, render_id, 2);
	CHECK_VASTATUS(va_status, "vaRenderPicture");

	free(packed_sei_buffer);

	vaDestroyBuffer(va_dpy, packed_sei_header_param_buf_id);
	vaDestroyBuffer(va_dpy, packed_sei_buf_id);

	return;
}

static int render_hrd(void)
{
	VABufferID misc_parameter_hrd_buf_id;
	VAStatus va_status;
	VAEncMiscParameterBuffer *misc_param;
	VAEncMiscParameterHRD *misc_hrd_param;

	va_status = vaCreateBuffer(va_dpy, context_id,
				   VAEncMiscParameterBufferType,
				   sizeof(VAEncMiscParameterBuffer) +
				   sizeof(VAEncMiscParameterHRD), 1, NULL,
				   &misc_parameter_hrd_buf_id);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");

	vaMapBuffer(va_dpy, misc_parameter_hrd_buf_id, (void **)&misc_param);
	misc_param->type = VAEncMiscParameterTypeHRD;
	misc_hrd_param = (VAEncMiscParameterHRD *) misc_param->data;

	if (encoder_frame_bitrate > 0) {
		misc_hrd_param->initial_buffer_fullness =
		    encoder_frame_bitrate * 1024 * 4;
		misc_hrd_param->buffer_size = encoder_frame_bitrate * 1024 * 8;
	} else {
		misc_hrd_param->initial_buffer_fullness = 0;
		misc_hrd_param->buffer_size = 0;
	}
	vaUnmapBuffer(va_dpy, misc_parameter_hrd_buf_id);

	va_status =
	    vaRenderPicture(va_dpy, context_id, &misc_parameter_hrd_buf_id, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture");;

	vaDestroyBuffer(va_dpy, misc_parameter_hrd_buf_id);

	return 0;
}
#endif

static int render_slice(void)
{
	VABufferID slice_param_buf;
	VAStatus va_status;
	int i;

	update_RefPicList();

	/* one frame, one slice */
	slice_param.macroblock_address = 0;
	slice_param.num_macroblocks = frame_width_mbaligned * frame_height_mbaligned / (16 * 16);	/* Measured by MB */
	slice_param.slice_type =
	    (current_frame_type == FRAME_IDR) ? 2 : current_frame_type;
	if (current_frame_type == FRAME_IDR) {
		if (current_frame_encoding != 0)
			++slice_param.idr_pic_id;
	} else if (current_frame_type == FRAME_P) {
		int refpiclist0_max = h264_maxref & 0xffff;
		memcpy(slice_param.RefPicList0, RefPicList0_P,
		       refpiclist0_max * sizeof(VAPictureH264));

		for (i = refpiclist0_max; i < 32; i++) {
			slice_param.RefPicList0[i].picture_id =
			    VA_INVALID_SURFACE;
			slice_param.RefPicList0[i].flags =
			    VA_PICTURE_H264_INVALID;
		}
	} else if (current_frame_type == FRAME_B) {
		int refpiclist0_max = h264_maxref & 0xffff;
		int refpiclist1_max = (h264_maxref >> 16) & 0xffff;

		memcpy(slice_param.RefPicList0, RefPicList0_B,
		       refpiclist0_max * sizeof(VAPictureH264));
		for (i = refpiclist0_max; i < 32; i++) {
			slice_param.RefPicList0[i].picture_id =
			    VA_INVALID_SURFACE;
			slice_param.RefPicList0[i].flags =
			    VA_PICTURE_H264_INVALID;
		}

		memcpy(slice_param.RefPicList1, RefPicList1_B,
		       refpiclist1_max * sizeof(VAPictureH264));
		for (i = refpiclist1_max; i < 32; i++) {
			slice_param.RefPicList1[i].picture_id =
			    VA_INVALID_SURFACE;
			slice_param.RefPicList1[i].flags =
			    VA_PICTURE_H264_INVALID;
		}
	}

	slice_param.slice_alpha_c0_offset_div2 = 0;
	slice_param.slice_beta_offset_div2 = 0;
	slice_param.direct_spatial_mv_pred_flag = 1;
	slice_param.pic_order_cnt_lsb =
	    (current_frame_display - current_IDR_display) % MaxPicOrderCntLsb;

	va_status =
	    vaCreateBuffer(va_dpy, context_id, VAEncSliceParameterBufferType,
			   sizeof(slice_param), 1, &slice_param,
			   &slice_param_buf);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");;

	va_status = vaRenderPicture(va_dpy, context_id, &slice_param_buf, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture");

	vaDestroyBuffer(va_dpy, slice_param_buf);

	return 0;
}

static int save_codeddata(unsigned long long display_order,
			  unsigned long long encode_order)
{
	VACodedBufferSegment *buf_list = NULL;
	VAStatus va_status;
	unsigned int coded_size = 0;

	va_status =
	    vaMapBuffer(va_dpy, coded_buf[display_order % SURFACE_NUM],
			(void **)(&buf_list));
	CHECK_VASTATUS(va_status, "vaMapBuffer");

	while (buf_list != NULL) {
		if (nal_fp)
			coded_size += fwrite(buf_list->buf, 1, buf_list->size, nal_fp);
		else
			coded_size = buf_list->size;

		/* ... will drop the packet if ES2TS was not requested */
		sendESPacket(buf_list->buf, buf_list->size);

		/* ... will drop the packet if RTP was not requested */
		sendRTPPacket(buf_list->buf, buf_list->size);

		buf_list = (VACodedBufferSegment *) buf_list->next;
		frame_size += coded_size;
	}
	vaUnmapBuffer(va_dpy, coded_buf[display_order % SURFACE_NUM]);

	printf("\r      ");	/* return back to startpoint */
	switch (encode_order % 4) {
	case 0:
		printf("|");
		break;
	case 1:
		printf("/");
		break;
	case 2:
		printf("-");
		break;
	case 3:
		printf("\\");
		break;
	}
	printf("%08lld", encode_order);
	printf("(%06d bytes coded)", coded_size);

	fflush(nal_fp);

	return 0;
}

static struct storage_task_t *storage_task_dequeue(void)
{
	struct storage_task_t *header;

	pthread_mutex_lock(&encode_mutex);

	header = storage_task_header;
	if (storage_task_header != NULL) {
		if (storage_task_tail == storage_task_header)
			storage_task_tail = NULL;
		storage_task_header = header->next;
	}

	pthread_mutex_unlock(&encode_mutex);

	return header;
}

static int storage_task_queue(unsigned long long display_order,
			      unsigned long long encode_order)
{
	struct storage_task_t *tmp;

	tmp = calloc(1, sizeof(struct storage_task_t));
	tmp->display_order = display_order;
	tmp->encode_order = encode_order;

	pthread_mutex_lock(&encode_mutex);

	if (storage_task_header == NULL) {
		storage_task_header = tmp;
		storage_task_tail = tmp;
	} else {
		storage_task_tail->next = tmp;
		storage_task_tail = tmp;
	}

	srcsurface_status[display_order % SURFACE_NUM] = SRC_SURFACE_IN_STORAGE;
	pthread_cond_signal(&encode_cond);

	pthread_mutex_unlock(&encode_mutex);

	return 0;
}

static void storage_task(unsigned long long display_order,
			 unsigned long long encode_order)
{
	VAStatus va_status;

	va_status = vaSyncSurface(va_dpy, src_surface[display_order % SURFACE_NUM]);
	CHECK_VASTATUS(va_status, "vaSyncSurface");
	save_codeddata(display_order, encode_order);

	/* reload a new frame data */

	pthread_mutex_lock(&encode_mutex);
	srcsurface_status[display_order % SURFACE_NUM] = SRC_SURFACE_IN_ENCODING;
	pthread_mutex_unlock(&encode_mutex);
}

static void *storage_task_thread(void *t)
{
	while (1) {
		struct storage_task_t *current;

		current = storage_task_dequeue();
		if (current == NULL) {
			pthread_mutex_lock(&encode_mutex);
			pthread_cond_wait(&encode_cond, &encode_mutex);
			pthread_mutex_unlock(&encode_mutex);
			continue;
		}

		storage_task(current->display_order, current->encode_order);

		free(current);
	}

	return 0;
}

/* Map a surface, shift the inbuf pixels into it */
static void upload_yuv_to_surface(unsigned char *inbuf, VASurfaceID surface_id,
				  int picture_width,
				  int picture_height)
{
	VAImage image;
	VAStatus va_status;
	void *pbuffer = NULL;
	unsigned char *psrc = inbuf;
	unsigned char *pdst = NULL;
	unsigned char *dst_y, *dst_uv;
	unsigned char *dst_uv_line = NULL;
	int i, j, pw;
	
	va_status = vaDeriveImage(va_dpy, surface_id, &image);
	va_status = vaMapBuffer(va_dpy, image.buf, &pbuffer);
	pdst = (unsigned char *)pbuffer;
	dst_uv_line = pdst + image.offsets[1];
	dst_uv = dst_uv_line;

	/* For each pair of lines */
	pw = picture_width / 2;
	for (i = 0; i < picture_height; i += 2) {
		dst_y = (pdst + image.offsets[0]) + i * image.pitches[0];
#if 0
		/* Switch to single DWORD reads/writes to try and improve
		 * memory performance. Sadly, the entire code saved < 1%
		 * and has a few visual glitches. Abandoning this.
		 */
		unsigned int *pdst_y = (unsigned int *)dst_y;
		unsigned int *pdst_uv = (unsigned int *)dst_uv;
		for (j = 0; j < pw / 2; ++j) {
			dw1 = *dw++;
			dw2 = *dw++;
			dw3 = *dw;

			t  = dw1 & 0xff;
			t |= ((dw1 & 0x00ff0000) >> 8);
			t |= (dw2 & 0xff) << 16;
			t |= (dw2 & 0x00ff0000) << 8;

			z  = ((dw1 & 0xff00) >> 8);
			z |= ((dw1 & 0xff000000) >> 16);
			z |= ((dw2 & 0xff00) << 8);
			z |= (dw2 & 0xff000000);

			*(pdst_y++) = t;
			*(pdst_uv++) = z;
		}

		dst_y = (pdst + image.offsets[0]) + (i + 1) * image.pitches[0];
		pdst_y = (unsigned int *)dst_y;
		for (j = 0; j < pw / 2; ++j) {

			t  = dw2 & 0xff;
			t |= ((dw2 & 0x00ff0000) >> 8);
			t |= (dw3 & 0xff) << 16;
			t |= (dw3 & 0x00ff0000) << 8;

			*(pdst_y++) = t;

			dw += 2;
		}
#endif
		for (j = 0; j < pw; ++j) {
			*(dst_y++)  = psrc[0];	// y1;
			*(dst_uv++) = psrc[1];	// u;
			*(dst_y++)  = psrc[2];	// y1;
			*(dst_uv++) = psrc[3];	// v;
			psrc += 4;
		}

		dst_y = (pdst + image.offsets[0]) + (i + 1) * image.pitches[0];
		for (j = 0; j < pw; ++j) {
			*(dst_y++) = psrc[0];	//y1;
			*(dst_y++) = psrc[2];	//y2;
			psrc += 4;
		}

		dst_uv_line += image.pitches[1];
		dst_uv = dst_uv_line;
	}

	va_status = vaUnmapBuffer(va_dpy, image.buf);
	CHECK_VASTATUS(va_status, "vaUnmapBuffer");

	va_status = vaDestroyImage(va_dpy, image.image_id);
	CHECK_VASTATUS(va_status, "vaDestroyImage");
}

static int encode_YUY2_frame(unsigned char *frame)
{
	unsigned int i;
	VAStatus va_status;
	static int preload = 0;

	if (preload++ == 0) {
		/* upload RAW YUV data into all surfaces, so the compressor doesn't assert in our first few
		 * real frames.
		 */
		for (i = 0; i < SURFACE_NUM; i++)
			upload_yuv_to_surface(frame, src_surface[i], frame_width, frame_height);
	} else {
		/* TODO: We probably don't need to specifically upload non de-interlaced content to the
		 * current slot, it's probably OK to run the stream 1 frame behind live and always
		 * upload to the same slot regardless of whether VPP is enabled or not.
		 * IE. Most likely we should always upload to the prior slot.
		 */
		if (vpp_deinterlace_mode > 0)
			upload_yuv_to_surface(frame, src_surface[prior_slot()], frame_width, frame_height);
		else
			upload_yuv_to_surface(frame, src_surface[current_slot], frame_width, frame_height);
	}

	/* Once only - Create the encoding thread */
	if (encode_syncmode == 0 && (encode_thread == (pthread_t)-1))
		pthread_create(&encode_thread, NULL, storage_task_thread, NULL);

	if (vpp_deinterlace_mode > 0) {
		/* (input surface, output surface) Take new clean data, merge into current during encoding. */
		vpp_perform_deinterlace(src_surface[current_slot], frame_width, frame_height, src_surface[next_slot]);
		vpp_perform_deinterlace(src_surface[prior_slot()], frame_width, frame_height, src_surface[current_slot]);
	}

	encoding2display_order(current_frame_encoding,
			       intra_period,
			       intra_idr_period,
			       ip_period,
			       &current_frame_display, &current_frame_type);

	if (current_frame_type == FRAME_IDR) {
		numShortTerm = 0;
		current_frame_num = 0;
		current_IDR_display = current_frame_display;
	}

	/* Wait for the current surface to become ready */
	while (srcsurface_status[current_slot] != SRC_SURFACE_IN_ENCODING) {
		usleep(1);
	}

	va_status = vaBeginPicture(va_dpy, context_id, src_surface[current_slot]);
	CHECK_VASTATUS(va_status, "vaBeginPicture");

	if (current_frame_type == FRAME_IDR) {
		render_sequence();
		render_picture();
		if (h264_packedheader) {
			render_packedsequence();
			render_packedpicture();
		}
		if (rc_mode == VA_RC_CBR)
		    render_packedsei();
		render_hrd();
	} else {
		//render_sequence();
		render_picture();
		if (rc_mode == VA_RC_CBR)
		    render_packedsei();
		render_hrd();
	}
	render_slice();

	va_status = vaEndPicture(va_dpy, context_id);
	CHECK_VASTATUS(va_status, "vaEndPicture");

	if (encode_syncmode) {
		storage_task(current_frame_display, current_frame_encoding);
	} else {
		/* queue the storage task queue */
		storage_task_queue(current_frame_display,
				   current_frame_encoding);
	}

	update_ReferenceFrames();

	current_frame_encoding++;

	return 1;
}

static int release_encode()
{
	int i;

	vaDestroySurfaces(va_dpy, &src_surface[0], SURFACE_NUM);
	vaDestroySurfaces(va_dpy, &ref_surface[0], SURFACE_NUM);

	for (i = 0; i < SURFACE_NUM; i++)
		vaDestroyBuffer(va_dpy, coded_buf[i]);

	vaDestroyContext(va_dpy, context_id);
	vaDestroyConfig(va_dpy, config_id);

	return 0;
}

static int deinit_va()
{
	deinit_vpp();

	vaTerminate(va_dpy);

	va_close_display(va_dpy);

	return 0;
}

static int print_input()
{
	printf("\n\nINPUT:Try to encode H264...\n");
	printf("INPUT: RateControl  : %s\n", rc_to_string(rc_mode));
	printf("INPUT: Resolution   : %dx%d, %d frames\n",
	       frame_width, frame_height, frame_count);
	printf("INPUT: FrameRate    : %d\n", frame_rate);
	printf("INPUT: Bitrate      : %d\n", encoder_frame_bitrate);
	printf("INPUT: Slices       : %d\n", frame_slices);
	printf("INPUT: IntraPeriod  : %d\n", intra_period);
	printf("INPUT: IDRPeriod    : %d\n", intra_idr_period);
	printf("INPUT: IpPeriod     : %d\n", ip_period);
	printf("INPUT: Initial QP   : %d\n", initial_qp);
	printf("INPUT: Min QP       : %d\n", minimal_qp);
	printf("INPUT: Coded Clip   : %s\n", encoder_nalOutputFilename);
	printf("\n\n");		/* return back to startpoint */

	return 0;
}

int encoder_init(struct encoder_params_s *params)
{
	assert(params);
	printf("%s(%d, %d)\n", __func__, params->width, params->height);

	frame_width = params->width;
	frame_height = params->height;
	frame_rate = 30;
	h264_profile = VAProfileH264High;
	intra_idr_period = frame_rate;
	initial_qp = params->initial_qp;
	minimal_qp = params->minimal_qp;
	vpp_deinterlace_mode = params->deinterlacemode;

	current_frame_encoding = 0;
	encode_syncmode = 0;

	/* ready for encoding */
	memset(srcsurface_status, SRC_SURFACE_IN_ENCODING, sizeof(srcsurface_status));
	memset(&seq_param, 0, sizeof(seq_param));
	memset(&pic_param, 0, sizeof(pic_param));
	memset(&slice_param, 0, sizeof(slice_param));

	encoder_display_init(&display_ctx);

	if (params->enable_osd) {
		frame_osd = 1;
		frame_osd_length = (params->width * 2) * params->height;
	}

	/* store coded data into a file */
	if (encoder_nalOutputFilename) {
		nal_fp = fopen(encoder_nalOutputFilename, "w+");
		if (nal_fp == NULL) {
			printf("Open file %s failed, exit\n", encoder_nalOutputFilename);
			exit(1);
		}
	}

	frame_width_mbaligned = (frame_width + 15) & (~15);
	frame_height_mbaligned = (frame_height + 15) & (~15);
	if (frame_width != frame_width_mbaligned
	    || frame_height != frame_height_mbaligned) {
		printf
		    ("Source frame is %dx%d and will code clip to %dx%d with crop\n",
		     frame_width, frame_height, frame_width_mbaligned,
		     frame_height_mbaligned);
	}

	print_input();

	init_va();
	init_vpp();
	setup_encode();
	return 0;
}

void encoder_close()
{
	release_encode();
	deinit_va();
}

int encoder_encode_frame(unsigned char *inbuf)
{
	if (!inbuf)
		return 0;

#if 0
	/* Grab a frame - we'll use this for the static image */
	static int fnr = 0;
	if (fnr++ == 800) {
		FILE *fh = fopen("/tmp/frame800.yuy2", "wb");
		if (fh) {
			fwrite(inbuf, (frame_width * 2) * frame_height, 1, fh);
			fclose(fh);
		}
	}
#endif

	if (frame_osd) {
		/* Warning: We're going to directly modify the input pixels. In fixed
		 * frame encoding we'll continuiously overwrite and alter the static
		 * image. If for any reason our OSD strings below begin to shorten,
		 * we'll leave old pixel data in the source image.
		 * This is intensional and saves an additional frame copy.
		 */
		encoder_display_render_reset(&display_ctx, inbuf, frame_width * 2);

		/* Render any OSD */
		char str[256];
		time_t now = time(NULL);
		struct tm *tm = localtime(&now);
		sprintf(str, "%04d/%02d/%02d-%02d:%02d:%02d",
			tm->tm_year + 1900,
			tm->tm_mon + 1,
			tm->tm_mday,
			tm->tm_hour,
			tm->tm_min,
			tm->tm_sec
			);
		encoder_display_render_string(&display_ctx, (unsigned char*)str, strlen(str), 0, 10);

		sprintf(str, "FRM: %lld", frames_processed);
		encoder_display_render_string(&display_ctx, (unsigned char*)str, strlen(str), 0, 11);
	}

	encode_YUY2_frame(inbuf);

	frames_processed++;
	return 1;
}

