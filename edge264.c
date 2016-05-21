// TODO: Test with lint.
// TODO: Deoptimise and comment!
// TODO: Fix all unsigned " / " and " * " operations
// TODO: Study the value of max_dec_frame_buffering vs max_num_ref_frames in clips
// TODO: Consider storing RefPicList in field mode after implementing MBAFF
// TODO: Switch to RefPicList[l * 32 + i] if it later speeds up the decoder loop
// TODO: Put codIOffset and codIRange in GRVs for 16-regs architectures

/**
 * Copyright (c) 2013-2014, Celticom / TVLabs
 * Copyright (c) 2014-2016 Thibault Raffaillac <traf@kth.se>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of their
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "edge264_common.h"
#include "edge264_cabac.c"



static const v16qu Default_4x4_Intra = {
	 6, 13, 20, 28,
	13, 20, 28, 32,
	20, 28, 32, 37,
	28, 32, 37, 42
};
static const v16qu Default_4x4_Inter = {
	10, 14, 20, 24,
	14, 20, 24, 27,
	20, 24, 27, 30,
	24, 27, 30, 34
};
static const v16qu Default_8x8_Intra[4] = {
	{ 6, 10, 13, 16, 18, 23, 25, 27,
	 10, 11, 16, 18, 23, 25, 27, 29},
	{13, 16, 18, 23, 25, 27, 29, 31,
	 16, 18, 23, 25, 27, 29, 31, 33},
	{18, 23, 25, 27, 29, 31, 33, 36,
	 23, 25, 27, 29, 31, 33, 36, 38},
	{25, 27, 29, 31, 33, 36, 38, 40,
	 27, 29, 31, 33, 36, 38, 40, 42}
};
static const v16qu Default_8x8_Inter[4] = {
	{ 9, 13, 15, 17, 19, 21, 22, 24,
	 13, 13, 17, 19, 21, 22, 24, 25},
	{15, 17, 19, 21, 22, 24, 25, 27,
	 17, 19, 21, 22, 24, 25, 27, 28},
	{19, 21, 22, 24, 25, 27, 28, 30,
	 21, 22, 24, 25, 27, 28, 30, 32},
	{22, 24, 25, 27, 28, 30, 32, 33,
	 24, 25, 27, 28, 30, 32, 33, 35}
};



/**
 * Initialises and updates the reference picture lists (8.2.4).
 */
static void parse_ref_pic_list_modification(const Edge264_ctx *e)
{
	// sort the initial list of frames
	const int32_t *values = (s->slice_type == 0) ? e->FrameNum : e->FieldOrderCnt;
	unsigned offset = (s->slice_type == 0) ? e->prevFrameNum : s->TopFieldOrderCnt;
	uint16_t top = e->reference_flags, bot = e->reference_flags >> 16;
	unsigned refs = (s->field_pic_flag) ? top | bot : top & bot;
	int count[3] = {}, next = 0;
	do {
		int best = INT_MAX;
		unsigned r = refs;
		do {
			int i = __builtin_ctz(r);
			int diff = values[i] - offset;
			int ShortTermNum = (diff <= 0) ? -diff : 0x8000 + diff;
			int LongTermFrameNum = e->FrameNum[i] + 0x10000;
			int v = (e->long_term_flags & 1 << i) ? LongTermFrameNum : ShortTermNum;
			if (best > v)
				best = v, next = i;
		} while (r &= r - 1);
		s->RefPicList[0][count[0] + count[1] + count[2]] = next;
		count[best >> 15]++;
	} while (refs ^= 1 << next);
	for (int i = 0; i < 16; i++)
		s->RefPicList[1][(i < count[0]) ? i + count[1] : (i < count[0] + count[1]) ? i - count[0] : i] = s->RefPicList[0][i];
	
	// When decoding a field, extract a list of fields from each list of frames.
	for (int l = 0; s->field_pic_flag && l <= s->slice_type; l++) {
		static const v16qi v16 = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16};
		v16qi v = ((v16qi *)s->RefPicList[l])[0];
		v16qi RefPicList[2] = {v, v + v16};
		int lim = count[0] + count[1], tot = lim + count[2], n = 0;
		int i = s->bottom_field_flag << 4, j = i ^ 16, k;
		do {
			if ((i & 15) >= lim)
				i = (s->bottom_field_flag << 4) + lim, j = i ^ 16, lim = tot;
			int pic = ((int8_t *)RefPicList)[i++];
			if (e->reference_flags & 1 << pic) {
				s->RefPicList[l][n++] = pic;
				if ((j & 15) < lim)
					k = i, i = j, j = k; // swap
			}
		} while ((i & 15) < tot);
	}
	
	// RefPicList0==RefPicList1 can be reduced to testing only the first slot.
	if (s->RefPicList[0][0] == s->RefPicList[1][0] && count[0] + count[1] + count[2] > 1) {
		s->RefPicList[1][0] = s->RefPicList[0][1];
		s->RefPicList[1][1] = s->RefPicList[0][0];
	}
	
	// parse the ref_pic_list_modification() instructions
	for (int l = 0; l <= s->slice_type; l++) {
		unsigned picNumLX = (s->field_pic_flag) ? e->prevFrameNum * 2 + 1 : e->prevFrameNum;
		int modification_of_pic_nums_idc;
		
		// Let's not waste some precious indentation space...
		if (get_u1())
			for (int refIdx = 0; (modification_of_pic_nums_idc = get_ue16()) < 3 && refIdx < 32; refIdx++)
		{
			int num = get_ue32();
			unsigned MaskFrameNum = -1;
			unsigned r = e->long_term_flags;
			if (modification_of_pic_nums_idc < 2) {
				num = picNumLX = (modification_of_pic_nums_idc == 0) ? picNumLX - (num + 1) : picNumLX + (num + 1);
				MaskFrameNum = (1 << s->ps.log2_max_frame_num) - 1;
				r = ~r;
			}
			
			// LongTerm and ShortTerm share this same picture search.
			unsigned FrameNum = MaskFrameNum & (s->field_pic_flag ? num >> 1 : num);
			int pic = ((num & 1) ^ s->bottom_field_flag) << 4;
			r &= (pic) ? e->reference_flags : e->reference_flags >> 16;
			do {
				int i = __builtin_ctz(r);
				if ((e->FrameNum[i] & MaskFrameNum) == FrameNum)
					pic += i; // can only happen once, since each FrameNum is unique
			} while (r &= r - 1);
			
			// insert pic at position refIdx in RefPicList
			int old, new = pic;
			for (int i = refIdx; i < 32 && (old = s->RefPicList[l][i]) != pic; i++)
				s->RefPicList[l][i] = new, new = old;
		}
		
		for (int i = 0; i < s->ps.num_ref_idx_active[l]; i++)
			printf("<li>RefPicList%x[%u]: <code>%u %s</code></li>\n", l, i, e->FieldOrderCnt[s->RefPicList[l][i]], (s->RefPicList[l][i] >> 4) ? "bot" : "top");
	}
	
	// initialisations for the colocated reference picture
	s->MapPicToList0[0] = 0; // when refPicCol == -1
	for (int refIdxL0 = 0; refIdxL0 < 32; refIdxL0++)
		s->MapPicToList0[1 + s->RefPicList[0][refIdxL0]] = refIdxL0;
	s->mbCol = NULL; // FIXME
	s->mvCol = NULL;
	s->col_short_term = ~e->long_term_flags >> (s->RefPicList[1][0] & 15) & 1;
}



/**
 * Stores the pre-shifted weights and offsets (7.4.3.2).
 */
static void parse_pred_weight_table(Edge264_ctx *e)
{
	/*// Initialise implicit_weights and DistScaleFactor for frames.
	if (s->slice_type == 1 && !s->field_pic_flag) {
		int PicOrderCnt = min(s->TopFieldOrderCnt, s->BottomFieldOrderCnt);
		int topAbsDiffPOC = abs(e->FieldOrderCnt[s->RefPicList[1][0]] - PicOrderCnt);
		int bottomAbsDiffPOC = abs(e->FieldOrderCnt[s->RefPicList[1][1] - PicOrderCnt);
		s->firstRefPicL1 = (topAbsDiffPOC >= bottomAbsDiffPOC);
		for (int refIdxL0 = 0; refIdxL0 < s->ps.num_ref_idx_active[0]; refIdxL0++) {
			int pic0 = s->RefPicList[0][2 * refIdxL0];
			int PicOrderCnt0 = min(pic0->PicOrderCnt, pic0[1].PicOrderCnt);
			int tb = min(max(PicOrderCnt - PicOrderCnt0, -128), 127);
			int DistScaleFactor = 0;
			for (int refIdxL1 = s->ps.num_ref_idx_active[1]; refIdxL1-- > 0; ) {
				const Edge264_picture *pic1 = s->DPB + s->RefPicList[1][2 * refIdxL1];
				int PicOrderCnt1 = min(pic1->PicOrderCnt, pic1[1].PicOrderCnt);
				int td = min(max(PicOrderCnt1 - PicOrderCnt0, -128), 127);
				DistScaleFactor = 256;
				int w_1C = 32;
				if (td != 0 && !(long_term_flags & (1 << refIdxL0))) {
					int tx = (16384 + abs(td / 2)) / td;
					DistScaleFactor = min(max((tb * tx + 32) >> 6, -1024), 1023);
					if (!(long_term_flags & (1 << refIdxL1)) &&
						(DistScaleFactor >> 2) >= -64 && (DistScaleFactor >> 2) <= 128)
						w_1C = DistScaleFactor >> 2;
				}
				s->implicit_weights[2][2 * refIdxL0][2 * refIdxL1] = -w_1C;
			}
			s->DistScaleFactor[2][2 * refIdxL0] = DistScaleFactor << 5;
		}
	}
	
	// Initialise the same for fields.
	if (s->slice_type == 1 && (s->field_pic_flag || s->MbaffFrameFlag))
		for (int refIdxL0 = s->ps.num_ref_idx_active[0] << s->MbaffFrameFlag; refIdxL0-- > 0; )
	{
		const Edge264_picture *pic0 = s->DPB + s->RefPicList[0][refIdxL0];
		int tb0 = min(max(s->p.PicOrderCnt - pic0->PicOrderCnt, -128), 127);
		int tb1 = min(max(OtherFieldOrderCnt - pic0->PicOrderCnt, -128), 127);
		int DistScaleFactor0 = 0, DistScaleFactor1 = 0;
		for (int refIdxL1 = s->ps.num_ref_idx_active[1] << s->MbaffFrameFlag; refIdxL1-- > 0; ) {
			const Edge264_picture *pic1 = s->DPB + s->RefPicList[1][refIdxL1];
			int td = min(max(pic1->PicOrderCnt - pic0->PicOrderCnt, -128), 127);
			DistScaleFactor0 = DistScaleFactor1 = 256;
			int w_1C = 32, W_1C = 32;
			if (td != 0 && !(long_term_flags & (1 << (refIdxL0 / 2)))) {
				int tx = (16384 + abs(td / 2)) / td;
				DistScaleFactor0 = min(max((tb0 * tx + 32) >> 6, -1024), 1023);
				DistScaleFactor1 = min(max((tb1 * tx + 32) >> 6, -1024), 1023);
				if (!(long_term_flags & (1 << (refIdxL1 / 2)))) {
					if ((DistScaleFactor0 >> 2) >= -64 && (DistScaleFactor0 >> 2) <= 128)
						w_1C = DistScaleFactor0 >> 2;
					if ((DistScaleFactor1 >> 2) >= -64 && (DistScaleFactor1 >> 2) <= 128)
						W_1C = DistScaleFactor1 >> 2;
				}
			}
			s->implicit_weights[s->bottom_field_flag][refIdxL0][refIdxL1] = -w_1C;
			s->implicit_weights[s->bottom_field_flag ^ 1][refIdxL0][refIdxL1] = -W_1C;
		}
		s->DistScaleFactor[s->bottom_field_flag][refIdxL0] = DistScaleFactor0 << 5;
		s->DistScaleFactor[s->bottom_field_flag ^ 1][refIdxL0] = DistScaleFactor1 << 5;
	}*/
	
	// Parse explicit weights/offsets.
	if ((s->slice_type == 0 && s->ps.weighted_pred & 4) ||
		(s->slice_type == 1 && s->ps.weighted_pred & 1)) {
		unsigned luma_shift = 7 - get_ue(7);
		unsigned chroma_shift = (s->ps.ChromaArrayType != 0) ? 7 - get_ue(7) : 0;
		for (int l = 0; l <= s->slice_type; l++) {
			for (int i = 0; i < s->ps.num_ref_idx_active[l]; i++) {
				s->weights[0][i][l] = 1 << 7;
				if (get_u1()) {
					s->weights[0][i][l] = get_se(-128, 127) << luma_shift;
					s->offsets[0][i][l] = get_se(-128, 127) << (s->ps.BitDepth[0] - 8);
					printf("<li>luma_weight_l%x[%u]: <code>%.2f</code></li>\n"
						"<li>luma_offset_l%x[%u]: <code>%d</code></li>\n",
						l, i, (double)s->weights[0][i][l] / 128,
						l, i, s->offsets[0][i][l]);
				}
				s->weights[1][i][l] = s->weights[2][i][l] = 1 << 7;
				if (s->ps.ChromaArrayType != 0 && get_u1()) {
					for (int j = 1; j < 3; j++) {
						s->weights[j][i][l] = get_se(-128, 127) << chroma_shift;
						s->offsets[j][i][l] = get_se(-128, 127) << (s->ps.BitDepth[1] - 8);
						printf("<li>chroma_weight_l%x[%u][%x]: <code>%.2f</code></li>\n"
							"<li>chroma_offset_l%x[%u][%x]: <code>%d</code></li>\n",
							l, i, j - 1, (double)s->weights[j][i][l] / 128,
							l, i, j - 1,s->offsets[j][i][l]);
					}
				}
			}
		}
	}
}



/**
 * Updates the reference flags by adaptive memory control or sliding window
 * marking process (8.2.5).
 */
static void parse_dec_ref_pic_marking(Edge264_ctx *e)
{
	int memory_management_control_operation;
	int i = 32;
	if (s->IdrPicFlag) {
		int no_output_of_prior_pics_flag = get_u1();
		if (no_output_of_prior_pics_flag)
			e->output_flags = e->prevPicOrderCnt = 0;
		int long_term_reference_flag = get_u1();
		e->long_term_flags = long_term_reference_flag << e->currPic;
		if (long_term_reference_flag)
			e->FrameNum[e->currPic] = 0;
		printf("<li%s>no_output_of_prior_pics_flag: <code>%x</code></li>\n"
			"<li>long_term_reference_flag: <code>%x</code></li>\n",
			red_if(no_output_of_prior_pics_flag), no_output_of_prior_pics_flag,
			long_term_reference_flag);
	
	// 8.2.5.4 - Adaptive memory control marking process.
	} else if (get_u1())
		while ((memory_management_control_operation = get_ue16()) != 0 && i-- > 0)
	{
		if (memory_management_control_operation == 4) {
			int max_long_term_frame_idx = get_ue16() - 1;
			for (unsigned r = e->long_term_flags; r != 0; r &= r - 1) {
				int j = __builtin_ctz(r);
				if (e->FrameNum[j] > max_long_term_frame_idx)
					e->reference_flags &= ~(0x10001 << j), e->long_term_flags ^= 1 << j;
			}
			printf("<li>Above LongTermFrameIdx %u -> unused for reference</li>\n", max_long_term_frame_idx);
			continue;
		} else if (memory_management_control_operation == 5) {
			e->reference_flags = e->long_term_flags = 0;
			printf("<li>All references -> unused for reference</li>\n");
			continue;
		} else if (memory_management_control_operation == 6) {
			e->long_term_flags |= 1 << e->currPic;
			e->FrameNum[e->currPic] = get_ue16();
			printf("<li>Current picture -> LongTermFrameIdx %u</li>\n", e->FrameNum[e->currPic]);
			continue;
		}
		
		// The remaining three operations share the search for FrameNum.
		int pic_num = get_ue16();
		int bottom = ((pic_num & 1) ^ s->bottom_field_flag) << 4;
		int LongTermFrameNum = (s->field_pic_flag) ? pic_num >> 1 : pic_num;
		unsigned r = (memory_management_control_operation != 2 ?
			~e->long_term_flags : e->long_term_flags) & e->reference_flags >> bottom & 0xffff;
		int FrameNum = (memory_management_control_operation != 2) ?
			e->prevFrameNum - 1 - LongTermFrameNum : LongTermFrameNum;
		int j = e->currPic;
		while (r != 0 && e->FrameNum[j = __builtin_ctz(r)] != FrameNum)
			r &= r - 1;
		unsigned full = 0x10001 << j;
		unsigned mask = s->field_pic_flag ? 1 << (bottom + j) : full;
		if (memory_management_control_operation == 1) {
			e->reference_flags &= ~mask;
			printf("<li>FrameNum %u -> unused for reference</li>\n", FrameNum);
		} else if (memory_management_control_operation == 2) {
			e->reference_flags &= ~mask;
			if (!(e->reference_flags & full))
				e->long_term_flags &= ~full;
			printf("<li>LongTermFrameIdx %u -> unused for reference</li>\n", FrameNum);
		} else if (memory_management_control_operation == 3) {
			e->FrameNum[j] = get_ue(15);
			e->long_term_flags |= full;
			printf("<li>FrameNum %u -> LongTermFrameIdx %u</li>\n", FrameNum, e->FrameNum[j]);
		}
	}
	
	// 8.2.5.3 - Sliding window marking process
	unsigned r = (uint16_t)e->reference_flags | e->reference_flags >> 16;
	if (__builtin_popcount(r) > s->ps.max_num_ref_frames) {
		r ^= e->long_term_flags;
		int best = INT_MAX;
		int next = 0;
		do {
			int i = __builtin_ctz(r);
			if (best > e->FrameNum[i])
				best = e->FrameNum[next = i];
		} while (r &= r - 1);
		e->reference_flags &= ~(0x10001 << best);
	}
	
	e->reference_flags |= (!s->field_pic_flag ? 0x10001 : s->bottom_field_flag ? 0x1000 : 1) << e->currPic;
}



/**
 * This function is dedicated to the bumping process specified in C.4.4, which
 * outputs pictures until a free DPB slot is found.
 */
static __attribute__((noinline)) void bump_pictures(Edge264_ctx *e) {
	while (1) {
		unsigned o = e->output_flags;
		int best = INT_MAX, output = 0, num = 0;
		do {
			int i = __builtin_ctz(o);
			if (best > e->FieldOrderCnt[i])
				best = e->FieldOrderCnt[output = i];
		} while (num++, o &= o - 1);
		if (num <= s->ps.max_num_reorder_frames &&
			((uint16_t)e->reference_flags | e->reference_flags >> 16 | e->output_flags) != 0)
			break;
		e->output_flags ^= 1 << output;
		if (e->output_frame != NULL)
			e->output_frame(output);
	}
}



/**
 * This function matches slice_header() in 7.3.3, which it parses while updating
 * the DPB and initialising slice data for further decoding. Pictures are output
 * through bumping.
 *
 * Contrary to SPSs and PPSs, slice_header() has no explicit trailing_bits() to
 * detect errors with high probability and revert changes, thus the main context
 * is directly updated with no particular protection.
 */
static const uint8_t *parse_slice_layer_without_partitioning(Edge264_ctx *e,
	const uint8_t *CPB, const uint8_t *end)
{
	static const char * const slice_type_names[5] = {"P", "B", "I", "SP", "SI"};
	
	// Zero-setting should not be necessary and will be amended in the future.
	memset(s, 0, sizeof(*s));
	s->nal_ref_flag = (*CPB & 0xe0) != 0;
	s->IdrPicFlag = (*CPB & 0x1f) == 5;
	s->CPB = CPB + 3;
	s->RBSP[1] = CPB[1] << 8 | CPB[2];
	refill(SIZE_BIT * 2 - 16, 0);
	
	// We correctly input these values to better display them... in red.
	int first_mb_in_slice = get_ue(294848);
	int slice_type = get_ue(9);
	s->slice_type = (slice_type < 5) ? slice_type : slice_type - 5;
	int pic_parameter_set_id = get_ue(255);
	printf("<li%s>first_mb_in_slice: <code>%u</code></li>\n"
		"<li%s>slice_type: <code>%u (%s)</code></li>\n"
		"<li%s>pic_parameter_set_id: <code>%u</code></li>\n",
		red_if(first_mb_in_slice > 0), first_mb_in_slice,
		red_if(s->slice_type > 2), slice_type, slice_type_names[s->slice_type],
		red_if(pic_parameter_set_id >= 4 || e->PPSs[pic_parameter_set_id].num_ref_idx_active[0] == 0), pic_parameter_set_id);
	
	if (first_mb_in_slice > 0 || s->slice_type > 2 || pic_parameter_set_id >= 4 ||
		e->PPSs[pic_parameter_set_id].num_ref_idx_active[0] == 0)
		return NULL;
	s->ps = e->PPSs[pic_parameter_set_id];
	
	// Computing an absolute FrameNum simplifies further code.
	unsigned relFrameNum = get_uv(s->ps.log2_max_frame_num) - e->prevFrameNum;
	e->prevFrameNum += relFrameNum & ~(-1u << s->ps.log2_max_frame_num);
	printf("<li>frame_num: <code>%u</code></li>\n", e->prevFrameNum);
	
	// This comment is just here to segment the code, glad you read it :)
	if (!s->ps.frame_mbs_only_flag) {
		s->field_pic_flag = get_u1();
		printf("<li>field_pic_flag: <code>%x</code></li>\n", s->field_pic_flag);
		if (s->field_pic_flag) {
			s->bottom_field_flag = get_u1();
			printf("<li>bottom_field_flag: <code>%x</code></li>\n",
				s->bottom_field_flag);
		}
	}
	s->MbaffFrameFlag = s->ps.mb_adaptive_frame_field_flag & ~s->field_pic_flag;
	
	// I did not get the point of idr_pic_id yet.
	if (s->IdrPicFlag) {
		e->reference_flags = e->long_term_flags = e->prevFrameNum = 0;
		int idr_pic_id = get_ue(65535);
		printf("<li>idr_pic_id: <code>%u</code></li>\n", idr_pic_id);
	}
	
	// Compute Top/BottomFieldOrderCnt (8.2.1).
	s->TopFieldOrderCnt = s->BottomFieldOrderCnt = e->prevFrameNum * 2 - (s->nal_ref_flag == 0);
	if (s->ps.pic_order_cnt_type == 0) {
		unsigned shift = WORD_BIT - s->ps.log2_max_pic_order_cnt_lsb;
		int diff = get_uv(s->ps.log2_max_pic_order_cnt_lsb) - e->prevPicOrderCnt;
		unsigned PicOrderCnt = e->prevPicOrderCnt + (diff << shift >> shift);
		s->TopFieldOrderCnt = PicOrderCnt;
		s->BottomFieldOrderCnt = (!s->field_pic_flag && s->ps.bottom_field_pic_order_in_frame_present_flag) ?
			PicOrderCnt + map_se(get_ue32()) : PicOrderCnt;
		if (s->nal_ref_flag)
			e->prevPicOrderCnt = PicOrderCnt;
	} else if (s->ps.pic_order_cnt_type == 1) {
		unsigned absFrameNum = e->prevFrameNum - (s->nal_ref_flag == 0);
		unsigned expectedPicOrderCnt = (s->nal_ref_flag == 0) ? s->ps.offset_for_non_ref_pic : 0;
		if (s->ps.num_ref_frames_in_pic_order_cnt_cycle != 0) {
			expectedPicOrderCnt += (absFrameNum / s->ps.num_ref_frames_in_pic_order_cnt_cycle) *
				e->PicOrderCntDeltas[s->ps.num_ref_frames_in_pic_order_cnt_cycle] +
				e->PicOrderCntDeltas[absFrameNum % s->ps.num_ref_frames_in_pic_order_cnt_cycle];
		}
		s->TopFieldOrderCnt = s->BottomFieldOrderCnt = expectedPicOrderCnt;
		if (!s->ps.delta_pic_order_always_zero_flag) {
			s->TopFieldOrderCnt = expectedPicOrderCnt += map_se(get_ue32());
			s->BottomFieldOrderCnt = (!s->field_pic_flag && s->ps.bottom_field_pic_order_in_frame_present_flag) ?
				expectedPicOrderCnt + map_se(get_ue32()) : expectedPicOrderCnt;
		}
	}
	printf("<li>pic_order_cnt: <code>%u</code></li>\n", min(s->TopFieldOrderCnt, s->BottomFieldOrderCnt));
	
	// That could be optimised into fast bit tests, but no compiler knows it :)
	if (s->slice_type == 0 || s->slice_type == 1) {
		if (s->slice_type == 1) {
			s->direct_spatial_mv_pred_flag = get_u1();
			printf("<li>direct_spatial_mv_pred_flag: <code>%x</code></li>\n",
				s->direct_spatial_mv_pred_flag);
		}
		
		// Use the last decoded picture for reference when at least one is missing.
		uint16_t top = e->reference_flags, bot = e->reference_flags >> 16;
		unsigned refs = (s->field_pic_flag) ? top | bot : top & bot;
		if (refs == 0)
			e->reference_flags |= 0x10001 << e->currPic;
		
		// num_ref_idx_active_override_flag
		if (get_u1()) {
			for (int l = 0; l <= s->slice_type; l++) {
				s->ps.num_ref_idx_active[l] = get_ue(31) + 1;
				printf("<li>num_ref_idx_l%x_active: <code>%u</code></li>\n",
					l, s->ps.num_ref_idx_active[l]);
			}
		}
		s->ref_idx_mask = (s->ps.num_ref_idx_active[0] > 1 ? 0x1111 : 0) |
			(s->ps.num_ref_idx_active[1] > 1 ? 0x11110000 : 0);
		parse_ref_pic_list_modification(e);
		parse_pred_weight_table(e);
	}
	
	// Without slices we can assume previous picture was complete, so don't need bumping.
	if (e->remaining_mbs == 0) {
		unsigned avail = ~((uint16_t)e->reference_flags | e->reference_flags >> 16 | e->output_flags);
		e->currPic = __builtin_ctz(avail | 1 << 15);
		e->output_flags |= 1 << e->currPic;
		e->remaining_mbs = s->ps.width * s->ps.height >> 8;
		e->FrameNum[e->currPic] = e->prevFrameNum;
	}
	e->FieldOrderCnt[(s->bottom_field_flag) ? e->currPic + 16 : e->currPic] = s->TopFieldOrderCnt;
	if (!s->field_pic_flag)
		e->FieldOrderCnt[16 + e->currPic] = s->BottomFieldOrderCnt;
	
	// not much to say in this comment either (though there is intention!)
	if (s->nal_ref_flag)
		parse_dec_ref_pic_marking(e);
	if (s->ps.entropy_coding_mode_flag && s->slice_type != 2) {
		s->cabac_init_idc = 1 + get_ue(2);
		printf("<li>cabac_init_idc: <code>%u</code></li>\n", s->cabac_init_idc - 1);
	}
	s->ps.QP_Y = min(max(s->ps.QP_Y + map_se(get_ue16()), -6 * ((int)s->ps.BitDepth[0] - 8)), 51);
	printf("<li>SliceQP<sub>Y</sub>: <code>%d</code></li>\n", s->ps.QP_Y);
	
	// Loop filter is not enabled, though not yet implemented.
	if (s->ps.deblocking_filter_control_present_flag) {
		s->disable_deblocking_filter_idc = get_ue(2);
		if (s->disable_deblocking_filter_idc != 1) {
			s->FilterOffsetA = get_se(-6, 6) * 2;
			s->FilterOffsetB = get_se(-6, 6) * 2;
			printf("<li>FilterOffsetA: <code>%d</code></li>\n"
				"<li>FilterOffsetB: <code>%d</code></li>\n",
				s->FilterOffsetA,
				s->FilterOffsetB);
		}
	}
	
	if (s->ps.entropy_coding_mode_flag)
		e->remaining_mbs -= CABAC_parse_slice_data();
	
	// without slices, we always get a complete picture, so can bump almost every time
	if (e->remaining_mbs == 0)
		bump_pictures(e);
	
	// CPB pointer might have gone far if 000003000003000003... bytes follow,
	// so we backtrack to a point that is 100% behind next start code.
	return CPB - 22;
}



/** Used for end_of_seq too, deallocates the picture buffer, then resets e. */
static const uint8_t *parse_end_of_stream(Edge264_ctx *e, const uint8_t *CPB, const uint8_t *end) {
	if (e->DPB != NULL)
		free(e->DPB);
	memset(e, 0, sizeof(*e));
	return CPB;
}



/** Access Unit Delimiters are ignored to avoid depending on their occurence. */
static const uint8_t *parse_access_unit_delimiter(Edge264_ctx *e, const uint8_t *CPB, const uint8_t *end) {
	static const char * const primary_pic_type_names[8] = {"I", "P, I",
		"P, B, I", "SI", "SP, SI", "I, SI", "P, I, SP, SI", "P, B, I, SP, SI"};
	int primary_pic_type = CPB[1] >> 5;
	printf("<li>primary_pic_type: <code>%u (%s)</code></li>\n",
		primary_pic_type, primary_pic_type_names[primary_pic_type]);
	return CPB; // Some streams omit the rbsp_trailing_bits, but that's fine.
}



/**
 * Parses the scaling lists into p->weightScaleNxN (7.3.2.1 and Table 7-2).
 *
 * Fall-back rules for indices 0, 3, 6 and 7 are applied by keeping the
 * existing list, so they must be initialised with Default scaling lists at
 * the very first call.
 */
static void parse_scaling_lists()
{
	static const uint8_t scan_4x4[16] =
		{0,  4,  1,  2,  5,  8, 12,  9,  6,  3,  7, 10, 13, 14, 11, 15};
	static const uint8_t scan_8x8[64] =
		{0,  8,  1,  2,  9, 16, 24, 17, 10,  3,  4, 11, 18, 25, 32, 40,
		33, 26, 19, 12,  5,  6, 13, 20, 27, 34, 41, 48, 56, 49, 42, 35,
		28, 21, 14,  7, 15, 22, 29, 36, 43, 50, 57, 58, 51, 44, 37, 30,
		23, 31, 38, 45, 52, 59, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63};
	
	// The 4x4 scaling lists are small enough to fit a vector register.
	v16qu d4x4 = Default_4x4_Intra;
	v16qu *w4x4 = (v16qu *)s->ps.weightScale4x4;
	do {
		v16qu v4x4 = *w4x4;
		const char *str = "unchanged";
		do {
			printf("<li>weightScale4x4[%tu]: <code>", (uint8_t(*)[16])w4x4 - s->ps.weightScale4x4);
			*w4x4 = v4x4;
			uint8_t nextScale;
			if (!get_u1() || !(*w4x4 = d4x4, str = "default", nextScale = 8 + get_se(-128, 127))) {
				printf(str, (uint8_t(*)[16])w4x4 - s->ps.weightScale4x4 - 1);
			} else {
				uint8_t lastScale = nextScale;
				int j = 0;
				while (((uint8_t *)w4x4)[scan_4x4[j]] = lastScale, printf(" %u", lastScale), ++j < 16) {
					if (nextScale != 0)
						lastScale = nextScale, nextScale += get_se(-128, 127);
				}
			}
			printf("</code></li>\n");
			str = "weightScale4x4[%tu]";
			v4x4 = *w4x4++;
		} while (w4x4 != (v16qu *)s->ps.weightScale4x4[3] && w4x4 != (v16qu *)s->ps.weightScale4x4[6]);
		d4x4 = Default_4x4_Inter;
	} while (w4x4 != (v16qu *)s->ps.weightScale4x4[6]);
	
	// For 8x8 scaling lists, we only pass pointers around.
	if (!s->ps.transform_8x8_mode_flag)
		return;
	v16qu *w8x8 = (v16qu *)s->ps.weightScale8x8;
	const v16qu *v8x8 = w8x8;
	do {
		const v16qu *d8x8 = Default_8x8_Intra;
		do {
			printf("<li>weightScale8x8[%tu]: <code>", (uint8_t(*)[64])w8x8 - s->ps.weightScale8x8);
			const char *str = ((uint8_t *)w8x8 < s->ps.weightScale8x8[2]) ? "existing" : "weightScale8x8[%tu]";
			const v16qu *src = v8x8;
			uint8_t nextScale;
			if (!get_u1() || (src = d8x8, str = "default", nextScale = 8 + get_se(-128, 127))) {
				w8x8[0] = src[0];
				w8x8[1] = src[1];
				w8x8[2] = src[2];
				w8x8[3] = src[3];
				printf(str, (uint8_t(*)[64])src - s->ps.weightScale8x8);
			} else {
				uint8_t lastScale = nextScale;
				int j = 0;
				while (((uint8_t *)w8x8)[scan_8x8[j]] = lastScale, printf(" %u", lastScale), ++j < 64) {
					if (nextScale != 0)
						lastScale = nextScale, nextScale += get_se(-128, 127);
				}
			}
			printf("</code></li>\n");
			d8x8 = Default_8x8_Inter;
			w8x8 += 4;
		} while (((uint8_t *)w8x8 - s->ps.weightScale8x8[0]) & 64);
		v8x8 = w8x8 - 8;
	} while (s->ps.chroma_format_idc == 3 && w8x8 < (v16qu *)s->ps.weightScale8x8[6]);
}



/**
 * Parses the PPS into a copy of the current SPS, then saves it into one of four
 * PPS slots if a rbsp_trailing_bits pattern follows.
 *
 * Slice groups are not supported because:
 * _ The sixth group requires a per-PPS storage of mapUnitToSliceGroupMap, with
 *   an upper size of 543^2 bytes, though a slice group needs 3 bits at most;
 * _ Groups 3-5 ignore the PPS's mapUnitToSliceGroupMap, and use 1 bit per mb;
 * _ Skipping unavailable mbs while decoding a slice messes with the storage of
 *   neighbouring macroblocks as a cirbular buffer.
 */
static const uint8_t *parse_pic_parameter_set(Edge264_ctx *e,
	const uint8_t *CPB, const uint8_t *end)
{
	static const char * const slice_group_map_type_names[7] = {"interleaved",
		"dispersed", "foreground with left-over", "box-out", "raster scan",
		"wipe", "explicit"};
	
	// initialise the parsing context
	s->CPB = CPB + 3;
	s->RBSP[1] = CPB[1] << 8 | CPB[2];
	refill(SIZE_BIT * 2 - 16, 0);
	
	// Actual streams never use more than 4 PPSs (I, P, B, b).
	int pic_parameter_set_id = get_ue(255);
	int seq_parameter_set_id = get_ue(31);
	s->ps.entropy_coding_mode_flag = get_u1();
	s->ps.bottom_field_pic_order_in_frame_present_flag = get_u1();
	int num_slice_groups = get_ue(7) + 1;
	printf("<li%s>pic_parameter_set_id: <code>%u</code></li>\n"
		"<li%s>seq_parameter_set_id: <code>%u</code></li>\n"
		"<li%s>entropy_coding_mode_flag: <code>%x</code></li>\n"
		"<li>bottom_field_pic_order_in_frame_present_flag: <code>%x</code></li>\n"
		"<li%s>num_slice_groups: <code>%u</code></li>\n",
		red_if(pic_parameter_set_id >= 4), pic_parameter_set_id,
		red_if(seq_parameter_set_id != 0), seq_parameter_set_id,
		red_if(!s->ps.entropy_coding_mode_flag), s->ps.entropy_coding_mode_flag,
		s->ps.bottom_field_pic_order_in_frame_present_flag,
		red_if(num_slice_groups > 1), num_slice_groups);
	
	// Let's be nice enough to print the headers for unsupported stuff.
	if (num_slice_groups > 1) {
		int slice_group_map_type = get_ue(6);
		printf("<li>slice_group_map_type: <code>%u (%s)</code></li>\n",
			slice_group_map_type, slice_group_map_type_names[slice_group_map_type]);
		switch (slice_group_map_type) {
		case 0:
			for (int iGroup = 0; iGroup < num_slice_groups; iGroup++) {
				int run_length = get_ue32() + 1;
				printf("<li>run_length[%u]: <code>%u</code></li>\n",
					iGroup, run_length);
			}
			break;
		case 2:
			for (int iGroup = 0; iGroup < num_slice_groups; iGroup++) {
				int top_left = get_ue32();
				int bottom_right = get_ue32();
				printf("<li>top_left[%u]: <code>%u</code></li>\n"
					"<li>bottom_right[%u]: <code>%u</code></li>\n",
					iGroup, top_left,
					iGroup, bottom_right);
			}
			break;
		case 3 ... 5: {
			int slice_group_change_direction_flag = get_u1();
			int SliceGroupChangeRate = get_ue32() + 1;
			printf("<li>slice_group_change_direction_flag: <code>%x</code></li>\n"
				"<li>SliceGroupChangeRate: <code>%u</code></li>\n",
				slice_group_change_direction_flag,
				SliceGroupChangeRate);
			} break;
		case 6: {
			get_ue32(); // pic_size_in_map_units
			int PicSizeInMapUnits = s->ps.width * s->ps.height << s->ps.frame_mbs_only_flag >> 9;
			printf("<li>slice_group_ids: <code>");
			for (int i = 0; i < PicSizeInMapUnits; i++) {
				int slice_group_id = get_uv(WORD_BIT - __builtin_clz(num_slice_groups - 1));
				printf("%u ", slice_group_id);
			}
			printf("</code></li>\n");
			} break;
		}
	}
	
	// (num_ref_idx_active[0] != 0) is used as indicator that the PPS is initialised.
	s->ps.num_ref_idx_active[0] = get_ue(31) + 1;
	s->ps.num_ref_idx_active[1] = get_ue(31) + 1;
	s->ps.weighted_pred = get_uv(3);
	s->ps.QP_Y = get_se(-62, 25) + 26;
	int pic_init_qs = get_se(-26, 25) + 26;
	s->ps.second_chroma_qp_index_offset = s->ps.chroma_qp_index_offset = get_se(-12, 12);
	s->ps.deblocking_filter_control_present_flag = get_u1();
	s->ps.constrained_intra_pred_flag = get_u1();
	int redundant_pic_cnt_present_flag = get_u1();
	printf("<li>num_ref_idx_l0_default_active: <code>%u</code></li>\n"
		"<li>num_ref_idx_l1_default_active: <code>%u</code></li>\n"
		"<li>weighted_pred_flag: <code>%x</code></li>\n"
		"<li>weighted_bipred_idc: <code>%u</code></li>\n"
		"<li>pic_init_qp: <code>%u</code></li>\n"
		"<li>pic_init_qs: <code>%u</code></li>\n"
		"<li>chroma_qp_index_offset: <code>%d</code></li>\n"
		"<li>deblocking_filter_control_present_flag: <code>%x</code></li>\n"
		"<li>constrained_intra_pred_flag: <code>%x</code></li>\n"
		"<li%s>redundant_pic_cnt_present_flag: <code>%x</code></li>\n",
		s->ps.num_ref_idx_active[0],
		s->ps.num_ref_idx_active[1],
		s->ps.weighted_pred >> 2,
		s->ps.weighted_pred & 0x3,
		s->ps.QP_Y,
		pic_init_qs,
		s->ps.chroma_qp_index_offset,
		s->ps.deblocking_filter_control_present_flag,
		s->ps.constrained_intra_pred_flag,
		red_if(redundant_pic_cnt_present_flag), redundant_pic_cnt_present_flag);
	s->ps.transform_8x8_mode_flag = 0;
	
	// short for peek-24-bits-without-having-to-define-a-one-use-function
	if (lsd(s->RBSP[0], s->RBSP[1], s->shift) >> (SIZE_BIT - 24) != 0x800000) {
		s->ps.transform_8x8_mode_flag = get_u1();
		printf("<li>transform_8x8_mode_flag: <code>%x</code></li>\n",
			s->ps.transform_8x8_mode_flag);
		if (get_u1())
			parse_scaling_lists();
		s->ps.second_chroma_qp_index_offset = get_se(-12, 12);
		printf("<li>second_chroma_qp_index_offset: <code>%d</code></li>\n",
			s->ps.second_chroma_qp_index_offset);
	}
	
	// seq_parameter_set_id was ignored so far as long as no SPS data was read.
	if (get_uv(24) != 0x800000 || pic_parameter_set_id >= 4 || seq_parameter_set_id > 0 || e->DPB == NULL)
		return NULL;
	
	// If this PPS is acceptable but unsupported, invalidate it!
	e->PPSs[pic_parameter_set_id] = *(!redundant_pic_cnt_present_flag &&
		num_slice_groups == 1 && s->ps.entropy_coding_mode_flag ? &s->ps :
		&(Edge264_parameter_set){});
	
	// CPB pointer might have gone far if following NAL is xx000003000003000003...
	// so we backtrack to a point that is 100% behind next start code.
	return s->CPB - 26;
}



/**
 * For the sake of implementation simplicity, the responsibility for timing
 * management is left to the parent library, hence any HRD data is ignored.
 */
static void parse_hrd_parameters() {
	int cpb_cnt = get_ue(31) + 1;
	int bit_rate_scale = get_uv(4);
	int cpb_size_scale = get_uv(4);
	printf("<li>cpb_cnt: <code>%u</code></li>\n"
		"<li>bit_rate_scale: <code>%u</code></li>\n"
		"<li>cpb_size_scale: <code>%u</code></li>\n",
		cpb_cnt,
		bit_rate_scale,
		cpb_size_scale);
	for (int i = 0; i < cpb_cnt; i++) {
		unsigned bit_rate_value = get_ue(4294967294) + 1;
		unsigned cpb_size_value = get_ue(4294967294) + 1;
		int cbr_flag = get_u1();
		printf("<ul>\n"
			"<li>bit_rate_value[%u]: <code>%u</code></li>\n"
			"<li>cpb_size_value[%u]: <code>%u</code></li>\n"
			"<li>cbr_flag[%u]: <code>%x</code></li>\n"
			"</ul>\n",
			i, bit_rate_value,
			i, cpb_size_value,
			i, cbr_flag);
	}
	unsigned delays = get_uv(20);
	int initial_cpb_removal_delay_length = (delays >> 15) + 1;
	int cpb_removal_delay_length = ((delays >> 10) & 0x1f) + 1;
	int dpb_output_delay_length = ((delays >> 5) & 0x1f) + 1;
	int time_offset_length = delays & 0x1f;
	printf("<li>initial_cpb_removal_delay_length: <code>%u</code></li>\n"
		"<li>cpb_removal_delay_length: <code>%u</code></li>\n"
		"<li>dpb_output_delay_length: <code>%u</code></li>\n"
		"<li>time_offset_length: <code>%u</code></li>\n",
		initial_cpb_removal_delay_length,
		cpb_removal_delay_length,
		dpb_output_delay_length,
		time_offset_length);
}



/**
 * To avoid cluttering the memory layout with unused data, VUI parameters are
 * mostly ignored until explicitly asked in the future.
 */
static void parse_vui_parameters()
{
	static const unsigned ratio2sar[256] = {0, 0x00010001, 0x000c000b,
		0x000a000b, 0x0010000b, 0x00280021, 0x0018000b, 0x0014000b, 0x0020000b,
		0x00500021, 0x0012000b, 0x000f000b, 0x00400021, 0x00a00063, 0x00040003,
		0x00030002, 0x00020001};
	static const char * const video_format_names[8] = {"Component", "PAL",
		"NTSC", "SECAM", "MAC", [5 ... 7] = "Unspecified"};
	static const char * const colour_primaries_names[256] = {
		[0] = "unknown",
		[1] = "green(0.300,0.600) blue(0.150,0.060) red(0.640,0.330) whiteD65(0.3127,0.3290)",
		[2 ... 3] = "unknown",
		[4] = "green(0.21,0.71) blue(0.14,0.08) red(0.67,0.33) whiteC(0.310,0.316)",
		[5] = "green(0.29,0.60) blue(0.15,0.06) red(0.64,0.33) whiteD65(0.3127,0.3290)",
		[6 ... 7] = "green(0.310,0.595) blue(0.155,0.070) red(0.630,0.340) whiteD65(0.3127,0.3290)",
		[8] = "green(0.243,0.692) blue(0.145,0.049) red(0.681,0.319) whiteC(0.310,0.316)",
		[9] = "green(0.170,0.797) blue(0.131,0.046) red(0.708,0.292) whiteD65(0.3127,0.3290)",
		[10 ... 255] = "unknown",
	};
	static const char * const transfer_characteristics_names[256] = {
		[0] = "unknown",
		[1] = "V=1.099*Lc^0.45-0.099 for Lc in [0.018,1], V=4.500*Lc for Lc in [0,0.018[",
		[2 ... 3] = "unknown",
		[4] = "Assumed display gamma 2.2",
		[5] = "Assumed display gamma 2.8",
		[6] = "V=1.099*Lc^0.45-0.099 for Lc in [0.018,1], V=4.500*Lc for Lc in [0,0.018[",
		[7] = "V=1.1115*Lc^0.45-0.1115 for Lc in [0.0228,1], V=4.0*Lc for Lc in [0,0.0228[",
		[8] = "V=Lc for Lc in [0,1[",
		[9] = "V=1.0+Log10(Lc)/2 for Lc in [0.01,1], V=0.0 for Lc in [0,0.01[",
		[10] = "V=1.0+Log10(Lc)/2.5 for Lc in [Sqrt(10)/1000,1], V=0.0 for Lc in [0,Sqrt(10)/1000[",
		[11] = "V=1.099*Lc^0.45-0.099 for Lc>=0.018, V=4.500*Lc for Lc in ]-0.018,0.018[, V=-1.099*(-Lc)^0.45+0.099 for Lc<=-0.018",
		[12] = "V=1.099*Lc^0.45-0.099 for Lc in [0.018,1.33[, V=4.500*Lc for Lc in [-0.0045,0.018[, V=-(1.099*(-4*Lc)^0.45-0.099)/4 for Lc in [-0.25,-0.0045[",
		[13] = "V=1.055*Lc^(1/2.4)-0.055 for Lc in [0.0031308,1[, V=12.92*Lc for Lc in [0,0.0031308[",
		[14] = "V=1.099*Lc^0.45-0.099 for Lc in [0.018,1], V=4.500*Lc for Lc in [0,0.018[",
		[15] = "V=1.0993*Lc^0.45-0.0993 for Lc in [0.0181,1], V=4.500*Lc for Lc in [0,0.0181[",
		[16 ... 255] = "unknown",
	};
	static const char * const matrix_coefficients_names[256] = {
		[0] = "unknown",
		[1] = "Kr = 0.2126; Kb = 0.0722",
		[2 ... 3] = "unknown",
		[4] = "Kr = 0.30; Kb = 0.11",
		[5 ... 6] = "Kr = 0.299; Kb = 0.114",
		[7] = "Kr = 0.212; Kb = 0.087",
		[8] = "YCgCo",
		[9] = "Kr = 0.2627; Kb = 0.0593 (non-constant luminance)",
		[10] = "Kr = 0.2627; Kb = 0.0593 (constant luminance)",
		[11 ... 255] = "unknown",
	};
	
	if (get_u1()) {
		int aspect_ratio_idc = get_uv(8);
		unsigned sar = (aspect_ratio_idc == 255) ? get_uv(32) : ratio2sar[aspect_ratio_idc];
		int sar_width = sar >> 16;
		int sar_height = sar & 0xffff;
		printf("<li>aspect_ratio: <code>%u:%u</code></li>\n",
			sar_width, sar_height);
	}
	if (get_u1()) {
		int overscan_appropriate_flag = get_u1();
		printf("<li>overscan_appropriate_flag: <code>%x</code></li>\n",
			overscan_appropriate_flag);
	}
	if (get_u1()) {
		int video_format = get_uv(3);
		int video_full_range_flag = get_u1();
		printf("<li>video_format: <code>%u (%s)</code></li>\n"
			"<li>video_full_range_flag: <code>%x</code></li>\n",
			video_format, video_format_names[video_format],
			video_full_range_flag);
		if (get_u1()) {
			unsigned desc = get_uv(24);
			int colour_primaries = desc >> 16;
			int transfer_characteristics = (desc >> 8) & 0xff;
			int matrix_coefficients = desc & 0xff;
			printf("<li>colour_primaries: <code>%u (%s)</code></li>\n"
				"<li>transfer_characteristics: <code>%u (%s)</code></li>\n"
				"<li>matrix_coefficients: <code>%u (%s)</code></li>\n",
				colour_primaries, colour_primaries_names[colour_primaries],
				transfer_characteristics, transfer_characteristics_names[transfer_characteristics],
				matrix_coefficients, matrix_coefficients_names[matrix_coefficients]);
		}
	}
	if (get_u1()) {
		int chroma_sample_loc_type_top_field = get_ue(5);
		int chroma_sample_loc_type_bottom_field = get_ue(5);
		printf("<li>chroma_sample_loc_type_top_field: <code>%x</code></li>\n"
			"<li>chroma_sample_loc_type_bottom_field: <code>%x</code></li>\n",
			chroma_sample_loc_type_top_field,
			chroma_sample_loc_type_bottom_field);
	}
	if (get_u1()) {
		unsigned num_units_in_tick = get_uv(32);
		unsigned time_scale = get_uv(32);
		int fixed_frame_rate_flag = get_u1();
		printf("<li>num_units_in_tick: <code>%u</code></li>\n"
			"<li>time_scale: <code>%u</code></li>\n"
			"<li>fixed_frame_rate_flag: <code>%x</code></li>\n",
			num_units_in_tick,
			time_scale,
			fixed_frame_rate_flag);
	}
	int nal_hrd_parameters_present_flag = get_u1();
	if (nal_hrd_parameters_present_flag)
		parse_hrd_parameters();
	int vcl_hrd_parameters_present_flag = get_u1();
	if (vcl_hrd_parameters_present_flag)
		parse_hrd_parameters();
	if (nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag) {
		int low_delay_hrd_flag = get_u1();
		printf("<li>low_delay_hrd_flag: <code>%x</code></li>\n",
			low_delay_hrd_flag);
	}
	int pic_struct_present_flag = get_u1();
	printf("<li>pic_struct_present_flag: <code>%x</code></li>\n",
		pic_struct_present_flag);
	if (get_u1()) {
		int motion_vectors_over_pic_boundaries_flag = get_u1();
		int max_bytes_per_pic_denom = get_ue(16);
		int max_bits_per_mb_denom = get_ue(16);
		int log2_max_mv_length_horizontal = get_ue(16);
		int log2_max_mv_length_vertical = get_ue(16);
		s->ps.max_num_reorder_frames = min(get_ue16(), s->ps.max_num_ref_frames);
		int max_dec_frame_buffering = get_ue(16);
		printf("<li>motion_vectors_over_pic_boundaries_flag: <code>%x</code></li>\n"
			"<li>max_bytes_per_pic_denom: <code>%u</code></li>\n"
			"<li>max_bits_per_mb_denom: <code>%u</code></li>\n"
			"<li>max_mv_length_horizontal: <code>%u</code></li>\n"
			"<li>max_mv_length_vertical: <code>%u</code></li>\n"
			"<li>max_num_reorder_frames: <code>%u</code></li>\n"
			"<li>max_dec_frame_buffering: <code>%u</code></li>\n",
			motion_vectors_over_pic_boundaries_flag,
			max_bytes_per_pic_denom,
			max_bits_per_mb_denom,
			1 << log2_max_mv_length_horizontal,
			1 << log2_max_mv_length_vertical,
			s->ps.max_num_reorder_frames,
			max_dec_frame_buffering);
	}
}



/**
 * Parses the SPS into a Edge264_parameter_set structure, then saves it if a
 * rbsp_trailing_bits pattern follows.
 */
static const uint8_t *parse_seq_parameter_set(Edge264_ctx *e,
	const uint8_t *CPB, const uint8_t *end)
{
	static const char * const profile_idc_names[256] = {
		[44] = "CAVLC 4:4:4 Intra",
		[66] = "Baseline",
		[77] = "Main",
		[83] = "Scalable Baseline",
		[86] = "Scalable High",
		[88] = "Extended",
		[100] = "High",
		[110] = "High 10",
		[118] = "Multiview High",
		[122] = "High 4:2:2",
		[128] = "Stereo High",
		[138] = "Multiview Depth High",
		[244] = "High 4:4:4 Predictive",
	};
	static const char * const chroma_format_idc_names[4] = {"4:0:0", "4:2:0", "4:2:2", "4:4:4"};
	
	// initialise the parsing context and parameter set
	s->CPB = CPB + 6;
	s->end = end;
	s->RBSP[1] = CPB[4] << 8 | CPB[5];
	refill(SIZE_BIT * 2 - 16, 0);
	s->ps = (Edge264_parameter_set){}; // should be optional
	
	// Without error codes, unsupported profiles are silently ignored.
	int profile_idc = CPB[1];
	unsigned constraint_set_flags = CPB[2];
	int level_idc = CPB[3];
	int seq_parameter_set_id = get_ue(31);
	printf("<li>profile_idc: <code>%u (%s)</code></li>\n"
		"<li>constraint_set0_flag: <code>%x</code></li>\n"
		"<li>constraint_set1_flag: <code>%x</code></li>\n"
		"<li>constraint_set2_flag: <code>%x</code></li>\n"
		"<li>constraint_set3_flag: <code>%x</code></li>\n"
		"<li>constraint_set4_flag: <code>%x</code></li>\n"
		"<li>constraint_set5_flag: <code>%x</code></li>\n"
		"<li>level_idc: <code>%f</code></li>\n"
		"<li%s>seq_parameter_set_id: <code>%u</code></li>\n",
		profile_idc, profile_idc_names[profile_idc],
		constraint_set_flags >> 7,
		(constraint_set_flags >> 6) & 1,
		(constraint_set_flags >> 5) & 1,
		(constraint_set_flags >> 4) & 1,
		(constraint_set_flags >> 3) & 1,
		(constraint_set_flags >> 2) & 1,
		(double)level_idc / 10,
		red_if(seq_parameter_set_id != 0), seq_parameter_set_id);
	
	// At this level in code, assigning bitfields is preferred over ORing them.
	s->ps.chroma_format_idc = s->ps.ChromaArrayType = 1;
	s->ps.BitDepth[0] = s->ps.BitDepth[1] = s->ps.BitDepth[2] = 8;
	int seq_scaling_matrix_present_flag = 0;
	if (profile_idc != 66 && profile_idc != 77 && profile_idc != 88) {
		s->ps.ChromaArrayType = s->ps.chroma_format_idc = get_ue(3);
		printf("<li>chroma_format_idc: <code>%u (%s)</code></li>\n",
			s->ps.chroma_format_idc, chroma_format_idc_names[s->ps.chroma_format_idc]);
		
		// Separate colour planes will be supported with slices, so code should need minimal changes
		if (s->ps.chroma_format_idc == 3) {
			s->ps.separate_colour_plane_flag = get_u1();
			s->ps.ChromaArrayType &= s->ps.separate_colour_plane_flag - 1;
			printf("<li%s>separate_colour_plane_flag: <code>%x</code></li>\n",
				red_if(s->ps.separate_colour_plane_flag), s->ps.separate_colour_plane_flag);
		}
		
		// Separate bit sizes are not hard to implement, thus supported.
		s->ps.BitDepth[0] = 8 + get_ue(6);
		s->ps.BitDepth[1] = s->ps.BitDepth[2] = 8 + get_ue(6);
		s->ps.qpprime_y_zero_transform_bypass_flag = get_u1();
		seq_scaling_matrix_present_flag = get_u1();
		printf("<li>BitDepth<sub>Y</sub>: <code>%u</code></li>\n"
			"<li>BitDepth<sub>C</sub>: <code>%u</code></li>\n"
			"<li>qpprime_y_zero_transform_bypass_flag: <code>%x</code></li>\n"
			"<li>seq_scaling_matrix_present_flag: <code>%x</code></li>\n",
			s->ps.BitDepth[0],
			s->ps.BitDepth[1],
			s->ps.qpprime_y_zero_transform_bypass_flag,
			seq_scaling_matrix_present_flag);
	}
	
	// first occurence of useful vector code
	v16qu *w = (v16qu *)s->ps.weightScale4x4;
	if (!seq_scaling_matrix_present_flag) {
		v16qu Flat_16 = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16};
		for (int i = 0; i < 30; i++)
			w[i] = Flat_16;
	} else {
		w[0] = Default_4x4_Intra;
		w[3] = Default_4x4_Inter;
		for (int i = 0; i < 4; i++) {
			w[6 + i] = Default_8x8_Intra[i];
			w[10 + i] = Default_8x8_Inter[i];
		}
		parse_scaling_lists();
	}
	
	// I like to decorate every block with a comment.
	s->ps.log2_max_frame_num = get_ue(12) + 4;
	s->ps.pic_order_cnt_type = get_ue(2);
	printf("<li>log2_max_frame_num: <code>%u</code></li>\n"
		"<li>pic_order_cnt_type: <code>%u</code></li>\n",
		s->ps.log2_max_frame_num,
		s->ps.pic_order_cnt_type);
	
	// This one will make excep... err
	int16_t PicOrderCntDeltas[256];
	s->ps.log2_max_pic_order_cnt_lsb = 16;
	if (s->ps.pic_order_cnt_type == 0) {
		s->ps.log2_max_pic_order_cnt_lsb = get_ue(12) + 4;
		printf("<li>log2_max_pic_order_cnt_lsb: <code>%u</code></li>\n",
			s->ps.log2_max_pic_order_cnt_lsb);
	
	// clearly one of the spec's useless bits (and a waste of time to implement)
	} else if (s->ps.pic_order_cnt_type == 1) {
		s->ps.delta_pic_order_always_zero_flag = get_u1();
		s->ps.offset_for_non_ref_pic = map_se(get_ue32());
		s->ps.offset_for_top_to_bottom_field = map_se(get_ue32());
		s->ps.num_ref_frames_in_pic_order_cnt_cycle = get_ue(255);
		printf("<li>delta_pic_order_always_zero_flag: <code>%x</code></li>\n"
			"<li>offset_for_non_ref_pic: <code>%d</code></li>\n"
			"<li>offset_for_top_to_bottom: <code>%d</code></li>\n"
			"<li>num_ref_frames_in_pic_order_cnt_cycle: <code>%u</code></li>\n"
			"<ul>\n",
			s->ps.delta_pic_order_always_zero_flag,
			s->ps.offset_for_non_ref_pic,
			s->ps.offset_for_top_to_bottom_field,
			s->ps.num_ref_frames_in_pic_order_cnt_cycle);
		PicOrderCntDeltas[0] = 0;
		for (int i = 1, delta = 0; i <= s->ps.num_ref_frames_in_pic_order_cnt_cycle; i++) {
			int offset_for_ref_frame = map_se(get_ue32());
			PicOrderCntDeltas[i] = delta += offset_for_ref_frame;
			printf("<li>PicOrderCntDeltas[%u]: <code>%d</code></li>\n",
				i, PicOrderCntDeltas[i]);
		}
		printf("</ul>\n");
	}
	
	// For compatibility with CoreAVC's 8100x8100, the MaxFS limit is not enforced.
	s->ps.max_num_ref_frames = s->ps.max_num_reorder_frames = get_ue(16);
	int gaps_in_frame_num_value_allowed_flag = get_u1();
	s->ps.width = (get_ue(543) + 1) << 4;
	// An offset might be added if 2048-wide videos actually suffer from cache alignment.
	s->ps.stride_Y = s->ps.width << ((s->ps.BitDepth[0] - 1) >> 3);
	int width_C = (s->ps.chroma_format_idc == 0) ? 0 : s->ps.width << ((s->ps.BitDepth[1] - 1) >> 3);
	s->ps.stride_C = (s->ps.chroma_format_idc == 3) ? width_C : width_C >> 1;
	int pic_height_in_map_units = get_ue16() + 1;
	s->ps.frame_mbs_only_flag = get_u1();
	s->ps.height = min((s->ps.frame_mbs_only_flag) ? pic_height_in_map_units :
		pic_height_in_map_units << 1, 543) << 4;
	printf("<li>max_num_ref_frames: <code>%u</code></li>\n"
		"<li>gaps_in_frame_num_value_allowed_flag: <code>%x</code></li>\n"
		"<li>width: <code>%u</code></li>\n"
		"<li>height: <code>%u</code></li>\n"
		"<li>frame_mbs_only_flag: <code>%x</code></li>\n",
		s->ps.max_num_ref_frames,
		gaps_in_frame_num_value_allowed_flag,
		s->ps.width,
		s->ps.height,
		s->ps.frame_mbs_only_flag);
	
	// Evil has a name...
	if (s->ps.frame_mbs_only_flag == 0) {
		s->ps.mb_adaptive_frame_field_flag = get_u1();
		printf("<li>mb_adaptive_frame_field_flag: <code>%x</code></li>\n",
			s->ps.mb_adaptive_frame_field_flag);
	}
	s->ps.direct_8x8_inference_flag = get_u1();
	printf("<li>direct_8x8_inference_flag: <code>%x</code></li>\n",
		s->ps.direct_8x8_inference_flag);
	
	// frame_cropping_flag
	if (get_u1()) {
		unsigned shiftX = (s->ps.ChromaArrayType == 1) | (s->ps.ChromaArrayType == 2);
		unsigned shiftY = (s->ps.ChromaArrayType == 1) + (s->ps.frame_mbs_only_flag ^ 1);
		int limX = (s->ps.width - 1) >> shiftX << shiftX;
		int limY = (s->ps.height - 1) >> shiftY << shiftY;
		s->ps.frame_crop_left_offset = min(get_ue16() << shiftX, limX);
		s->ps.frame_crop_right_offset = min(get_ue16() << shiftX, limX - s->ps.frame_crop_left_offset);
		s->ps.frame_crop_top_offset = min(get_ue16() << shiftY, limY);
		s->ps.frame_crop_bottom_offset = min(get_ue16() << shiftY, limY - s->ps.frame_crop_top_offset);
		printf("<li>frame_crop_left_offset: <code>%u</code></li>\n"
			"<li>frame_crop_right_offset: <code>%u</code></li>\n"
			"<li>frame_crop_top_offset: <code>%u</code></li>\n"
			"<li>frame_crop_bottom_offset: <code>%u</code></li>\n",
			s->ps.frame_crop_left_offset,
			s->ps.frame_crop_right_offset,
			s->ps.frame_crop_top_offset,
			s->ps.frame_crop_bottom_offset);
	}
	if (get_u1())
		parse_vui_parameters();
	if (get_uv(24) != 0x800000 || seq_parameter_set_id > 0 || s->ps.separate_colour_plane_flag)
		return NULL;
	
	// Reallocate the DPB when the image format changes.
	if (s->ps.chroma_format_idc != e->SPS.chroma_format_idc || memcmp(&s->ps, &e->SPS, 8) != 0) {
		int PicSizeInMbs = s->ps.width * s->ps.height >> 8;
		int pixY = s->ps.stride_Y * s->ps.height;
		int pixC = s->ps.stride_C * (s->ps.chroma_format_idc < 2 ? s->ps.height : s->ps.height << 1);
		int mvs = PicSizeInMbs << 6;
		int refs = PicSizeInMbs << 2;
		int flags = PicSizeInMbs;
		
		// (DPB != NULL) is used as indicator that the SPS is initialised.
		if (e->DPB != NULL) {
			free(e->DPB);
			memset(e, 0, sizeof(*e));
		}
		e->DPB = malloc((pixY + pixC + mvs + refs + flags) * (s->ps.max_num_ref_frames + 1));
	}
	e->SPS = s->ps;
	memcpy(e->PicOrderCntDeltas, PicOrderCntDeltas, (s->ps.num_ref_frames_in_pic_order_cnt_cycle + 1) << 2);
	
	// CPB pointer might have gone far if following NAL is xx000003000003000003...
	// so we backtrack to a point that is 100% behind next start code.
	return s->CPB - 26;
}



/**
 * This function allocates a decoding context on stack and branches to the
 * handler given by nal_unit_type.
 */
 const uint8_t *Edge264_decode_NAL(const uint8_t *CPB, const uint8_t *end, Edge264_ctx *e)
{
	static const char * const nal_unit_type_names[32] = {
		[0] = "unknown",
		[1] = "Coded slice of a non-IDR picture",
		[2] = "Coded slice data partition A",
		[3] = "Coded slice data partition B",
		[4] = "Coded slice data partition C",
		[5] = "Coded slice of an IDR picture",
		[6] = "Supplemental enhancement information (SEI)",
		[7] = "Sequence parameter set",
		[8] = "Picture parameter set",
		[9] = "Access unit delimiter",
		[10] = "End of sequence",
		[11] = "End of stream",
		[12] = "Filler data",
		[13] = "Sequence parameter set extension",
		[14] = "Prefix NAL unit",
		[15] = "Subset sequence parameter set",
		[16 ... 18] = "unknown",
		[19] = "Coded slice of an auxiliary coded picture",
		[20] = "Coded slice extension",
		[21] = "Coded slice extension for depth view components",
		[22 ... 31] = "unknown",
	};
	typedef const uint8_t *(*Parser)(Edge264_ctx *, const uint8_t *, const uint8_t *);
	static const Parser parse_nal_unit[32] = {
		[1] = parse_slice_layer_without_partitioning,
		[5] = parse_slice_layer_without_partitioning,
		[7] = parse_seq_parameter_set,
		[8] = parse_pic_parameter_set,
		[9] = parse_access_unit_delimiter,
		[10] = parse_end_of_stream,
		[11] = parse_end_of_stream,
	};
	
	// beware we're parsing a NAL header :)
	unsigned nal_ref_idc = *CPB >> 5;
	unsigned nal_unit_type = *CPB & 0x1f;
	printf("<ul class=\"frame\">\n"
		"<li>nal_ref_idc: <code>%u</code></li>\n"
		"<li%s>nal_unit_type: <code>%u (%s)</code></li>\n",
		nal_ref_idc,
		red_if(parse_nal_unit[nal_unit_type] == NULL), nal_unit_type, nal_unit_type_names[nal_unit_type]);
	
	// decoding context and branching on nal_unit_type
	Edge264_slice *old = s, slice;
	s = &slice;
	if (parse_nal_unit[nal_unit_type] != NULL) {
		const uint8_t *res = parse_nal_unit[nal_unit_type](e, CPB, end);
		if (res == NULL)
			printf("<li style=\"color: red\">Erroneous NAL unit</li>\n");
		CPB = (res > CPB) ? res : CPB;
	}
	s = old;
	printf("</ul>\n");
	return Edge264_find_start_code(CPB, end, 1);
}

