#include "edge264_internal.h"

#include "edge264_bitstream.c"
#include "edge264_deblock.c"
#include "edge264_inter.c"
#include "edge264_intra.c"
#include "edge264_mvpred.c"
#include "edge264_residual.c"
#define CABAC 0
#include "edge264_slice.c"
#define CABAC 1
#include "edge264_slice.c"



/**
 * Default scaling matrices (tables 7-3 and 7-4).
 */
static const i8x16 Default_4x4_Intra =
	{6, 13, 20, 28, 13, 20, 28, 32, 20, 28, 32, 37, 28, 32, 37, 42};
static const i8x16 Default_4x4_Inter =
	{10, 14, 20, 24, 14, 20, 24, 27, 20, 24, 27, 30, 24, 27, 30, 34};
static const i8x16 Default_8x8_Intra[4] = {
	{ 6, 10, 13, 16, 18, 23, 25, 27, 10, 11, 16, 18, 23, 25, 27, 29},
	{13, 16, 18, 23, 25, 27, 29, 31, 16, 18, 23, 25, 27, 29, 31, 33},
	{18, 23, 25, 27, 29, 31, 33, 36, 23, 25, 27, 29, 31, 33, 36, 38},
	{25, 27, 29, 31, 33, 36, 38, 40, 27, 29, 31, 33, 36, 38, 40, 42},
};
static const i8x16 Default_8x8_Inter[4] = {
	{ 9, 13, 15, 17, 19, 21, 22, 24, 13, 13, 17, 19, 21, 22, 24, 25},
	{15, 17, 19, 21, 22, 24, 25, 27, 17, 19, 21, 22, 24, 25, 27, 28},
	{19, 21, 22, 24, 25, 27, 28, 30, 21, 22, 24, 25, 27, 28, 30, 32},
	{22, 24, 25, 27, 28, 30, 32, 33, 24, 25, 27, 28, 30, 32, 33, 35},
};



/**
 * This function sets the context pointers to the frame about to be decoded,
 * and fills the context caches with useful values.
 */
static void initialize_context(Edge264Context *ctx, int currPic)
{
	static const int8_t QP_Y2C[88] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39,
		39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39};
	
	union { int8_t q[32]; i8x16 v[2]; } tb, td;
	ctx->PicOrderCnt = min(ctx->d->FieldOrderCnt[0][currPic], ctx->d->FieldOrderCnt[1][currPic]);
	ctx->CurrMbAddr = ctx->t.first_mb_in_slice;
	ctx->mby = (unsigned)ctx->t.first_mb_in_slice / (unsigned)ctx->t.pic_width_in_mbs;
	ctx->mbx = (unsigned)ctx->t.first_mb_in_slice % (unsigned)ctx->t.pic_width_in_mbs;
	ctx->samples_mb[0] = ctx->t.samples_base + (ctx->mbx + ctx->mby * ctx->t.stride[0]) * 16;
	ctx->samples_mb[1] = ctx->t.samples_base + ctx->t.plane_size_Y + (ctx->mbx + ctx->mby * ctx->t.stride[1]) * 8;
	ctx->samples_mb[2] = ctx->samples_mb[1] + (ctx->t.stride[1] >> 1);
	int mb_offset = ctx->t.plane_size_Y + ctx->t.plane_size_C + sizeof(Edge264Macroblock) * (ctx->mbx + ctx->mby * (ctx->t.pic_width_in_mbs + 1));
	ctx->mbCol = ctx->_mb = (Edge264Macroblock *)(ctx->t.samples_base + mb_offset);
	ctx->A4x4_int8_v = (i16x16){0, 0, 2, 2, 1, 4, 3, 6, 8, 8, 10, 10, 9, 12, 11, 14};
	ctx->B4x4_int8_v = (i32x16){0, 1, 0, 1, 4, 5, 4, 5, 2, 3, 8, 9, 6, 7, 12, 13};
	if (ctx->t.ChromaArrayType == 1) {
		ctx->ACbCr_int8_v[0] = (i16x8){0, 0, 2, 2, 4, 4, 6, 6};
		ctx->BCbCr_int8_v[0] = (i32x8){0, 1, 0, 1, 4, 5, 4, 5};
	}
	
	ctx->QP_C_v[0] = load128(QP_Y2C + 12 + ctx->t.pps.chroma_qp_index_offset);
	ctx->QP_C_v[1] = load128(QP_Y2C + 28 + ctx->t.pps.chroma_qp_index_offset);
	ctx->QP_C_v[2] = load128(QP_Y2C + 44 + ctx->t.pps.chroma_qp_index_offset);
	ctx->QP_C_v[3] = load128(QP_Y2C + 60 + ctx->t.pps.chroma_qp_index_offset);
	ctx->QP_C_v[4] = load128(QP_Y2C + 12 + ctx->t.pps.second_chroma_qp_index_offset);
	ctx->QP_C_v[5] = load128(QP_Y2C + 28 + ctx->t.pps.second_chroma_qp_index_offset);
	ctx->QP_C_v[6] = load128(QP_Y2C + 44 + ctx->t.pps.second_chroma_qp_index_offset);
	ctx->QP_C_v[7] = load128(QP_Y2C + 60 + ctx->t.pps.second_chroma_qp_index_offset);
	ctx->t.QP[1] = ctx->QP_C[0][ctx->t.QP[0]];
	ctx->t.QP[2] = ctx->QP_C[1][ctx->t.QP[0]];
	for (int i = 1; i < 4; i++) {
		ctx->sig_inc_v[i] = sig_inc_8x8[0][i];
		ctx->last_inc_v[i] = last_inc_8x8[i];
		ctx->scan_v[i] = scan_8x8_cabac[0][i];
	}
	
	// P/B slices
	if (ctx->t.slice_type < 2) {
		ctx->refIdx4x4_C_v = (i8x16){2, 3, 12, -1, 3, 6, 13, -1, 12, 13, 14, -1, 13, -1, 15, -1};
		ctx->absMvd_A_v = (i16x16){0, 0, 4, 4, 2, 8, 6, 12, 16, 16, 20, 20, 18, 24, 22, 28};
		ctx->absMvd_B_v = (i32x16){0, 2, 0, 2, 8, 10, 8, 10, 4, 6, 16, 18, 12, 14, 24, 26};
		ctx->mvs_A_v = (i16x16){0, 0, 2, 2, 1, 4, 3, 6, 8, 8, 10, 10, 9, 12, 11, 14};
		ctx->mvs_B_v = (i32x16){0, 1, 0, 1, 4, 5, 4, 5, 2, 3, 8, 9, 6, 7, 12, 13};
		ctx->mvs_C_v = (i32x16){0, 1, 1, -1, 4, 5, 5, -1, 3, 6, 9, -1, 7, -1, 13, -1};
		ctx->mvs_D_v = (i32x16){0, 1, 2, 0, 4, 5, 1, 4, 8, 2, 10, 8, 3, 6, 9, 12};
		ctx->num_ref_idx_mask = (ctx->t.pps.num_ref_idx_active[0] > 1) * 0x0f + (ctx->t.pps.num_ref_idx_active[1] > 1) * 0xf0;
		ctx->transform_8x8_mode_flag = ctx->t.pps.transform_8x8_mode_flag; // for P slices this value is constant
		int max0 = ctx->t.pps.num_ref_idx_active[0] - 1;
		int max1 = ctx->t.slice_type == 0 ? -1 : ctx->t.pps.num_ref_idx_active[1] - 1;
		ctx->clip_ref_idx_v = (i8x8){max0, max0, max0, max0, max1, max1, max1, max1};
		
		// B slides
		if (ctx->t.slice_type == 1) {
			ctx->mbCol = (Edge264Macroblock *)(ctx->t.frame_buffers[ctx->t.RefPicList[1][0]] + mb_offset);
			ctx->col_short_term = 1 & ~(ctx->t.long_term_flags >> ctx->t.RefPicList[1][0]);
			
			// initializations for temporal prediction and implicit weights
			int rangeL1 = ctx->t.pps.num_ref_idx_active[1];
			if (ctx->t.pps.weighted_bipred_idc == 2 || (rangeL1 = 1, !ctx->t.direct_spatial_mv_pred_flag)) {
				tb.v[0] = packs16(ctx->t.diff_poc_v[0], ctx->t.diff_poc_v[1]);
				tb.v[1] = packs16(ctx->t.diff_poc_v[2], ctx->t.diff_poc_v[3]);
				ctx->MapPicToList0_v[0] = ctx->MapPicToList0_v[1] = (i8x16){}; // FIXME pictures not found in RefPicList0 should point to self
				for (int refIdxL0 = ctx->t.pps.num_ref_idx_active[0], DistScaleFactor; refIdxL0-- > 0; ) {
					int pic0 = ctx->t.RefPicList[0][refIdxL0];
					ctx->MapPicToList0[pic0] = refIdxL0;
					i16x8 diff0 = set16(ctx->t.diff_poc[pic0]);
					td.v[0] = packs16(diff0 - ctx->t.diff_poc_v[0], diff0 - ctx->t.diff_poc_v[1]);
					td.v[1] = packs16(diff0 - ctx->t.diff_poc_v[2], diff0 - ctx->t.diff_poc_v[3]);
					for (int refIdxL1 = rangeL1, implicit_weight; refIdxL1-- > 0; ) {
						int pic1 = ctx->t.RefPicList[1][refIdxL1];
						if (td.q[pic1] != 0 && !(ctx->t.long_term_flags & 1 << pic0)) {
							int tx = (16384 + abs(td.q[pic1] / 2)) / td.q[pic1];
							DistScaleFactor = min(max((tb.q[pic0] * tx + 32) >> 6, -1024), 1023);
							implicit_weight = (!(ctx->t.long_term_flags & 1 << pic1) && DistScaleFactor >= -256 && DistScaleFactor <= 515) ? DistScaleFactor >> 2 : 32;
						} else {
							DistScaleFactor = 256;
							implicit_weight = 32;
						}
						ctx->implicit_weights[refIdxL0][refIdxL1] = implicit_weight + 64;
					}
					ctx->DistScaleFactor[refIdxL0] = DistScaleFactor;
				}
			}
		}
	}
}



/**
 * Helper function to raise a probability sampled to 0..65535 to a power k.
 */
static unsigned ppow(unsigned p65536, unsigned k) {
	unsigned r = 65536;
	while (k) {
		if (k & 1)
			r = (r * p65536) >> 16;
		p65536 = (p65536 * p65536) >> 16;
		k >>= 1;
	}
	return r;
}



/**
 * If the slice ends on error, invalidate all its mbs and recover them.
 * 
 * For CAVLC the error is equiprobable in all of the slice mbs.
 * For CABAC every erroneous mb had a random probability p=2/383 to exit
 * early at end_of_slice_flag, so for each mb we only count a proportion
 * that reached CurrMbAddr without early exit: (1-p)^d, d being the
 * distance to the last decoded mb. We sum these proportions to normalize
 * the probabilities: (1-(1-p)^n)/p. Then we compute each probability as
 * the normalized sum of its proportion and all proportions before it:
 * 1-(1-(1-p)^d)/(1-(1-p)^n). Note that p is sampled to 16-bits int to
 * avoid dependency on float and to fit all multiplications on 32 bits
 * with max precision.
*/
static void recover_slice(Edge264Context *ctx, int currPic) {
	// mark all previous mbs as erroneous and assign them an error probability
	ctx->mby = (unsigned)ctx->t.first_mb_in_slice / (unsigned)ctx->t.pic_width_in_mbs;
	ctx->mbx = (unsigned)ctx->t.first_mb_in_slice % (unsigned)ctx->t.pic_width_in_mbs;
	ctx->samples_mb[0] = ctx->t.samples_base + (ctx->mbx + ctx->mby * ctx->t.stride[0]) * 16;
	ctx->samples_mb[1] = ctx->t.samples_base + ctx->t.plane_size_Y + (ctx->mbx + ctx->mby * ctx->t.stride[1]) * 8;
	ctx->samples_mb[2] = ctx->samples_mb[1] + (ctx->t.stride[1] >> 1);
	int mb_offset = ctx->t.plane_size_Y + ctx->t.plane_size_C + sizeof(Edge264Macroblock) * (ctx->mbx + ctx->mby * (ctx->t.pic_width_in_mbs + 1));
	ctx->_mb = (Edge264Macroblock *)(ctx->t.samples_base + mb_offset);
	ctx->mbCol = (Edge264Macroblock *)(ctx->t.frame_buffers[ctx->t.RefPicList[1][0]] + mb_offset);
	unsigned num = ctx->CurrMbAddr - ctx->t.first_mb_in_slice;
	unsigned div = 65536 - ppow(65194, num);
	for (unsigned i = 0; i < num; i++) {
		unsigned p12800 = (!ctx->t.pps.entropy_coding_mode_flag) ?
			((i + 1) * 12800 + num - 1) / num : // division with upward rounding
			((div - (65536 - ppow(65194, num - 1 - i))) * 12800 + div - 1) / div;
		ctx->_mb->error_probability = p12800 >> 7;
		unsigned p128 = p12800 / 100;
		
		// recover the macroblock depending on slice_type
		if (ctx->t.slice_type == 2) { // I slice -> blend with intra DC
			uint8_t * restrict p = ctx->samples_mb[0];
			size_t stride = ctx->t.stride[0];
			INIT_P();
			i8x16 l = set8(-128), t = l;
			if (i == 0 || ctx->mbx == 0) { // A not available
				if (i >= ctx->t.pic_width_in_mbs) // B available
					l = t = load128(P(0, -1));
			} else { // A available
				l = t = ldleft16(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
				if (i >= ctx->t.pic_width_in_mbs) // B available
					t = load128(P(0, -1));
			}
			i8x16 dcY = broadcast8(shrru16(sumd8(t, l), 5), __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__);
			i8x16 w8 = ziplo8(set8(128 - p128), set8(p128));
			i16x8 w16 = {128 - p128, p128};
			i16x8 o = {};
			i64x2 wd64 = {7};
			i16x8 wd16 = set16(7);
			if (p128 == 128) {
				w8 = (i8x16){0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
				w16 = (i16x8){0, 1};
				wd64 = wd16 = o;
			}
			*(i8x16 *)P(0, 0) = maddshr8(*(i8x16 *)P(0, 0), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 1) = maddshr8(*(i8x16 *)P(0, 1), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 2) = maddshr8(*(i8x16 *)P(0, 2), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 3) = maddshr8(*(i8x16 *)P(0, 3), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 4) = maddshr8(*(i8x16 *)P(0, 4), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 5) = maddshr8(*(i8x16 *)P(0, 5), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 6) = maddshr8(*(i8x16 *)P(0, 6), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 7) = maddshr8(*(i8x16 *)P(0, 7), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 8) = maddshr8(*(i8x16 *)P(0, 8), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 9) = maddshr8(*(i8x16 *)P(0, 9), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 10) = maddshr8(*(i8x16 *)P(0, 10), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 11) = maddshr8(*(i8x16 *)P(0, 11), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 12) = maddshr8(*(i8x16 *)P(0, 12), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 13) = maddshr8(*(i8x16 *)P(0, 13), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 14) = maddshr8(*(i8x16 *)P(0, 14), dcY, w8, w16, o, wd64, wd16);
			*(i8x16 *)P(0, 15) = maddshr8(*(i8x16 *)P(0, 15), dcY, w8, w16, o, wd64, wd16);
			{
				uint8_t * restrict p = ctx->samples_mb[1];
				size_t stride = ctx->t.stride[1] >> 1;
				INIT_P();
				i8x16 b = ziplo64(load64(P(0, -2)), ldleft8(0, 2, 4, 6, 8, 10, 12, 14));
				i8x16 r = ziplo64(load64(P(0, -1)), ldleft8(1, 3, 5, 7, 9, 11, 13, 15));
				i8x16 dcb = broadcast8(shrru16(sum8(b), 4), __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__);
				i8x16 dcr = broadcast8(shrru16(sum8(r), 4), __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__);
				i8x16 dcC = ziplo64(dcb, dcr);
				i64x2 v0 = maddshr8(load8x2(P(0, 0), P(0, 1)), dcC, w8, w16, o, wd64, wd16);
				i64x2 v1 = maddshr8(load8x2(P(0, 2), P(0, 3)), dcC, w8, w16, o, wd64, wd16);
				i64x2 v2 = maddshr8(load8x2(P(0, 4), P(0, 5)), dcC, w8, w16, o, wd64, wd16);
				i64x2 v3 = maddshr8(load8x2(P(0, 6), P(0, 7)), dcC, w8, w16, o, wd64, wd16);
				i64x2 v4 = maddshr8(load8x2(P(0, 8), P(0, 9)), dcC, w8, w16, o, wd64, wd16);
				i64x2 v5 = maddshr8(load8x2(P(0, 10), P(0, 11)), dcC, w8, w16, o, wd64, wd16);
				i64x2 v6 = maddshr8(load8x2(P(0, 12), P(0, 13)), dcC, w8, w16, o, wd64, wd16);
				i64x2 v7 = maddshr8(load8x2(P(0, 14), P(0, 15)), dcC, w8, w16, o, wd64, wd16);
				*(int64_t *)P(0, 0) = v0[0];
				*(int64_t *)P(0, 1) = v0[1];
				*(int64_t *)P(0, 2) = v1[0];
				*(int64_t *)P(0, 3) = v1[1];
				*(int64_t *)P(0, 4) = v2[0];
				*(int64_t *)P(0, 5) = v2[1];
				*(int64_t *)P(0, 6) = v3[0];
				*(int64_t *)P(0, 7) = v3[1];
				*(int64_t *)P(0, 8) = v4[0];
				*(int64_t *)P(0, 9) = v4[1];
				*(int64_t *)P(0, 10) = v5[0];
				*(int64_t *)P(0, 11) = v5[1];
				*(int64_t *)P(0, 12) = v6[0];
				*(int64_t *)P(0, 13) = v6[1];
				*(int64_t *)P(0, 14) = v7[0];
				*(int64_t *)P(0, 15) = v7[1];
			}
		} else if (i > 0 && p128 >= 32) { // recover above 25% error (arbitrary)
			if (ctx->t.slice_type == 0) { // P slice -> P_Skip
				mb->nC_v[0] = (i8x16){};
				decode_P_skip(ctx);
			} else { // B slice -> B_Skip
				mb->nC_v[0] = (i8x16){};
				decode_direct_mv_pred(ctx, 0xffffffff);
			}
		}
		__atomic_store_n(&ctx->_mb->recovery_bits, ctx->t.frame_flip_bit + 2, __ATOMIC_RELEASE);
		
		// point to the next macroblock
		ctx->_mb++;
		ctx->mbx++;
		ctx->mbCol++;
		ctx->samples_mb[0] += 16;
		ctx->samples_mb[1] += 8;
		ctx->samples_mb[2] += 8;
		if (ctx->mbx >= ctx->t.pic_width_in_mbs) {
			ctx->_mb++;
			ctx->mbx = 0;
			ctx->mby++;
			ctx->mbCol++;
			ctx->samples_mb[0] += ctx->t.stride[0] * 16 - ctx->t.pic_width_in_mbs * 16;
			ctx->samples_mb[1] += ctx->t.stride[1] * 8 - ctx->t.pic_width_in_mbs * 8;
			ctx->samples_mb[2] += ctx->t.stride[1] * 8 - ctx->t.pic_width_in_mbs * 8;
		}
	}
}



/**
 * This function is called when a frame ends with a positive remaining_mbs.
 * 
 * It sets up recover_slice to go through all mbs and recover them while
 * setting their error probability to 100%.
 */
static void recover_frame(Edge264Decoder *dec) {
	
}



/**
 * This function is the entry point for each worker thread, where it consumes
 * tasks continuously until killed by the parent process.
 */
void *ADD_VARIANT(worker_loop)(Edge264Decoder *dec) {
	Edge264Context c;
	c.d = dec;
	c.n_threads = dec->n_threads;
	#if EDGE264_TRACE
	c.trace_slices = dec->trace_slices;
	#endif
	if (c.n_threads)
		pthread_mutex_lock(&dec->lock);
	for (;;) {
		while (c.n_threads && !dec->ready_tasks)
			pthread_cond_wait(&dec->task_ready, &dec->lock);
		int task_id = __builtin_ctz(dec->ready_tasks); // FIXME arbitrary selection for now
		int currPic = dec->taskPics[task_id];
		dec->pending_tasks &= ~(1 << task_id);
		dec->ready_tasks &= ~(1 << task_id);
		if (c.n_threads) {
			pthread_mutex_unlock(&dec->lock);
			print_header(dec, "<h>Thread started decoding frame %d at macroblock %d</h>\n", dec->FieldOrderCnt[0][dec->taskPics[task_id]], dec->tasks[task_id].first_mb_in_slice);
		}
		c.t = dec->tasks[task_id];
		initialize_context(&c, currPic);
		size_t ret = 0;
		if (!c.t.pps.entropy_coding_mode_flag) {
			c.mb_skip_run = -1;
			parse_slice_data_cavlc(&c);
			// FIXME detect and signal error
		} else {
			// cabac_alignment_one_bit gives a good probability to catch random errors.
			if (cabac_start(&c)) {
				ret = EBADMSG; // FIXME error_flag
			} else {
				cabac_init(&c);
				c.mb_qp_delta_nz = 0;
				parse_slice_data_cabac(&c);
				// the possibility of cabac_zero_word implies we should not expect a start code yet
				if (c.t._gb.msb_cache != 0 || (c.t._gb.lsb_cache & (c.t._gb.lsb_cache - 1))) {
					ret = EBADMSG; // FIXME error_flag
				}
			}
		}
		
		// deblock the rest of mbs in this slice
		if (c.t.next_deblock_addr >= 0) {
			c.t.next_deblock_addr = max(c.t.next_deblock_addr, c.t.first_mb_in_slice);
			c.mby = (unsigned)c.t.next_deblock_addr / (unsigned)c.t.pic_width_in_mbs;
			c.mbx = (unsigned)c.t.next_deblock_addr % (unsigned)c.t.pic_width_in_mbs;
			c.samples_mb[0] = c.t.samples_base + (c.mbx + c.mby * c.t.stride[0]) * 16;
			c.samples_mb[1] = c.t.samples_base + c.t.plane_size_Y + (c.mbx + c.mby * c.t.stride[1]) * 8;
			c.samples_mb[2] = c.samples_mb[1] + (c.t.stride[1] >> 1);
			c._mb = (Edge264Macroblock *)(c.t.samples_base + c.t.plane_size_Y + c.t.plane_size_C) + c.mbx + c.mby * (c.t.pic_width_in_mbs + 1);
			while (c.t.next_deblock_addr < c.CurrMbAddr) {
				deblock_mb(&c);
				c.t.next_deblock_addr++;
				c._mb++;
				c.mbx++;
				c.samples_mb[0] += 16;
				c.samples_mb[1] += 8;
				c.samples_mb[2] += 8;
				if (c.mbx >= c.t.pic_width_in_mbs) {
					c._mb++;
					c.mbx = 0;
					c.samples_mb[0] += c.t.stride[0] * 16 - c.t.pic_width_in_mbs * 16;
					c.samples_mb[1] += c.t.stride[1] * 8 - c.t.pic_width_in_mbs * 8;
					c.samples_mb[2] += c.t.stride[1] * 8 - c.t.pic_width_in_mbs * 8;
				}
			}
		}
		
		// on error, recover mbs and signal them as erroneous (allows overwrite by redundant slices)
		if (__builtin_expect(ret != 0, 0))
			recover_slice(&c, currPic);
		
		// update dec->next_deblock_addr, considering it might have reached first_mb_in_slice since start
		if (dec->next_deblock_addr[currPic] >= c.t.first_mb_in_slice &&
		    !(c.t.disable_deblocking_filter_idc == 0 && c.t.next_deblock_addr < 0)) {
			dec->next_deblock_addr[currPic] = c.CurrMbAddr;
			pthread_cond_broadcast(&dec->task_progress);
		}
		
		// deblock the rest of the frame if all mbs have been decoded correctly
		int remaining_mbs = ret ?: __atomic_sub_fetch(&dec->remaining_mbs[currPic], c.CurrMbAddr - c.t.first_mb_in_slice, __ATOMIC_ACQ_REL);
		if (remaining_mbs == 0) {
			c.t.next_deblock_addr = dec->next_deblock_addr[currPic];
			c.CurrMbAddr = c.t.pic_width_in_mbs * c.t.pic_height_in_mbs;
			if ((unsigned)c.t.next_deblock_addr < c.CurrMbAddr) {
				c.mby = (unsigned)c.t.next_deblock_addr / (unsigned)c.t.pic_width_in_mbs;
				c.mbx = (unsigned)c.t.next_deblock_addr % (unsigned)c.t.pic_width_in_mbs;
				c.samples_mb[0] = c.t.samples_base + (c.mbx + c.mby * c.t.stride[0]) * 16;
				c.samples_mb[1] = c.t.samples_base + c.t.plane_size_Y + (c.mbx + c.mby * c.t.stride[1]) * 8;
				c.samples_mb[2] = c.samples_mb[1] + (c.t.stride[1] >> 1);
				c._mb = (Edge264Macroblock *)(c.t.samples_base + c.t.plane_size_Y + c.t.plane_size_C) + c.mbx + c.mby * (c.t.pic_width_in_mbs + 1);
				while (c.t.next_deblock_addr < c.CurrMbAddr) {
					deblock_mb(&c);
					c.t.next_deblock_addr++;
					c._mb++;
					c.mbx++;
					c.samples_mb[0] += 16;
					c.samples_mb[1] += 8;
					c.samples_mb[2] += 8;
					if (c.mbx >= c.t.pic_width_in_mbs) {
						c._mb++;
						c.mbx = 0;
						c.samples_mb[0] += c.t.stride[0] * 16 - c.t.pic_width_in_mbs * 16;
						c.samples_mb[1] += c.t.stride[1] * 8 - c.t.pic_width_in_mbs * 8;
						c.samples_mb[2] += c.t.stride[1] * 8 - c.t.pic_width_in_mbs * 8;
					}
				}
			}
			dec->next_deblock_addr[currPic] = INT_MAX; // signals the frame is complete
		}
		
		// if multi-threaded, check if we are the last task to touch this frame and ensure it is complete
		if (c.n_threads) {
			pthread_mutex_lock(&dec->lock);
			pthread_cond_signal(&dec->task_complete);
			print_header(dec, "<h>Thread finished decoding frame %d at macroblock %d</h>\n", dec->FieldOrderCnt[0][currPic], c.t.first_mb_in_slice);
			if (remaining_mbs == 0) {
				pthread_cond_broadcast(&dec->task_progress);
				dec->ready_tasks = ready_tasks(dec);
				if (dec->ready_tasks)
					pthread_cond_broadcast(&dec->task_ready);
			}
		}
		if (c.t.free_cb)
			c.t.free_cb(c.t.free_arg, (int)ret);
		dec->busy_tasks &= ~(1 << task_id);
		dec->task_dependencies[task_id] = 0;
		dec->taskPics[task_id] = -1;
		
		// in single-thread mode update the buffer pointer and return
		if (!c.n_threads) {
			dec->_gb = c.t._gb;
			return (void *)ret;
		}
	}
	return NULL;
}



/**
 * Updates the reference flags by adaptive memory control or sliding window
 * marking process (8.2.5).
 */
static void parse_dec_ref_pic_marking(Edge264Decoder *dec)
{
	static const char * const memory_management_control_operation_names[6] = {
		"%s1 (dereference frame %u)",
		"%s2 (dereference long-term frame %3$u)",
		"%s3 (convert frame %u into long-term index %u)",
		"%s4 (dereference long-term frames on and above %3$d)",
		"%s5 (convert current picture to IDR and dereference all frames)",
		"%s6 (assign long-term index %3$u to current picture)"};
	
	// while the exact release time of non-ref frames in C.4.5.2 is ambiguous, we ignore no_output_of_prior_pics_flag
	if (dec->IdrPicFlag) {
		int no_output_of_prior_pics_flag = get_u1(&dec->_gb);
		dec->pic_reference_flags = 1 << dec->currPic;
		dec->pic_long_term_flags = get_u1(&dec->_gb) << dec->currPic;
		dec->pic_LongTermFrameIdx_v[0] = dec->pic_LongTermFrameIdx_v[1] = (i8x16){};
		print_header(dec, "<k>no_output_of_prior_pics_flag</k><v>%x</v>\n"
			"<k>long_term_reference_flag</k><v>%x</v>\n",
			no_output_of_prior_pics_flag,
			dec->pic_long_term_flags >> dec->currPic);
		return;
	}
	
	// 8.2.5.4 - Adaptive memory control marking process.
	int memory_management_control_operation;
	int i = 32;
	if (get_u1(&dec->_gb)) {
		while ((memory_management_control_operation = get_ue16(&dec->_gb, 6)) != 0 && i-- > 0) {
			int target = dec->currPic, long_term_frame_idx = 0;
			if (10 & 1 << memory_management_control_operation) { // 1 or 3
				int FrameNum = dec->FrameNum - 1 - get_ue32(&dec->_gb, 4294967294);
				for (unsigned r = dec->pic_reference_flags & ~dec->pic_long_term_flags; r; r &= r - 1) {
					int j = __builtin_ctz(r);
					if (dec->FrameNums[j] == FrameNum) {
						target = j;
						if (memory_management_control_operation == 1) {
							dec->pic_reference_flags ^= 1 << j;
							dec->pic_long_term_flags &= ~(1 << j);
						}
					}
				}
			}
			if (92 & 1 << memory_management_control_operation) { // 2 or 3 or 4 or 6
				long_term_frame_idx = get_ue16(&dec->_gb, (dec->sps.max_num_ref_frames >> dec->sps.mvc) - (memory_management_control_operation != 4));
				for (unsigned r = dec->pic_long_term_flags; r; r &= r - 1) {
					int j = __builtin_ctz(r);
					if (dec->pic_LongTermFrameIdx[j] == long_term_frame_idx ||
						(dec->pic_LongTermFrameIdx[j] > long_term_frame_idx &&
						memory_management_control_operation == 4)) {
						dec->pic_reference_flags ^= 1 << j;
						dec->pic_long_term_flags ^= 1 << j;
					}
				}
				if (72 & 1 << memory_management_control_operation) { // 3 or 6
					dec->pic_LongTermFrameIdx[target] = long_term_frame_idx;
					dec->pic_long_term_flags |= 1 << target;
				}
			}
			if (memory_management_control_operation == 5) {
				dec->IdrPicFlag = 1;
				dec->pic_reference_flags = 0;
				dec->FrameNums[dec->currPic] = 0;
				dec->pic_long_term_flags = 0;
				dec->pic_LongTermFrameIdx_v[0] = dec->pic_LongTermFrameIdx_v[1] = (i8x16){};
				int tempPicOrderCnt = min(dec->TopFieldOrderCnt, dec->BottomFieldOrderCnt);
				dec->FieldOrderCnt[0][dec->currPic] = dec->TopFieldOrderCnt - tempPicOrderCnt;
				dec->FieldOrderCnt[1][dec->currPic] = dec->BottomFieldOrderCnt - tempPicOrderCnt;
			}
			print_header(dec, memory_management_control_operation_names[memory_management_control_operation - 1],
				(i == 31) ? "<k>memory_management_control_operations</k><v>" : "<br>", dec->FrameNums[target], long_term_frame_idx);
		}
		print_header(dec, "</v>\n");
	}
	
	// 8.2.5.3 - Sliding window marking process
	if (__builtin_popcount(dec->pic_reference_flags) >= (dec->sps.max_num_ref_frames >> dec->sps.mvc)) {
		int best = INT_MAX;
		int next = 0;
		for (unsigned r = dec->pic_reference_flags ^ dec->pic_long_term_flags; r != 0; r &= r - 1) {
			int i = __builtin_ctz(r);
			if (best > dec->FrameNums[i])
				best = dec->FrameNums[next = i];
		}
		dec->pic_reference_flags ^= 1 << next;
	}
	dec->pic_reference_flags |= 1 << dec->currPic;
}



/**
 * Parses coefficients for weighted sample prediction (7.4.3.2 and 8.4.2.3).
 */
static void parse_pred_weight_table(Edge264Decoder *dec, Edge264Task *t)
{
	// further tests will depend only on weighted_bipred_idc
	if (t->slice_type == 0)
		t->pps.weighted_bipred_idc = t->pps.weighted_pred_flag;
	
	// parse explicit weights/offsets
	if (t->pps.weighted_bipred_idc == 1) {
		t->luma_log2_weight_denom = get_ue16(&dec->_gb, 7);
		if (dec->sps.ChromaArrayType != 0)
			t->chroma_log2_weight_denom = get_ue16(&dec->_gb, 7);
		for (int l = 0; l <= t->slice_type; l++) {
			print_header(dec, "<k>Prediction weights L%x (weight/offset)</k><v>", l);
			for (int i = l * 32; i < l * 32 + t->pps.num_ref_idx_active[l]; i++) {
				if (get_u1(&dec->_gb)) {
					t->explicit_weights[0][i] = get_se16(&dec->_gb, -128, 127);
					t->explicit_offsets[0][i] = get_se16(&dec->_gb, -128, 127);
				} else {
					t->explicit_weights[0][i] = 1 << t->luma_log2_weight_denom;
					t->explicit_offsets[0][i] = 0;
				}
				if (dec->sps.ChromaArrayType != 0 && get_u1(&dec->_gb)) {
					t->explicit_weights[1][i] = get_se16(&dec->_gb, -128, 127);
					t->explicit_offsets[1][i] = get_se16(&dec->_gb, -128, 127);
					t->explicit_weights[2][i] = get_se16(&dec->_gb, -128, 127);
					t->explicit_offsets[2][i] = get_se16(&dec->_gb, -128, 127);
				} else {
					t->explicit_weights[1][i] = 1 << t->chroma_log2_weight_denom;
					t->explicit_offsets[1][i] = 0;
					t->explicit_weights[2][i] = 1 << t->chroma_log2_weight_denom;
					t->explicit_offsets[2][i] = 0;
				}
				print_header(dec, (dec->sps.ChromaArrayType == 0) ? "*%d/%u+%d" : "*%d/%u+%d : *%d/%u+%d : *%d/%u+%d",
					t->explicit_weights[0][i], 1 << t->luma_log2_weight_denom, t->explicit_offsets[0][i] << (dec->sps.BitDepth_Y - 8),
					t->explicit_weights[1][i], 1 << t->chroma_log2_weight_denom, t->explicit_offsets[1][i] << (dec->sps.BitDepth_C - 8),
					t->explicit_weights[2][i], 1 << t->chroma_log2_weight_denom, t->explicit_offsets[2][i] << (dec->sps.BitDepth_C - 8));
				print_header(dec, (i < t->pps.num_ref_idx_active[l] - 1) ? "<br>" : "</v>\n");
			}
		}
	}
}



/**
 * Initialises and updates the reference picture lists (8.2.4).
 *
 * Both initialisation and parsing of ref_pic_list_modification are fit into a
 * single function to foster compactness and maintenance. Performance is not
 * crucial here.
 */
static void parse_ref_pic_list_modification(Edge264Decoder *dec, Edge264Task *t)
{
	// For P we sort on FrameNum, for B we sort on PicOrderCnt.
	const int32_t *values = (t->slice_type == 0) ? dec->FrameNums : dec->FieldOrderCnt[0];
	int pic_value = (t->slice_type == 0) ? dec->FrameNum : dec->TopFieldOrderCnt;
	int count[3] = {0, 0, 0}; // number of refs before/after/long
	int size = 0;
	
	// sort all short and long term references for RefPicListL0
	for (unsigned refs = dec->pic_reference_flags, next = 0; refs; refs ^= 1 << next) {
		int best = INT_MAX;
		for (unsigned r = refs; r; r &= r - 1) {
			int i = __builtin_ctz(r);
			int diff = values[i] - pic_value;
			int ShortTermNum = (diff <= 0) ? -diff : 0x10000 + diff;
			int LongTermNum = dec->LongTermFrameIdx[i] + 0x20000;
			int v = (dec->pic_long_term_flags & 1 << i) ? LongTermNum : ShortTermNum;
			if (v < best)
				best = v, next = i;
		}
		t->RefPicList[0][size++] = next;
		count[best >> 16]++;
	}
	if (dec->basePic >= 0)
		t->RefPicList[0][size++] = dec->basePic; // add inter-view ref for MVC
	
	// fill RefPicListL1 by swapping before/after references
	for (int src = 0; src < size; src++) {
		int dst = (src < count[0]) ? src + count[1] :
			(src < count[0] + count[1]) ? src - count[0] : src;
		t->RefPicList[1][dst] = t->RefPicList[0][src];
	}
	
	// When decoding a field, extract a list of fields from each list of frames.
	/*union { int8_t q[32]; i8x16 v[2]; } RefFrameList;
	for (int l = 0; t->field_pic_flag && l <= t->slice_type; l++) {
		i8x16 v = t->RefPicList_v[l * 2];
		RefFrameList.v[0] = v;
		RefFrameList.v[1] = v + (i8x16){16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16};
		size = 0;
		int i = t->bottom_field_flag << 4; // first parity to check
		int j = i ^ 16; // other parity to alternate
		int lim_i = i + count[0] + count[1]; // set a first limit to short term frames
		int lim_j = j + count[0] + count[1]; // don't init with XOR as there can be 16 refs!
		int tot = count[0] + count[1] + count[2]; // ... then long term
		
		// probably not the most readable portion, yet otherwise needs a lot of code
		for (int k;;) {
			if (i >= lim_i) {
				if (j < lim_j) { // i reached limit but not j, swap them
					k = i, i = j, j = k;
					k = lim_i, lim_i = lim_j, lim_j = k;
				} else if (min(lim_i, lim_j) < tot) { // end of short term refs, go for long
					int parity = t->bottom_field_flag << 4;
					i = (t->bottom_field_flag << 4) + count[0] + count[1];
					j = i ^ 16;
					lim_i = i + count[2];
					lim_j = j + count[2];
				} else break; // end of long term refs, break
			}
			int pic = RefFrameList.q[i++];
			if (dec->reference_flags & 1 << pic) {
				t->RefPicList[l][size++] = pic;
				if (j < lim_j) { // swap parity if we have not emptied other parity yet
					k = i, i = j, j = k;
					k = lim_i, lim_i = lim_j, lim_j = k;
				}
			}
		}
	}*/
	
	// Swap the two first slots of RefPicListL1 if it the same as RefPicListL0.
	if (t->RefPicList[0][1] >= 0 && t->RefPicList[0][0] == t->RefPicList[1][0]) {
		t->RefPicList[1][0] = t->RefPicList[0][1];
		t->RefPicList[1][1] = t->RefPicList[0][0];
	}
	
	// parse the ref_pic_list_modification() header
	for (int l = 0; l <= t->slice_type; l++) {
		unsigned picNumLX = (t->field_pic_flag) ? dec->FrameNum * 2 + 1 : dec->FrameNum;
		if (get_u1(&dec->_gb)) { // ref_pic_list_modification_flag
			print_header(dec, "<k>ref_pic_list_modifications_l%x</k><v>", l);
			for (int refIdx = 0, modification_of_pic_nums_idc; (modification_of_pic_nums_idc = get_ue16(&dec->_gb, 5)) != 3 && refIdx < 32; refIdx++) {
				int num = get_ue32(&dec->_gb, 4294967294);
				print_header(dec, "%s%d%s", refIdx ? ", " : "",
					modification_of_pic_nums_idc % 4 == 0 ? -num - 1 : num + (modification_of_pic_nums_idc != 2),
					modification_of_pic_nums_idc == 2 ? "l" : modification_of_pic_nums_idc > 3 ? "v" : "");
				int pic = dec->basePic;
				if (modification_of_pic_nums_idc < 2) {
					picNumLX = (modification_of_pic_nums_idc == 0) ? picNumLX - (num + 1) : picNumLX + (num + 1);
					unsigned MaskFrameNum = (1 << dec->sps.log2_max_frame_num) - 1;
					for (unsigned r = dec->pic_reference_flags & ~dec->pic_long_term_flags; r; r &= r - 1) {
						pic = __builtin_ctz(r);
						if (!((dec->FrameNums[pic] ^ picNumLX) & MaskFrameNum))
							break;
					}
				} else if (modification_of_pic_nums_idc == 2) {
					for (unsigned r = dec->pic_reference_flags & dec->pic_long_term_flags; r; r &= r - 1) {
						pic = __builtin_ctz(r);
						if (dec->LongTermFrameIdx[pic] == num)
							break;
					}
				}
				int buf = pic;
				int cIdx = refIdx;
				do {
					int swap = t->RefPicList[l][cIdx];
					t->RefPicList[l][cIdx] = buf;
					buf = swap;
				} while (++cIdx < t->pps.num_ref_idx_active[l] && buf != pic);
			}
			print_header(dec, "</v>\n");
		}
	}
	
	#ifdef TRACE
		print_header(dec, "<k>RefPicLists</k><v>");
		for (int lx = 0; lx <= t->slice_type; lx++) {
			for (int i = 0; i < t->pps.num_ref_idx_active[lx]; i++)
				print_header(dec, "%d%s", t->RefPicList[lx][i], (i < t->pps.num_ref_idx_active[lx] - 1) ? ", " : (t->slice_type - lx == 1) ? "<br>" : "");
		}
		print_header(dec, "</v>\n");
	#endif
}



static int alloc_frame(Edge264Decoder *dec, int id) {
	dec->frame_buffers[id] = malloc(dec->frame_size);
	if (dec->frame_buffers[id] == NULL)
		return ENOMEM;
	Edge264Macroblock *m = (Edge264Macroblock *)(dec->frame_buffers[id] + dec->plane_size_Y + dec->plane_size_C);
	int mbs = (dec->sps.pic_width_in_mbs + 1) * dec->sps.pic_height_in_mbs - 1;
	for (int i = 0; i < mbs; i += dec->sps.pic_width_in_mbs + 1) {
		for (int j = i; j < i + dec->sps.pic_width_in_mbs; j++)
			m[j].recovery_bits = 0;
		if (i + dec->sps.pic_width_in_mbs < mbs)
			m[i + dec->sps.pic_width_in_mbs] = unavail_mb;
	}
	return 0;
}



/**
 * This function applies the updates required for the next picture. It is
 * called when a slice is received with a different frame_num/POC/view_id.
 * pair_view is set if the picture differs only by view_id.
 * 
 * The test on POC alone is not sufficient without frame_num, because the
 * correct POC value depends on FrameNum which needs an up-to-date PrevFrameNum.
 */
static void finish_frame(Edge264Decoder *dec, int pair_view) {
	if (dec->pic_reference_flags & 1 << dec->currPic)
		dec->prevPicOrderCnt = dec->FieldOrderCnt[0][dec->currPic];
	int non_base_view = dec->sps.mvc & dec->currPic & 1;
	unsigned other_views = -dec->sps.mvc & 0xaaaaaaaa >> non_base_view;
	dec->reference_flags = (dec->reference_flags & other_views) | dec->pic_reference_flags;
	dec->long_term_flags = (dec->long_term_flags & other_views) | dec->pic_long_term_flags;
	dec->LongTermFrameIdx_v[0] = dec->pic_LongTermFrameIdx_v[0];
	dec->LongTermFrameIdx_v[1] = dec->pic_LongTermFrameIdx_v[1];
	dec->prevRefFrameNum[non_base_view] = dec->FrameNums[dec->currPic]; // for mmco5
	dec->basePic = (dec->sps.mvc & ~non_base_view & pair_view) ? dec->currPic : -1;
	dec->currPic = -1;
}



/**
 * This fonction copies the last set of fields to finish initializing the task.
 */
static void initialize_task(Edge264Decoder *dec, Edge264Task *t)
{
	// set task pointer to current pointer and current pointer to next start code
	t->_gb = dec->_gb;
	if (dec->n_threads) {
		t->_gb.end = edge264_find_start_code(dec->_gb.CPB - 2, dec->_gb.end); // works if CPB already crossed end
		dec->_gb.CPB = t->_gb.end + 2;
	}
	
	// copy most essential fields from st
	t->ChromaArrayType = dec->sps.ChromaArrayType;
	t->direct_8x8_inference_flag = dec->sps.direct_8x8_inference_flag;
	t->frame_flip_bit = dec->frame_flip_bits >> dec->currPic & 1;
	t->pic_width_in_mbs = dec->sps.pic_width_in_mbs;
	t->pic_height_in_mbs = dec->sps.pic_height_in_mbs;
	t->stride[0] = dec->out.stride_Y;
	t->stride[1] = t->stride[2] = dec->out.stride_C;
	t->plane_size_Y = dec->plane_size_Y;
	t->plane_size_C = dec->plane_size_C;
	t->next_deblock_idc = (dec->next_deblock_addr[dec->currPic] == t->first_mb_in_slice &&
		dec->nal_ref_idc) ? dec->currPic : -1;
	t->next_deblock_addr = (dec->next_deblock_addr[dec->currPic] == t->first_mb_in_slice ||
		t->disable_deblocking_filter_idc == 2) ? t->first_mb_in_slice : INT_MIN;
	t->long_term_flags = dec->long_term_flags;
	t->samples_base = dec->frame_buffers[dec->currPic];
	t->samples_clip_v[0] = set16((1 << dec->sps.BitDepth_Y) - 1);
	t->samples_clip_v[1] = t->samples_clip_v[2] = set16((1 << dec->sps.BitDepth_C) - 1);
	
	// P/B slices
	if (t->slice_type < 2) {
		memcpy(t->frame_buffers, dec->frame_buffers, sizeof(t->frame_buffers));
		if (t->slice_type == 1 && (t->pps.weighted_bipred_idc == 2 || !t->direct_spatial_mv_pred_flag)) {
			i32x4 poc = set32(min(dec->TopFieldOrderCnt, dec->BottomFieldOrderCnt));
			t->diff_poc_v[0] = packs32(poc - min32(dec->FieldOrderCnt_v[0][0], dec->FieldOrderCnt_v[1][0]),
			                           poc - min32(dec->FieldOrderCnt_v[0][1], dec->FieldOrderCnt_v[1][1]));
			t->diff_poc_v[1] = packs32(poc - min32(dec->FieldOrderCnt_v[0][2], dec->FieldOrderCnt_v[1][2]),
			                           poc - min32(dec->FieldOrderCnt_v[0][3], dec->FieldOrderCnt_v[1][3]));
			t->diff_poc_v[2] = packs32(poc - min32(dec->FieldOrderCnt_v[0][4], dec->FieldOrderCnt_v[1][4]),
			                           poc - min32(dec->FieldOrderCnt_v[0][5], dec->FieldOrderCnt_v[1][5]));
			t->diff_poc_v[3] = packs32(poc - min32(dec->FieldOrderCnt_v[0][6], dec->FieldOrderCnt_v[1][6]),
			                           poc - min32(dec->FieldOrderCnt_v[0][7], dec->FieldOrderCnt_v[1][7]));
		}
	}
}



/**
 * This function matches slice_header() in 7.3.3, which it parses while updating
 * the DPB and initialising slice data for further decoding.
 */
int ADD_VARIANT(parse_slice_layer_without_partitioning)(Edge264Decoder *dec, int non_blocking, void(*free_cb)(void*,int), void *free_arg)
{
	#if EDGE264_TRACE
	static const char * const slice_type_names[5] = {"P", "B", "I", "SP", "SI"};
	static const char * const disable_deblocking_filter_idc_names[3] = {"enabled", "disabled", "disabled across slices"};
	#endif
	
	// reserving a slot without locking is fine since workers can only unset busy_tasks
	unsigned avail_tasks;
	while (!(avail_tasks = 0xffff & ~dec->busy_tasks)) {
		if (non_blocking)
			return EWOULDBLOCK;
		pthread_cond_wait(&dec->task_complete, &dec->lock);
	}
	Edge264Task *t = dec->tasks + __builtin_ctz(avail_tasks);
	t->free_cb = free_cb;
	t->free_arg = free_arg;
	
	// first important fields and checks before decoding the slice header
	if (!dec->_gb.lsb_cache)
		refill(&dec->_gb, 0);
	t->first_mb_in_slice = get_ue32(&dec->_gb, 139263);
	int slice_type = get_ue16(&dec->_gb, 9);
	t->slice_type = (slice_type < 5) ? slice_type : slice_type - 5;
	int pic_parameter_set_id = get_ue16(&dec->_gb, 255);
	print_header(dec, "<k>first_mb_in_slice</k><v>%u</v>\n"
		"<k>slice_type</k><v%s>%u (%s)</v>\n"
		"<k>pic_parameter_set_id</k><v%s>%u</v>\n",
		t->first_mb_in_slice,
		red_if(t->slice_type > 2), slice_type, slice_type_names[t->slice_type],
		red_if(pic_parameter_set_id >= 4 || dec->PPS[pic_parameter_set_id].num_ref_idx_active[0] == 0), pic_parameter_set_id);
	if (t->slice_type > 2 || pic_parameter_set_id >= 4)
		return ENOTSUP;
	t->pps = dec->PPS[pic_parameter_set_id];
	if (t->pps.num_ref_idx_active[0] == 0) // if PPS wasn't initialized
		return EBADMSG;
	
	// parse frame_num
	int frame_num = get_uv(&dec->_gb, dec->sps.log2_max_frame_num);
	int FrameNumMask = (1 << dec->sps.log2_max_frame_num) - 1;
	if (dec->currPic >= 0 && frame_num != (dec->FrameNum & FrameNumMask))
		finish_frame(dec, 0);
	int non_base_view = 1;
	if (dec->nal_unit_type != 20) {
		dec->IdrPicFlag = dec->nal_unit_type == 5;
		non_base_view = 0;
	}
	unsigned view_mask = ((dec->sps.mvc - 1) | 0x55555555 << non_base_view) & ((1 << dec->sps.num_frame_buffers) - 1);
	int prevRefFrameNum = dec->IdrPicFlag ? 0 : dec->prevRefFrameNum[non_base_view];
	dec->FrameNum = prevRefFrameNum + ((frame_num - prevRefFrameNum) & FrameNumMask);
	print_header(dec, "<k>frame_num => FrameNum</k><v>%u => %u</v>\n", frame_num, dec->FrameNum);
	
	// Check for gaps in frame_num (8.2.5.2)
	int gap = dec->FrameNum - prevRefFrameNum;
	if (__builtin_expect(gap > 1, 0)) {
		// make enough non-referenced slots by dereferencing frames
		int max_num_ref_frames = dec->sps.max_num_ref_frames >> dec->sps.mvc;
		int non_existing = min(gap - 1, max_num_ref_frames - __builtin_popcount(dec->long_term_flags & view_mask));
		for (int excess = __builtin_popcount(view_mask & dec->reference_flags) + non_existing - max_num_ref_frames; excess > 0; excess--) {
			int unref, poc = INT_MAX;
			for (unsigned r = view_mask & dec->reference_flags & ~dec->long_term_flags; r; r &= r - 1) {
				int i = __builtin_ctz(r);
				if (dec->FrameNums[i] < poc)
					poc = dec->FrameNums[unref = i];
			}
			dec->reference_flags &= ~(1 << unref);
		}
		// make enough non-outputable slots by raising dispPicOrderCnt
		unsigned output_flags = dec->output_flags;
		for (int excess = non_existing - __builtin_popcount(view_mask & ~dec->reference_flags & ~output_flags); excess > 0; excess--) {
			int disp, poc = INT_MAX;
			for (unsigned o = view_mask & ~dec->reference_flags & output_flags; o; o &= o - 1) {
				int i = __builtin_ctz(o);
				if (dec->FieldOrderCnt[0][i] < poc)
					poc = dec->FieldOrderCnt[0][disp = i];
			}
			output_flags &= ~(1 << disp);
			dec->dispPicOrderCnt = max(dec->dispPicOrderCnt, poc);
		}
		// wait until enough of the slots we freed are undepended
		unsigned avail;
		while (__builtin_popcount(avail = view_mask & ~dec->reference_flags & ~output_flags & ~depended_frames(dec)) < non_existing) {
			if (non_blocking)
				return EWOULDBLOCK;
			pthread_cond_wait(&dec->task_complete, &dec->lock);
		}
		// stop here if we must wait for get_frame to consume and return enough frames
		avail &= ~dec->borrow_flags;
		if (output_flags != dec->output_flags || __builtin_popcount(avail) < non_existing)
			return ENOBUFS;
		// finally insert the last non-existing frames one by one
		for (unsigned FrameNum = dec->FrameNum - non_existing; FrameNum < dec->FrameNum; FrameNum++) {
			int i = __builtin_ctz(avail);
			avail ^= 1 << i;
			dec->reference_flags |= 1 << i;
			dec->FrameNums[i] = FrameNum;
			int PicOrderCnt = 0;
			if (dec->sps.pic_order_cnt_type == 2) {
				PicOrderCnt = FrameNum * 2;
			} else if (dec->sps.num_ref_frames_in_pic_order_cnt_cycle > 0) {
				PicOrderCnt = (FrameNum / dec->sps.num_ref_frames_in_pic_order_cnt_cycle) *
					dec->sps.PicOrderCntDeltas[dec->sps.num_ref_frames_in_pic_order_cnt_cycle] +
					dec->sps.PicOrderCntDeltas[FrameNum % dec->sps.num_ref_frames_in_pic_order_cnt_cycle];
			}
			dec->FieldOrderCnt[0][i] = dec->FieldOrderCnt[1][i] = PicOrderCnt;
			if (dec->frame_buffers[i] == NULL && alloc_frame(dec, i))
				return ENOMEM;
		}
		dec->prevRefFrameNum[non_base_view] = dec->FrameNum - 1;
	}
	if (dec->nal_ref_idc)
		dec->prevRefFrameNum[non_base_view] = dec->FrameNum;
	
	// As long as PAFF/MBAFF are unsupported, this code won't execute (but is still kept).
	t->field_pic_flag = 0;
	t->bottom_field_flag = 0;
	if (!dec->sps.frame_mbs_only_flag) {
		t->field_pic_flag = get_u1(&dec->_gb);
		print_header(dec, "<k>field_pic_flag</k><v>%x</v>\n", t->field_pic_flag);
		if (t->field_pic_flag) {
			t->bottom_field_flag = get_u1(&dec->_gb);
			print_header(dec, "<k>bottom_field_flag</k><v>%x</v>\n",
				t->bottom_field_flag);
		}
	}
	t->MbaffFrameFlag = dec->sps.mb_adaptive_frame_field_flag & ~t->field_pic_flag;
	
	// I did not get the point of idr_pic_id yet.
	if (dec->IdrPicFlag) {
		int idr_pic_id = get_ue32(&dec->_gb, 65535);
		print_header(dec, "<k>idr_pic_id</k><v>%u</v>\n", idr_pic_id);
	}
	
	// Compute Top/BottomFieldOrderCnt (8.2.1).
	if (dec->sps.pic_order_cnt_type == 0) {
		int pic_order_cnt_lsb = get_uv(&dec->_gb, dec->sps.log2_max_pic_order_cnt_lsb);
		int shift = WORD_BIT - dec->sps.log2_max_pic_order_cnt_lsb;
		if (dec->currPic >= 0 && pic_order_cnt_lsb != ((unsigned)dec->FieldOrderCnt[0][dec->currPic] << shift >> shift))
			finish_frame(dec, 0);
		int prevPicOrderCnt = dec->IdrPicFlag ? 0 : dec->prevPicOrderCnt;
		int inc = (pic_order_cnt_lsb - prevPicOrderCnt) << shift >> shift;
		dec->TopFieldOrderCnt = prevPicOrderCnt + inc;
		int delta_pic_order_cnt_bottom = 0;
		if (t->pps.bottom_field_pic_order_in_frame_present_flag && !t->field_pic_flag)
			delta_pic_order_cnt_bottom = get_se32(&dec->_gb, (-1u << 31) + 1, (1u << 31) - 1);
		dec->BottomFieldOrderCnt = dec->TopFieldOrderCnt + delta_pic_order_cnt_bottom;
		print_header(dec, "<k>pic_order_cnt_lsb/delta_pic_order_cnt_bottom => Top/Bottom POC</k><v>%u/%d => %d/%d</v>\n",
			pic_order_cnt_lsb, delta_pic_order_cnt_bottom, dec->TopFieldOrderCnt, dec->BottomFieldOrderCnt);
	} else if (dec->sps.pic_order_cnt_type == 1) {
		unsigned absFrameNum = dec->FrameNum + (dec->nal_ref_idc != 0) - 1;
		dec->TopFieldOrderCnt = (dec->nal_ref_idc) ? 0 : dec->sps.offset_for_non_ref_pic;
		if (dec->sps.num_ref_frames_in_pic_order_cnt_cycle > 0) {
			dec->TopFieldOrderCnt += (absFrameNum / dec->sps.num_ref_frames_in_pic_order_cnt_cycle) *
				dec->sps.PicOrderCntDeltas[dec->sps.num_ref_frames_in_pic_order_cnt_cycle] +
				dec->sps.PicOrderCntDeltas[absFrameNum % dec->sps.num_ref_frames_in_pic_order_cnt_cycle];
		}
		int delta_pic_order_cnt0 = 0, delta_pic_order_cnt1 = 0;
		if (!dec->sps.delta_pic_order_always_zero_flag) {
			delta_pic_order_cnt0 = get_se32(&dec->_gb, (-1u << 31) + 1, (1u << 31) - 1);
			if (t->pps.bottom_field_pic_order_in_frame_present_flag && !t->field_pic_flag)
				delta_pic_order_cnt1 = get_se32(&dec->_gb, (-1u << 31) + 1, (1u << 31) - 1);
		}
		dec->TopFieldOrderCnt += delta_pic_order_cnt0;
		if (dec->currPic >= 0 && dec->TopFieldOrderCnt != dec->FieldOrderCnt[0][dec->currPic])
			finish_frame(dec, 0);
		dec->BottomFieldOrderCnt = dec->TopFieldOrderCnt + delta_pic_order_cnt1;
		print_header(dec, "<k>delta_pic_order_cnt[0/1] => Top/Bottom POC</k><v>%d/%d => %d/%d</v>\n", delta_pic_order_cnt0, delta_pic_order_cnt1, dec->TopFieldOrderCnt, dec->BottomFieldOrderCnt);
	} else {
		dec->TopFieldOrderCnt = dec->BottomFieldOrderCnt = dec->FrameNum * 2 + (dec->nal_ref_idc != 0) - 1;
		print_header(dec, "<k>PicOrderCnt</k><v>%d</v>\n", dec->TopFieldOrderCnt);
	}
	if (abs(dec->TopFieldOrderCnt) >= 1 << 25 || abs(dec->BottomFieldOrderCnt) >= 1 << 25)
		return ENOTSUP;
	
	// find and possibly allocate a DPB slot for the upcoming frame
	if (dec->currPic >= 0 && non_base_view != (dec->currPic & dec->sps.mvc))
		finish_frame(dec, 1);
	int is_first_slice = 0;
	if (dec->currPic < 0) {
		// get a mask of free slots or find the next to be released by get_frame
		unsigned avail = view_mask & ~dec->reference_flags & ~dec->output_flags, ready;
		if (!avail) {
			int poc = INT_MAX;
			for (unsigned o = view_mask & ~dec->reference_flags & ~avail; o; o &= o - 1) {
				int i = __builtin_ctz(o);
				if (dec->FieldOrderCnt[0][i] < poc) {
					poc = dec->FieldOrderCnt[0][i];
					avail = 1 << i;
				}
			}
		}
		// wait until at least one of these slots is undepended
		while (!(ready = avail & ~depended_frames(dec))) {
			if (non_blocking)
				return EWOULDBLOCK;
			pthread_cond_wait(&dec->task_complete, &dec->lock);
		}
		// stop here if we must wait for get_frame to consume and return a non-ref frame
		if (ready & (dec->output_flags | dec->borrow_flags))
			return ENOBUFS;
		dec->currPic = __builtin_ctz(ready);
		if (dec->frame_buffers[dec->currPic] == NULL && alloc_frame(dec, dec->currPic))
			return ENOMEM;
		dec->frame_flip_bits ^= 1 << dec->currPic;
		dec->remaining_mbs[dec->currPic] = dec->sps.pic_width_in_mbs * dec->sps.pic_height_in_mbs;
		dec->next_deblock_addr[dec->currPic] = 0;
		dec->FrameNums[dec->currPic] = dec->FrameNum;
		dec->FieldOrderCnt[0][dec->currPic] = dec->TopFieldOrderCnt;
		dec->FieldOrderCnt[1][dec->currPic] = dec->BottomFieldOrderCnt;
		is_first_slice = 1;
	}
	
	// The first test could be optimised into a fast bit test, but would be less readable :)
	t->RefPicList_v[0] = t->RefPicList_v[1] = t->RefPicList_v[2] = t->RefPicList_v[3] =
		(i8x16){-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
	dec->pic_reference_flags = dec->reference_flags & view_mask;
	dec->pic_long_term_flags = dec->long_term_flags & view_mask;
	dec->pic_LongTermFrameIdx_v[0] = dec->LongTermFrameIdx_v[0];
	dec->pic_LongTermFrameIdx_v[1] = dec->LongTermFrameIdx_v[1];
	if (t->slice_type == 0 || t->slice_type == 1) {
		if (t->slice_type == 1) {
			t->direct_spatial_mv_pred_flag = get_u1(&dec->_gb);
			print_header(dec, "<k>direct_spatial_mv_pred_flag</k><v>%x</v>\n",
				t->direct_spatial_mv_pred_flag);
		}
		
		// num_ref_idx_active_override_flag
		int lim = 16 << t->field_pic_flag >> dec->sps.mvc;
		if (get_u1(&dec->_gb)) {
			for (int l = 0; l <= t->slice_type; l++)
				t->pps.num_ref_idx_active[l] = get_ue16(&dec->_gb, lim - 1) + 1;
			print_header(dec, t->slice_type ? "<k>num_ref_idx_active</k><v>%u, %u</v>\n": "<k>num_ref_idx_active</k><v>%u</v>\n",
				t->pps.num_ref_idx_active[0], t->pps.num_ref_idx_active[1]);
		} else {
			t->pps.num_ref_idx_active[0] = min(t->pps.num_ref_idx_active[0], lim);
			t->pps.num_ref_idx_active[1] = min(t->pps.num_ref_idx_active[1], lim);
			print_header(dec, t->slice_type ? "<k>num_ref_idx_active (inferred)</k><v>%u, %u</v>\n": "<k>num_ref_idx_active (inferred)</k><v>%u</v>\n",
				t->pps.num_ref_idx_active[0], t->pps.num_ref_idx_active[1]);
		}
		
		parse_ref_pic_list_modification(dec, t);
		parse_pred_weight_table(dec, t);
	}
	
	if (dec->nal_ref_idc)
		parse_dec_ref_pic_marking(dec);
	
	t->cabac_init_idc = 0;
	if (t->pps.entropy_coding_mode_flag && t->slice_type != 2) {
		t->cabac_init_idc = 1 + get_ue16(&dec->_gb, 2);
		print_header(dec, "<k>cabac_init_idc</k><v>%u</v>\n", t->cabac_init_idc - 1);
	}
	t->QP[0] = t->pps.QPprime_Y + get_se16(&dec->_gb, -t->pps.QPprime_Y, 51 - t->pps.QPprime_Y); // FIXME QpBdOffset
	print_header(dec, "<k>SliceQP<sub>Y</sub></k><v>%d</v>\n", t->QP[0]);
	
	if (t->pps.deblocking_filter_control_present_flag) {
		t->disable_deblocking_filter_idc = get_ue16(&dec->_gb, 2);
		print_header(dec, "<k>disable_deblocking_filter_idc</k><v>%x (%s)</v>\n",
			t->disable_deblocking_filter_idc, disable_deblocking_filter_idc_names[t->disable_deblocking_filter_idc]);
		if (t->disable_deblocking_filter_idc != 1) {
			t->FilterOffsetA = get_se16(&dec->_gb, -6, 6) * 2;
			t->FilterOffsetB = get_se16(&dec->_gb, -6, 6) * 2;
			print_header(dec, "<k>FilterOffsets</k><v>%d, %d</v>\n",
				t->FilterOffsetA, t->FilterOffsetB);
		}
	} else {
		t->disable_deblocking_filter_idc = 0;
		t->FilterOffsetA = 0;
		t->FilterOffsetB = 0;
		print_header(dec, "<k>disable_deblocking_filter_idc (inferred)</k><v>0 (enabled)</v>\n"
			"<k>FilterOffsets (inferred)</k><v>0, 0</v>\n");
	}
	
	// update output flags now that we know if mmco5 happened
	unsigned to_output = 1 << dec->currPic;
	if (is_first_slice) {
		if (!dec->sps.mvc || (to_output |= 1 << dec->basePic, dec->basePic >= 0)) {
			if (dec->IdrPicFlag) {
				dec->dispPicOrderCnt = -(1 << 25);
				for (unsigned o = dec->output_flags; o; o &= o - 1) {
					int i = __builtin_ctz(o);
					dec->FieldOrderCnt[0][i] -= 1 << 26;
					dec->FieldOrderCnt[1][i] -= 1 << 26;
				}
			}
			dec->output_flags |= to_output;
			if (!((dec->pic_reference_flags | dec->reference_flags & ~view_mask) & to_output))
				dec->dispPicOrderCnt = dec->FieldOrderCnt[0][dec->currPic]; // make all frames with lower POCs ready for output
		}
		#ifdef TRACE
			print_header(dec, "<k>updated DPB (FrameNum/PicOrderCnt)</k><v><small>");
			for (int i = 0; i < dec->sps.num_frame_buffers; i++) {
				int r = (dec->pic_reference_flags | dec->reference_flags & ~view_mask) & 1 << i;
				int l = (dec->pic_long_term_flags | dec->long_term_flags & ~view_mask) & 1 << i;
				int o = dec->output_flags & 1 << i;
				print_header(dec, l ? "<sup>%u</sup>/" : r ? "%u/" : "_/", l ? dec->pic_LongTermFrameIdx[i] : dec->FrameNums[i]);
				print_header(dec, o ? "<b>%u</b>" : r ? "%u" : "_", min(dec->FieldOrderCnt[0][i], dec->FieldOrderCnt[1][i]) << 6 >> 6);
				print_header(dec, (i < dec->sps.num_frame_buffers - 1) ? ", " : "</small></v>\n");
			}
		#endif
	}
	
	// prepare the task and signal it
	initialize_task(dec, t);
	int task_id = t - dec->tasks;
	dec->busy_tasks |= 1 << task_id;
	dec->pending_tasks |= 1 << task_id;
	dec->task_dependencies[task_id] = refs_to_mask(t);
	dec->ready_tasks |= ((dec->task_dependencies[task_id] & ~ready_frames(dec)) == 0) << task_id;
	dec->taskPics[task_id] = dec->currPic;
	if (!dec->n_threads)
		return (intptr_t)ADD_VARIANT(worker_loop)(dec);
	pthread_cond_signal(&dec->task_ready);
	return 0;
}



int ADD_VARIANT(parse_nal_unit_header_extension)(Edge264Decoder *dec, int non_blocking, void(*free_cb)(void*,int), void *free_arg) {
	refill(&dec->_gb, 0);
	unsigned u = get_uv(&dec->_gb, 24);
	if (u & 1 << 23)
		return ENOTSUP;
	dec->IdrPicFlag = u >> 22 & 1 ^ 1;
	print_header(dec, "<k>non_idr_flag</k><v>%x</v>\n"
		"<k>priority_id</k><v>%d</v>\n"
		"<k>view_id</k><v>%d</v>\n"
		"<k>temporal_id</k><v>%d</v>\n"
		"<k>anchor_pic_flag</k><v>%x</v>\n"
		"<k>inter_view_flag</k><v>%x</v>\n",
		u >> 22 & 1,
		u >> 16 & 0x3f,
		u >> 6 & 0x3ff,
		u >> 3 & 7,
		u >> 2 & 1,
		u >> 1 & 1);
	if (dec->nal_unit_type == 20)
		return ADD_VARIANT(parse_slice_layer_without_partitioning)(dec, non_blocking, free_cb, free_arg);
	// spec doesn't mention rbsp_trailing_bits at the end of prefix_nal_unit_rbsp
	if (dec->_gb.msb_cache != 0 || (dec->_gb.lsb_cache & (dec->_gb.lsb_cache - 1)) || (intptr_t)(dec->_gb.end - dec->_gb.CPB) > 0)
		return EBADMSG;
	return 0;
}



/**
 * Parses the scaling lists into w4x4 and w8x8 (7.3.2.1 and Table 7-2).
 *
 * Fall-back rules for indices 0, 3, 6 and 7 are applied by keeping the
 * existing list, so they must be initialised with Default scaling lists at
 * the very first call.
 */
static void parse_scaling_lists(Edge264Decoder *dec, i8x16 *w4x4, i8x16 *w8x8, int transform_8x8_mode_flag, int chroma_format_idc)
{
	i8x16 fb4x4 = *w4x4; // fall-back
	i8x16 d4x4 = Default_4x4_Intra; // for useDefaultScalingMatrixFlag
	for (int i = 0; i < 6; i++, w4x4++) {
		if (i == 3) {
			fb4x4 = *w4x4;
			d4x4 = Default_4x4_Inter;
		}
		if (!get_u1(&dec->_gb)) { // scaling_list_present_flag
			*w4x4 = fb4x4;
		} else {
			unsigned nextScale = 8 + get_se16(&dec->_gb, -128, 127);
			if (nextScale == 0) {
				*w4x4 = fb4x4 = d4x4;
			} else {
				for (unsigned j = 0, lastScale;;) {
					((uint8_t *)w4x4)[((int8_t *)scan_4x4)[j]] = nextScale ?: lastScale;
					if (++j >= 16)
						break;
					if (nextScale != 0) {
						lastScale = nextScale;
						nextScale = (nextScale + get_se16(&dec->_gb, -128, 127)) & 255;
					}
				}
				fb4x4 = *w4x4;
			}
		}
	}
	
	// For 8x8 scaling lists, we really have no better choice than pointers.
	if (!transform_8x8_mode_flag)
		return;
	for (int i = 0; i < (chroma_format_idc == 3 ? 6 : 2); i++, w8x8 += 4) {
		if (!get_u1(&dec->_gb)) {
			if (i >= 2) {
				w8x8[0] = w8x8[-8];
				w8x8[1] = w8x8[-7];
				w8x8[2] = w8x8[-6];
				w8x8[3] = w8x8[-5];
			}
		} else {
			unsigned nextScale = 8 + get_se16(&dec->_gb, -128, 127);
			if (nextScale == 0) {
				const i8x16 *d8x8 = (i % 2 == 0) ? Default_8x8_Intra : Default_8x8_Inter;
				w8x8[0] = d8x8[0];
				w8x8[1] = d8x8[1];
				w8x8[2] = d8x8[2];
				w8x8[3] = d8x8[3];
			} else {
				for (unsigned j = 0, lastScale;;) {
					((uint8_t *)w8x8)[((int8_t *)scan_8x8_cabac)[j]] = nextScale ?: lastScale;
					if (++j >= 64)
						break;
					if (nextScale != 0) {
						lastScale = nextScale;
						nextScale = (nextScale + get_se16(&dec->_gb, -128, 127)) & 255;
					}
				}
			}
		}
	}
}



/**
 * Parses the PPS into a copy of the current SPS, then saves it into one of four
 * PPS slots if a rbsp_trailing_bits pattern follows.
 */
int ADD_VARIANT(parse_pic_parameter_set)(Edge264Decoder *dec, int non_blocking,  void(*free_cb)(void*,int), void *free_arg)
{
	#if EDGE264_TRACE
	static const char * const slice_group_map_type_names[7] = {"interleaved",
		"dispersed", "foreground with left-over", "box-out", "raster scan",
		"wipe", "explicit"};
	static const char * const weighted_pred_names[3] = {"average", "explicit", "implicit"};
	#endif
	
	// temp storage, committed if entire NAL is correct
	Edge264PicParameterSet pps;
	pps.transform_8x8_mode_flag = 0;
	for (int i = 0; i < 6; i++)
		pps.weightScale4x4_v[i] = dec->sps.weightScale4x4_v[i];
	for (int i = 0; i < 24; i++)
		pps.weightScale8x8_v[i] = dec->sps.weightScale8x8_v[i];
	
	// Actual streams never use more than 4 PPSs (I, P, B, b).
	refill(&dec->_gb, 0);
	int pic_parameter_set_id = get_ue16(&dec->_gb, 255);
	int seq_parameter_set_id = get_ue16(&dec->_gb, 31);
	pps.entropy_coding_mode_flag = get_u1(&dec->_gb);
	pps.bottom_field_pic_order_in_frame_present_flag = get_u1(&dec->_gb);
	int num_slice_groups = get_ue16(&dec->_gb, 7) + 1;
	print_header(dec, "<k>pic_parameter_set_id</k><v%s>%u</v>\n"
		"<k>seq_parameter_set_id</k><v>%u</v>\n"
		"<k>entropy_coding_mode_flag</k><v>%x</v>\n"
		"<k>bottom_field_pic_order_in_frame_present_flag</k><v>%x</v>\n"
		"<k>num_slice_groups</k><v%s>%u</v>\n",
		red_if(pic_parameter_set_id >= 4), pic_parameter_set_id,
		seq_parameter_set_id,
		pps.entropy_coding_mode_flag,
		pps.bottom_field_pic_order_in_frame_present_flag,
		red_if(num_slice_groups > 1), num_slice_groups);
	
	// Let's be nice enough to print the headers for unsupported stuff.
	if (num_slice_groups > 1) {
		int slice_group_map_type = get_ue16(&dec->_gb, 6);
		print_header(dec, "<k>slice_group_map_type</k><v>%u (%s)</v>\n",
			slice_group_map_type, slice_group_map_type_names[slice_group_map_type]);
		switch (slice_group_map_type) {
		case 0:
			for (int iGroup = 0; iGroup < num_slice_groups; iGroup++) {
				int run_length = get_ue32(&dec->_gb, 139263) + 1; // level 6.2
				print_header(dec, "<k>run_length[%u]</k><v>%u</v>\n",
					iGroup, run_length);
			}
			break;
		case 2:
			for (int iGroup = 0; iGroup < num_slice_groups; iGroup++) {
				int top_left = get_ue32(&dec->_gb, 139264);
				int bottom_right = get_ue32(&dec->_gb, 139264);
				print_header(dec, "<k>top_left[%u]</k><v>%u</v>\n"
					"<k>bottom_right[%u]</k><v>%u</v>\n",
					iGroup, top_left,
					iGroup, bottom_right);
			}
			break;
		case 3 ... 5: {
			int slice_group_change_direction_flag = get_u1(&dec->_gb);
			int SliceGroupChangeRate = get_ue32(&dec->_gb, 139263) + 1;
			print_header(dec, "<k>slice_group_change_direction_flag</k><v>%x</v>\n"
				"<k>SliceGroupChangeRate</k><v>%u</v>\n",
				slice_group_change_direction_flag,
				SliceGroupChangeRate);
			} break;
		case 6: {
			int PicSizeInMapUnits = get_ue32(&dec->_gb, 139263) + 1;
			print_header(dec, "<k>slice_group_ids</k><v>");
			for (int i = 0; i < PicSizeInMapUnits; i++) {
				int slice_group_id = get_uv(&dec->_gb, WORD_BIT - __builtin_clz(num_slice_groups - 1));
				print_header(dec, "%u ", slice_group_id);
			}
			print_header(dec, "</v>\n");
			} break;
		}
	}
	
	// (num_ref_idx_active[0] != 0) is used as indicator that the PPS is initialised.
	pps.num_ref_idx_active[0] = get_ue16(&dec->_gb, 31) + 1;
	pps.num_ref_idx_active[1] = get_ue16(&dec->_gb, 31) + 1;
	pps.weighted_pred_flag = get_u1(&dec->_gb);
	pps.weighted_bipred_idc = get_uv(&dec->_gb, 2);
	pps.QPprime_Y = get_se16(&dec->_gb, -26, 25) + 26; // FIXME QpBdOffset
	int pic_init_qs = get_se16(&dec->_gb, -26, 25) + 26;
	pps.second_chroma_qp_index_offset = pps.chroma_qp_index_offset = get_se16(&dec->_gb, -12, 12);
	pps.deblocking_filter_control_present_flag = get_u1(&dec->_gb);
	pps.constrained_intra_pred_flag = get_u1(&dec->_gb);
	int redundant_pic_cnt_present_flag = get_u1(&dec->_gb);
	print_header(dec, "<k>num_ref_idx_default_active</k><v>%u, %u</v>\n"
		"<k>weighted_pred</k><v>%x (%s), %x (%s)</v>\n"
		"<k>pic_init_qp</k><v>%u</v>\n"
		"<k>pic_init_qs</k><v>%u</v>\n"
		"<k>chroma_qp_index_offset</k><v>%d</v>\n"
		"<k>deblocking_filter_control_present_flag</k><v>%x</v>\n"
		"<k>constrained_intra_pred_flag</k><v%s>%x</v>\n"
		"<k>redundant_pic_cnt_present_flag</k><v%s>%x</v>\n",
		pps.num_ref_idx_active[0], pps.num_ref_idx_active[1],
		pps.weighted_pred_flag, weighted_pred_names[pps.weighted_pred_flag], pps.weighted_bipred_idc, weighted_pred_names[pps.weighted_bipred_idc],
		pps.QPprime_Y,
		pic_init_qs,
		pps.chroma_qp_index_offset,
		pps.deblocking_filter_control_present_flag,
		red_if(pps.constrained_intra_pred_flag), pps.constrained_intra_pred_flag,
		red_if(redundant_pic_cnt_present_flag), redundant_pic_cnt_present_flag);
	
	// short for peek-24-bits-without-having-to-define-a-single-use-function
	if (dec->_gb.msb_cache != (size_t)1 << (SIZE_BIT - 1) || (dec->_gb.lsb_cache & (dec->_gb.lsb_cache - 1)) || (intptr_t)(dec->_gb.end - dec->_gb.CPB) > 0) {
		pps.transform_8x8_mode_flag = get_u1(&dec->_gb);
		print_header(dec, "<k>transform_8x8_mode_flag</k><v>%x</v>\n",
			pps.transform_8x8_mode_flag);
		if (get_u1(&dec->_gb)) {
			parse_scaling_lists(dec, pps.weightScale4x4_v, pps.weightScale8x8_v, pps.transform_8x8_mode_flag, dec->sps.chroma_format_idc);
			print_header(dec, "<k>ScalingList4x4</k><v><small>");
			for (int i = 0; i < 6; i++) {
				for (int j = 0; j < 16; j++)
					print_header(dec, "%u%s", pps.weightScale4x4[i][((int8_t *)scan_4x4)[j]], (j < 15) ? ", " : (i < 5) ? "<br>" : "</small></v>\n");
			}
			print_header(dec, "<k>ScalingList8x8</k><v><small>");
			for (int i = 0; i < (dec->sps.chroma_format_idc < 3 ? 2 : 6); i++) {
				for (int j = 0; j < 64; j++)
					print_header(dec, "%u%s", pps.weightScale8x8[i][((int8_t *)scan_8x8_cabac)[j]], (j < 63) ? ", " : (i < (dec->sps.chroma_format_idc < 3 ? 1 : 5)) ? "<br>" : "</small></v>\n");
			}
		}
		pps.second_chroma_qp_index_offset = get_se16(&dec->_gb, -12, 12);
		print_header(dec, "<k>second_chroma_qp_index_offset</k><v>%d</v>\n",
			pps.second_chroma_qp_index_offset);
	} else {
		print_header(dec, "<k>transform_8x8_mode_flag (inferred)</k><v>0</v>\n"
			"<k>second_chroma_qp_index_offset (inferred)</k><v>%d</v>\n",
			pps.second_chroma_qp_index_offset);
	}
	
	// check for trailing_bits before unsupported features (in case errors enabled them)
	if (dec->_gb.msb_cache != (size_t)1 << (SIZE_BIT - 1) || (dec->_gb.lsb_cache & (dec->_gb.lsb_cache - 1)) || (intptr_t)(dec->_gb.end - dec->_gb.CPB) > 0)
		return EBADMSG;
	if (pic_parameter_set_id >= 4 || num_slice_groups > 1 ||
		pps.constrained_intra_pred_flag || redundant_pic_cnt_present_flag)
		return ENOTSUP;
	if (dec->sps.DPB_format != 0)
		dec->PPS[pic_parameter_set_id] = pps;
	return 0;
}



/**
 * For the sake of implementation simplicity, the responsibility for timing
 * management is left to demuxing libraries, hence any HRD data is ignored.
 */
static void parse_hrd_parameters(Edge264Decoder *dec) {
	int cpb_cnt = get_ue16(&dec->_gb, 31) + 1;
	int bit_rate_scale = get_uv(&dec->_gb, 4);
	int cpb_size_scale = get_uv(&dec->_gb, 4);
	print_header(dec, "<k>cpb_cnt</k><v>%u</v>\n"
		"<k>bit_rate_scale</k><v>%u</v>\n"
		"<k>cpb_size_scale</k><v>%u</v>\n",
		cpb_cnt,
		bit_rate_scale,
		cpb_size_scale);
	for (int i = 0; i < cpb_cnt; i++) {
		unsigned bit_rate_value = get_ue32(&dec->_gb, 4294967294) + 1;
		unsigned cpb_size_value = get_ue32(&dec->_gb, 4294967294) + 1;
		int cbr_flag = get_u1(&dec->_gb);
		print_header(dec, "<k>bit_rate_value[%u]</k><v>%u</v>\n"
			"<k>cpb_size_value[%u]</k><v>%u</v>\n"
			"<k>cbr_flag[%u]</k><v>%x</v>\n",
			i, bit_rate_value,
			i, cpb_size_value,
			i, cbr_flag);
	}
	unsigned delays = get_uv(&dec->_gb, 20);
	int initial_cpb_removal_delay_length = (delays >> 15) + 1;
	int cpb_removal_delay_length = ((delays >> 10) & 0x1f) + 1;
	int dpb_output_delay_length = ((delays >> 5) & 0x1f) + 1;
	int time_offset_length = delays & 0x1f;
	print_header(dec, "<k>initial_cpb_removal_delay_length</k><v>%u</v>\n"
		"<k>cpb_removal_delay_length</k><v>%u</v>\n"
		"<k>dpb_output_delay_length</k><v>%u</v>\n"
		"<k>time_offset_length</k><v>%u</v>\n",
		initial_cpb_removal_delay_length,
		cpb_removal_delay_length,
		dpb_output_delay_length,
		time_offset_length);
}



/**
 * To avoid cluttering the memory layout with unused data, VUI parameters are
 * mostly ignored until explicitly asked in the future.
 */
static void parse_vui_parameters(Edge264Decoder *dec, Edge264SeqParameterSet *sps)
{
	static const unsigned ratio2sar[32] = {0, 0x00010001, 0x000c000b,
		0x000a000b, 0x0010000b, 0x00280021, 0x0018000b, 0x0014000b, 0x0020000b,
		0x00500021, 0x0012000b, 0x000f000b, 0x00400021, 0x00a00063, 0x00040003,
		0x00030002, 0x00020001};
	#if EDGE264_TRACE
	static const char * const video_format_names[8] = {"Component", "PAL",
		"NTSC", "SECAM", "MAC", [5 ... 7] = "Unknown"};
	static const char * const colour_primaries_names[32] = {
		[0] = "Unknown",
		[1] = "ITU-R BT.709-5",
		[2 ... 3] = "Unknown",
		[4] = "ITU-R BT.470-6 System M",
		[5] = "ITU-R BT.470-6 System B, G",
		[6 ... 7] = "ITU-R BT.601-6 525",
		[8] = "Generic film",
		[9] = "ITU-R BT.2020-2",
		[10] = "CIE 1931 XYZ",
		[11] = "Society of Motion Picture and Television Engineers RP 431-2",
		[12] = "Society of Motion Picture and Television Engineers EG 432-1",
		[13 ... 21] = "Unknown",
		[22] = "EBU Tech. 3213-E",
		[23 ... 31] = "Unknown",
	};
	static const char * const transfer_characteristics_names[32] = {
		[0] = "Unknown",
		[1] = "ITU-R BT.709-5",
		[2 ... 3] = "Unknown",
		[4] = "ITU-R BT.470-6 System M",
		[5] = "ITU-R BT.470-6 System B, G",
		[6] = "ITU-R BT.601-6 525 or 625",
		[7] = "Society of Motion Picture and Television Engineers 240M",
		[8] = "Linear transfer characteristics",
		[9] = "Logarithmic transfer characteristic (100:1 range)",
		[10] = "Logarithmic transfer characteristic (100 * Sqrt( 10 ) : 1 range)",
		[11] = "IEC 61966-2-4",
		[12] = "ITU-R BT.1361-0",
		[13] = "IEC 61966-2-1 sRGB or sYCC",
		[14] = "ITU-R BT.2020-2 (10 bit system)",
		[15] = "ITU-R BT.2020-2 (12 bit system)",
		[16] = "Society of Motion Picture and Television Engineers ST 2084",
		[17] = "Society of Motion Picture and Television Engineers ST 428-1",
		[18 ... 31] = "Unknown",
	};
	static const char * const matrix_coefficients_names[16] = {
		[0] = "Unknown",
		[1] = "Kr = 0.2126; Kb = 0.0722",
		[2 ... 3] = "Unknown",
		[4] = "Kr = 0.30; Kb = 0.11",
		[5 ... 6] = "Kr = 0.299; Kb = 0.114",
		[7] = "Kr = 0.212; Kb = 0.087",
		[8] = "YCgCo",
		[9] = "Kr = 0.2627; Kb = 0.0593 (non-constant luminance)",
		[10] = "Kr = 0.2627; Kb = 0.0593 (constant luminance)",
		[11] = "Y'D'zD'x",
		[12 ... 15] = "Unknown",
	};
	#endif
	
	if (get_u1(&dec->_gb)) {
		int aspect_ratio_idc = get_uv(&dec->_gb, 8);
		unsigned sar = (aspect_ratio_idc == 255) ? get_uv(&dec->_gb, 32) : ratio2sar[aspect_ratio_idc & 31];
		int sar_width = sar >> 16;
		int sar_height = sar & 0xffff;
		print_header(dec, "<k>aspect_ratio</k><v>%u:%u</v>\n",
			sar_width, sar_height);
	}
	if (get_u1(&dec->_gb)) {
		int overscan_appropriate_flag = get_u1(&dec->_gb);
		print_header(dec, "<k>overscan_appropriate_flag</k><v>%x</v>\n",
			overscan_appropriate_flag);
	}
	if (get_u1(&dec->_gb)) {
		int video_format = get_uv(&dec->_gb, 3);
		int video_full_range_flag = get_u1(&dec->_gb);
		print_header(dec, "<k>video_format</k><v>%u (%s)</v>\n"
			"<k>video_full_range_flag</k><v>%x</v>\n",
			video_format, video_format_names[video_format],
			video_full_range_flag);
		if (get_u1(&dec->_gb)) {
			unsigned desc = get_uv(&dec->_gb, 24);
			int colour_primaries = desc >> 16;
			int transfer_characteristics = (desc >> 8) & 0xff;
			int matrix_coefficients = desc & 0xff;
			print_header(dec, "<k>colour_primaries</k><v>%u (%s)</v>\n"
				"<k>transfer_characteristics</k><v>%u (%s)</v>\n"
				"<k>matrix_coefficients</k><v>%u (%s)</v>\n",
				colour_primaries, colour_primaries_names[colour_primaries & 31],
				transfer_characteristics, transfer_characteristics_names[transfer_characteristics & 31],
				matrix_coefficients, matrix_coefficients_names[matrix_coefficients & 15]);
		}
	}
	if (get_u1(&dec->_gb)) {
		int chroma_sample_loc_type_top_field = get_ue16(&dec->_gb, 5);
		int chroma_sample_loc_type_bottom_field = get_ue16(&dec->_gb, 5);
		print_header(dec, "<k>chroma_sample_loc_type_top_field</k><v>%x</v>\n"
			"<k>chroma_sample_loc_type_bottom_field</k><v>%x</v>\n",
			chroma_sample_loc_type_top_field,
			chroma_sample_loc_type_bottom_field);
	}
	if (get_u1(&dec->_gb)) {
		unsigned num_units_in_tick = get_uv(&dec->_gb, 32);
		unsigned time_scale = get_uv(&dec->_gb, 32);
		int fixed_frame_rate_flag = get_u1(&dec->_gb);
		print_header(dec, "<k>num_units_in_tick</k><v>%u</v>\n"
			"<k>time_scale</k><v>%u</v>\n"
			"<k>fixed_frame_rate_flag</k><v>%x</v>\n",
			num_units_in_tick,
			time_scale,
			fixed_frame_rate_flag);
	}
	int nal_hrd_parameters_present_flag = get_u1(&dec->_gb);
	if (nal_hrd_parameters_present_flag)
		parse_hrd_parameters(dec);
	int vcl_hrd_parameters_present_flag = get_u1(&dec->_gb);
	if (vcl_hrd_parameters_present_flag)
		parse_hrd_parameters(dec);
	if (nal_hrd_parameters_present_flag | vcl_hrd_parameters_present_flag) {
		int low_delay_hrd_flag = get_u1(&dec->_gb);
		print_header(dec, "<k>low_delay_hrd_flag</k><v>%x</v>\n",
			low_delay_hrd_flag);
	}
	int pic_struct_present_flag = get_u1(&dec->_gb);
	print_header(dec, "<k>pic_struct_present_flag</k><v>%x</v>\n",
		pic_struct_present_flag);
	if (get_u1(&dec->_gb)) {
		int motion_vectors_over_pic_boundaries_flag = get_u1(&dec->_gb);
		int max_bytes_per_pic_denom = get_ue16(&dec->_gb, 16);
		int max_bits_per_mb_denom = get_ue16(&dec->_gb, 16);
		int log2_max_mv_length_horizontal = get_ue16(&dec->_gb, 16);
		int log2_max_mv_length_vertical = get_ue16(&dec->_gb, 16);
		// we don't enforce MaxDpbFrames here since violating the level is harmless
		sps->max_num_reorder_frames = get_ue16(&dec->_gb, 16);
		sps->num_frame_buffers = max(get_ue16(&dec->_gb, 16), max(sps->max_num_ref_frames, sps->max_num_reorder_frames)) + 1;
		print_header(dec, "<k>motion_vectors_over_pic_boundaries_flag</k><v>%x</v>\n"
			"<k>max_bytes_per_pic_denom</k><v>%u</v>\n"
			"<k>max_bits_per_mb_denom</k><v>%u</v>\n"
			"<k>max_mv_length_horizontal</k><v>%u</v>\n"
			"<k>max_mv_length_vertical</k><v>%u</v>\n"
			"<k>max_num_reorder_frames</k><v>%u</v>\n"
			"<k>max_dec_frame_buffering</k><v>%u</v>\n",
			motion_vectors_over_pic_boundaries_flag,
			max_bytes_per_pic_denom,
			max_bits_per_mb_denom,
			1 << log2_max_mv_length_horizontal,
			1 << log2_max_mv_length_vertical,
			sps->max_num_reorder_frames,
			sps->num_frame_buffers - 1);
	} else {
		print_header(dec, "<k>max_num_reorder_frames (inferred)</k><v>%u</v>\n"
			"<k>max_dec_frame_buffering (inferred)</k><v>%u</v>\n",
			sps->max_num_reorder_frames,
			sps->num_frame_buffers - 1);
	}
}



/**
 * Parses the MVC VUI parameters extension, only advancing the stream pointer
 * for error detection, and ignoring it until requested in the future.
 */
static void parse_mvc_vui_parameters_extension(Edge264Decoder *dec)
{
	for (int i = get_ue16(&dec->_gb, 1023); i-- >= 0;) {
		get_uv(&dec->_gb, 3);
		for (int j = get_ue16(&dec->_gb, 1023); j-- >= 0; get_ue16(&dec->_gb, 1023));
		if (get_u1(&dec->_gb)) {
			get_uv(&dec->_gb, 32);
			get_uv(&dec->_gb, 32);
			get_u1(&dec->_gb);
		}
		int vui_mvc_nal_hrd_parameters_present_flag = get_u1(&dec->_gb);
		if (vui_mvc_nal_hrd_parameters_present_flag)
			parse_hrd_parameters(dec);
		int vui_mvc_vcl_hrd_parameters_present_flag = get_u1(&dec->_gb);
		if (vui_mvc_vcl_hrd_parameters_present_flag)
			parse_hrd_parameters(dec);
		if (vui_mvc_nal_hrd_parameters_present_flag | vui_mvc_vcl_hrd_parameters_present_flag)
			get_u1(&dec->_gb);
	}
}



/**
 * Parses the SPS extension for MVC.
 */
static int parse_seq_parameter_set_mvc_extension(Edge264Decoder *dec, Edge264SeqParameterSet *sps, int profile_idc)
{
	// returning unsupported asap is more efficient than keeping tedious code afterwards
	int num_views = get_ue16(&dec->_gb, 1023) + 1;
	int view_id0 = get_ue16(&dec->_gb, 1023);
	int view_id1 = get_ue16(&dec->_gb, 1023);
	print_header(dec, "<k>num_views {view_id<sub>0</sub>, view_id<sub>1</sub>}</k><v%s>%u {%u, %u}</v>\n",
		red_if(num_views != 2), num_views, view_id0, view_id1);
	if (num_views != 2)
		return ENOTSUP;
	sps->mvc = 1;
	sps->max_num_ref_frames = min(sps->max_num_ref_frames * 2, 16);
	sps->max_num_reorder_frames = min(sps->max_num_reorder_frames * 2 + 1, 17);
	sps->num_frame_buffers = min(sps->num_frame_buffers * 2, 18);
	
	// inter-view refs are ignored since we always add them anyway
	int num_anchor_refs_l0 = get_ue16(&dec->_gb, 1);
	if (num_anchor_refs_l0)
		get_ue16(&dec->_gb, 1023);
	int num_anchor_refs_l1 = get_ue16(&dec->_gb, 1);
	if (num_anchor_refs_l1)
		get_ue16(&dec->_gb, 1023);
	int num_non_anchor_refs_l0 = get_ue16(&dec->_gb, 1);
	if (num_non_anchor_refs_l0)
		get_ue16(&dec->_gb, 1023);
	int num_non_anchor_refs_l1 = get_ue16(&dec->_gb, 1);
	if (num_non_anchor_refs_l1)
		get_ue16(&dec->_gb, 1023);
	print_header(dec, "<k>Inter-view refs in anchors/non-anchors</k><v>%u, %u / %u, %u</v>\n",
		num_anchor_refs_l0, num_anchor_refs_l1, num_non_anchor_refs_l0, num_non_anchor_refs_l1);
	
	// level values and operation points are similarly ignored
	print_header(dec, "<k>level_values_signalled</k><v>");
	int num_level_values_signalled = get_ue16(&dec->_gb, 63) + 1;
	for (int i = 0; i < num_level_values_signalled; i++) {
		int level_idc = get_uv(&dec->_gb, 8);
		print_header(dec, "%s%.1f", (i == 0) ? "" : ", ", (double)level_idc / 10);
		for (int j = get_ue16(&dec->_gb, 1023); j-- >= 0;) {
			get_uv(&dec->_gb, 3);
			for (int k = get_ue16(&dec->_gb, 1023); k-- >= 0; get_ue16(&dec->_gb, 1023));
			get_ue16(&dec->_gb, 1023);
		}
	}
	print_header(dec, "</v>\n");
	return profile_idc == 134 ? ENOTSUP : 0; // MFC is unsupported until streams actually use it
}



/**
 * Parses the SPS into a edge264_parameter_set structure, then saves it if a
 * rbsp_trailing_bits pattern follows.
 */
int ADD_VARIANT(parse_seq_parameter_set)(Edge264Decoder *dec, int non_blocking, void(*free_cb)(void*,int), void *free_arg)
{
	#if EDGE264_TRACE
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
	#endif
	static const uint32_t MaxDpbMbs[64] = {
		396, 396, 396, 396, 396, 396, 396, 396, 396, 396, 396, // level 1
		900, // levels 1b and 1.1
		2376, 2376, 2376, 2376, 2376, 2376, 2376, 2376, 2376, // levels 1.2, 1.3 and 2
		4752, // level 2.1
		8100, 8100, 8100, 8100, 8100, 8100, 8100, 8100, 8100, // levels 2.2 and 3
		18000, // level 3.1
		20480, // level 3.2
		32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, // levels 4 and 4.1
		34816, // level 4.2
		110400, 110400, 110400, 110400, 110400, 110400, 110400, 110400, // level 5
		184320, 184320, // levels 5.1 and 5.2
		696320, 696320, 696320, 696320, 696320, 696320, 696320, 696320, 696320, 696320, // levels 6, 6.1 and 6.2
		UINT_MAX // no limit beyond
	};
	
	// temp storage, committed if entire NAL is correct
	Edge264SeqParameterSet sps = {
		.chroma_format_idc = 1,
		.ChromaArrayType = 1,
		.BitDepth_Y = 8,
		.BitDepth_C = 8,
		.qpprime_y_zero_transform_bypass_flag = 0,
		.log2_max_pic_order_cnt_lsb = 16,
		.mb_adaptive_frame_field_flag = 0,
		.mvc = 0,
		.PicOrderCntDeltas[0] = 0,
		.weightScale4x4_v = {[0 ... 5] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16}},
		.weightScale8x8_v = {[0 ... 23] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16}},
	};
	
	// Profiles are only useful to initialize max_num_reorder_frames/num_frame_buffers.
	refill(&dec->_gb, 0);
	int profile_idc = get_uv(&dec->_gb, 8);
	int constraint_set_flags = get_uv(&dec->_gb, 8);
	int level_idc = get_uv(&dec->_gb, 8);
	int seq_parameter_set_id = get_ue16(&dec->_gb, 31); // ignored until useful cases arise
	print_header(dec, "<k>profile_idc</k><v>%u (%s)</v>\n"
		"<k>constraint_set_flags</k><v>%x, %x, %x, %x, %x, %x</v>\n"
		"<k>level_idc</k><v>%.1f</v>\n"
		"<k>seq_parameter_set_id</k><v>%u</v>\n",
		profile_idc, profile_idc_names[profile_idc],
		constraint_set_flags >> 7, (constraint_set_flags >> 6) & 1, (constraint_set_flags >> 5) & 1, (constraint_set_flags >> 4) & 1, (constraint_set_flags >> 3) & 1, (constraint_set_flags >> 2) & 1,
		(double)level_idc / 10,
		seq_parameter_set_id);
	
	int seq_scaling_matrix_present_flag = 0;
	if (profile_idc != 66 && profile_idc != 77 && profile_idc != 88) {
		sps.ChromaArrayType = sps.chroma_format_idc = get_ue16(&dec->_gb, 3);
		if (sps.chroma_format_idc == 3)
			sps.ChromaArrayType = get_u1(&dec->_gb) ? 0 : 3;
		sps.BitDepth_Y = 8 + get_ue16(&dec->_gb, 6);
		sps.BitDepth_C = 8 + get_ue16(&dec->_gb, 6);
		sps.qpprime_y_zero_transform_bypass_flag = get_u1(&dec->_gb);
		seq_scaling_matrix_present_flag = get_u1(&dec->_gb);
		print_header(dec, "<k>chroma_format_idc</k><v%s>%u (%s%s)</v>\n"
			"<k>BitDepths</k><v%s>%u:%u:%u</v>\n"
			"<k>qpprime_y_zero_transform_bypass_flag</k><v%s>%x</v>\n",
			red_if(sps.chroma_format_idc != 1), sps.chroma_format_idc, chroma_format_idc_names[sps.chroma_format_idc], (sps.chroma_format_idc < 3) ? "" : (sps.ChromaArrayType == 0) ? " separate" : " non-separate",
			red_if(sps.BitDepth_Y != 8 || sps.BitDepth_C != 8), sps.BitDepth_Y, sps.BitDepth_C, sps.BitDepth_C,
			red_if(sps.qpprime_y_zero_transform_bypass_flag), sps.qpprime_y_zero_transform_bypass_flag);
	} else {
		print_header(dec, "<k>chroma_format_idc (inferred)</k><v>1 (4:2:0)</v>\n"
			"<k>BitDepths (inferred)</k><v>8:8:8</v>\n"
			"<k>qpprime_y_zero_transform_bypass_flag (inferred)</k><v>0</v>\n");
	}
	
	if (seq_scaling_matrix_present_flag) {
		sps.weightScale4x4_v[0] = Default_4x4_Intra;
		sps.weightScale4x4_v[3] = Default_4x4_Inter;
		for (int i = 0; i < 4; i++) {
			sps.weightScale8x8_v[i] = Default_8x8_Intra[i]; // scaling list 6
			sps.weightScale8x8_v[4 + i] = Default_8x8_Inter[i]; // scaling list 7
		}
		parse_scaling_lists(dec, sps.weightScale4x4_v, sps.weightScale8x8_v, 1, sps.chroma_format_idc);
	}
	print_header(dec, "<k>ScalingList4x4%s</k><v><small>", (seq_scaling_matrix_present_flag) ? "" : " (inferred)");
	for (int i = 0; i < 6; i++) {
		for (int j = 0; j < 16; j++)
			print_header(dec, "%u%s", sps.weightScale4x4[i][((int8_t *)scan_4x4)[j]], (j < 15) ? ", " : (i < 5) ? "<br>" : "</small></v>\n");
	}
	if (profile_idc != 66 && profile_idc != 77 && profile_idc != 88) {
		print_header(dec, "<k>ScalingList8x8%s</k><v><small>", (seq_scaling_matrix_present_flag) ? "" : " (inferred)");
		for (int i = 0; i < (sps.chroma_format_idc < 3 ? 2 : 6); i++) {
			for (int j = 0; j < 64; j++)
				print_header(dec, "%u%s", sps.weightScale8x8[i][((int8_t *)scan_8x8_cabac)[j]], (j < 63) ? ", " : (i < (dec->sps.chroma_format_idc < 3 ? 1 : 5)) ? "<br>" : "</small></v>\n");
		}
	}
	
	sps.log2_max_frame_num = get_ue16(&dec->_gb, 12) + 4;
	sps.pic_order_cnt_type = get_ue16(&dec->_gb, 2);
	print_header(dec, "<k>log2_max_frame_num</k><v>%u</v>\n"
		"<k>pic_order_cnt_type</k><v>%u</v>\n",
		sps.log2_max_frame_num,
		sps.pic_order_cnt_type);
	
	if (sps.pic_order_cnt_type == 0) {
		sps.log2_max_pic_order_cnt_lsb = get_ue16(&dec->_gb, 12) + 4;
		print_header(dec, "<k>log2_max_pic_order_cnt_lsb</k><v>%u</v>\n",
			sps.log2_max_pic_order_cnt_lsb);
	
	// clearly one of the spec's useless bits (and a waste of time to implement)
	} else if (sps.pic_order_cnt_type == 1) {
		sps.delta_pic_order_always_zero_flag = get_u1(&dec->_gb);
		sps.offset_for_non_ref_pic = get_se32(&dec->_gb, -32768, 32767); // tighter than spec thanks to condition on DiffPicOrderCnt
		sps.offset_for_top_to_bottom_field = get_se32(&dec->_gb, -32768, 32767);
		sps.num_ref_frames_in_pic_order_cnt_cycle = get_ue16(&dec->_gb, 255);
		print_header(dec, "<k>delta_pic_order_always_zero_flag</k><v>%x</v>\n"
			"<k>offset_for_non_ref_pic</k><v>%d</v>\n"
			"<k>offset_for_top_to_bottom</k><v>%d</v>\n"
			"<k>PicOrderCntDeltas</k><v>0",
			sps.delta_pic_order_always_zero_flag,
			sps.offset_for_non_ref_pic,
			sps.offset_for_top_to_bottom_field);
		for (int i = 1, delta = 0; i <= sps.num_ref_frames_in_pic_order_cnt_cycle; i++) {
			int offset_for_ref_frame = get_se32(&dec->_gb, (-1u << 31) + 1, (1u << 31) - 1);
			sps.PicOrderCntDeltas[i] = delta += offset_for_ref_frame;
			print_header(dec, " %d", sps.PicOrderCntDeltas[i]);
		}
		print_header(dec, "</v>\n");
	}
	
	// Max width is imposed by some int16 storage, wait for actual needs to push it.
	sps.max_num_ref_frames = get_ue16(&dec->_gb, 16);
	int gaps_in_frame_num_value_allowed_flag = get_u1(&dec->_gb);
	sps.pic_width_in_mbs = get_ue16(&dec->_gb, 1022) + 1;
	int pic_height_in_map_units = get_ue16(&dec->_gb, 1054) + 1;
	sps.frame_mbs_only_flag = get_u1(&dec->_gb);
	sps.pic_height_in_mbs = pic_height_in_map_units << 1 >> sps.frame_mbs_only_flag;
	int MaxDpbFrames = min(MaxDpbMbs[min(level_idc, 63)] / (unsigned)(sps.pic_width_in_mbs * sps.pic_height_in_mbs), 16);
	sps.max_num_reorder_frames = ((profile_idc == 44 || profile_idc == 86 ||
		profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
		profile_idc == 244) && (constraint_set_flags & 1 << 4)) ? 0 : MaxDpbFrames;
	sps.num_frame_buffers = max(sps.max_num_reorder_frames, sps.max_num_ref_frames) + 1;
	sps.mb_adaptive_frame_field_flag = 0;
	if (sps.frame_mbs_only_flag == 0)
		sps.mb_adaptive_frame_field_flag = get_u1(&dec->_gb);
	sps.direct_8x8_inference_flag = get_u1(&dec->_gb);
	print_header(dec, "<k>max_num_ref_frames</k><v>%u</v>\n"
		"<k>gaps_in_frame_num_value_allowed_flag</k><v>%x</v>\n"
		"<k>pic_width_in_mbs</k><v>%u</v>\n"
		"<k>pic_height_in_mbs</k><v>%u</v>\n"
		"<k>frame_mbs_only_flag</k><v%s>%x</v>\n"
		"<k>mb_adaptive_frame_field_flag%s</k><v%s>%x</v>\n"
		"<k>direct_8x8_inference_flag</k><v>%x</v>\n",
		sps.max_num_ref_frames,
		gaps_in_frame_num_value_allowed_flag,
		sps.pic_width_in_mbs,
		sps.pic_height_in_mbs,
		red_if(!sps.frame_mbs_only_flag), sps.frame_mbs_only_flag,
		(sps.frame_mbs_only_flag) ? " (inferred)" : "", red_if(!sps.frame_mbs_only_flag), sps.mb_adaptive_frame_field_flag,
		sps.direct_8x8_inference_flag);
	
	// frame_cropping_flag
	if (get_u1(&dec->_gb)) {
		unsigned shiftX = (sps.ChromaArrayType == 1) | (sps.ChromaArrayType == 2);
		unsigned shiftY = (sps.ChromaArrayType == 1);
		int limX = (sps.pic_width_in_mbs << 4 >> shiftX) - 1;
		int limY = (sps.pic_height_in_mbs << 4 >> shiftY) - 1;
		sps.frame_crop_offsets[3] = get_ue16(&dec->_gb, limX) << shiftX;
		sps.frame_crop_offsets[1] = get_ue16(&dec->_gb, limX - (sps.frame_crop_offsets[3] >> shiftX)) << shiftX;
		sps.frame_crop_offsets[0] = get_ue16(&dec->_gb, limY) << shiftY;
		sps.frame_crop_offsets[2] = get_ue16(&dec->_gb, limY - (sps.frame_crop_offsets[0] >> shiftY)) << shiftY;
		print_header(dec, "<k>frame_crop_offsets</k><v>left %u, right %u, top %u, bottom %u</v>\n",
			sps.frame_crop_offsets[3], sps.frame_crop_offsets[1], sps.frame_crop_offsets[0], sps.frame_crop_offsets[2]);
	} else {
		print_header(dec, "<k>frame_crop_offsets (inferred)</k><v>left 0, right 0, top 0, bottom 0</v>\n");
	}
	
	if (get_u1(&dec->_gb)) {
		parse_vui_parameters(dec, &sps);
	} else {
		print_header(dec, "<k>max_num_reorder_frames (inferred)</k><v>%u</v>\n"
			"<k>max_dec_frame_buffering (inferred)</k><v>%u</v>\n",
			sps.max_num_reorder_frames,
			sps.num_frame_buffers - 1);
	}
	
	// additional stuff for subset_seq_parameter_set
	if (dec->nal_unit_type == 15 && (profile_idc == 118 || profile_idc == 128 || profile_idc == 134)) {
		if (memcmp(&sps, &dec->sps, sizeof(sps)) != 0)
			return ENOTSUP;
		if (!get_u1(&dec->_gb))
			return EBADMSG;
		if (parse_seq_parameter_set_mvc_extension(dec, &sps, profile_idc))
			return ENOTSUP;
		if (get_u1(&dec->_gb))
			parse_mvc_vui_parameters_extension(dec);
		get_u1(&dec->_gb);
	}
	
	// check for trailing_bits before unsupported features (in case errors enabled them)
	if (dec->_gb.msb_cache != (size_t)1 << (SIZE_BIT - 1) || (dec->_gb.lsb_cache & (dec->_gb.lsb_cache - 1)) || (intptr_t)(dec->_gb.end - dec->_gb.CPB) > 0)
		return EBADMSG;
	if (sps.ChromaArrayType != 1 || sps.BitDepth_Y != 8 || sps.BitDepth_C != 8 ||
		sps.qpprime_y_zero_transform_bypass_flag || !sps.frame_mbs_only_flag)
		return ENOTSUP;
	
	// apply the changes on the dependent variables if the frame format changed
	int64_t offsets;
	memcpy(&offsets, dec->out.frame_crop_offsets, 8);
	if (sps.DPB_format != dec->DPB_format || sps.frame_crop_offsets_l != offsets) {
		if (dec->output_flags | dec->borrow_flags) {
			for (unsigned o = dec->output_flags; o; o &= o - 1)
				dec->dispPicOrderCnt = max(dec->dispPicOrderCnt, dec->FieldOrderCnt[0][__builtin_ctz(o)]);
			while (!non_blocking && dec->busy_tasks)
				pthread_cond_wait(&dec->task_complete, &dec->lock);
			return dec->busy_tasks ? EWOULDBLOCK : ENOBUFS;
		}
		dec->DPB_format = sps.DPB_format;
		memcpy(dec->out.frame_crop_offsets, &sps.frame_crop_offsets_l, 8);
		int width = sps.pic_width_in_mbs << 4;
		int height = sps.pic_height_in_mbs << 4;
		dec->out.pixel_depth_Y = sps.BitDepth_Y > 8;
		dec->out.width_Y = width - dec->out.frame_crop_offsets[3] - dec->out.frame_crop_offsets[1];
		dec->out.height_Y = height - dec->out.frame_crop_offsets[0] - dec->out.frame_crop_offsets[2];
		dec->out.stride_Y = width << dec->out.pixel_depth_Y;
		if (!(dec->out.stride_Y & 2047)) // add an offset to stride if it is a multiple of 2048
			dec->out.stride_Y += 16 << dec->out.pixel_depth_Y;
		dec->plane_size_Y = dec->out.stride_Y * height;
		if (sps.chroma_format_idc > 0) {
			dec->out.pixel_depth_C = sps.BitDepth_C > 8;
			dec->out.width_C = sps.chroma_format_idc == 3 ? dec->out.width_Y : dec->out.width_Y >> 1;
			dec->out.stride_C = (sps.chroma_format_idc == 3 ? width << 1 : width) << dec->out.pixel_depth_C;
			if (!(dec->out.stride_C & 4095)) // add an offset to stride if it is a multiple of 4096
				dec->out.stride_C += (sps.chroma_format_idc == 3 ? 16 : 8) << dec->out.pixel_depth_C;
			dec->out.height_C = sps.chroma_format_idc == 1 ? dec->out.height_Y >> 1 : dec->out.height_Y;
			dec->plane_size_C = (sps.chroma_format_idc == 1 ? height >> 1 : height) * dec->out.stride_C;
		}
		int mbs = (sps.pic_width_in_mbs + 1) * sps.pic_height_in_mbs - 1;
		dec->frame_size = dec->plane_size_Y + dec->plane_size_C + mbs * sizeof(Edge264Macroblock);
		dec->currPic = dec->basePic = -1;
		dec->reference_flags = dec->long_term_flags = dec->frame_flip_bits = 0;
		for (int i = 0; i < 32; i++) {
			if (dec->frame_buffers[i] != NULL) {
				free(dec->frame_buffers[i]);
				dec->frame_buffers[i] = NULL;
			}
		}
	}
	dec->sps = sps;
	return 0;
}



/**
 * This NAL type for transparent videos is unsupported until encoders actually
 * support it.
 */
int ADD_VARIANT(parse_seq_parameter_set_extension)(Edge264Decoder *dec, int non_blocking, void(*free_cb)(void*,int), void *free_arg) {
	refill(&dec->_gb, 0);
	int seq_parameter_set_id = get_ue16(&dec->_gb, 31);
	int aux_format_idc = get_ue16(&dec->_gb, 3);
	print_header(dec, "<k>seq_parameter_set_id</k><v>%u</v>\n"
		"<k>aux_format_idc</k><v%s>%u</v>\n",
		seq_parameter_set_id,
		red_if(aux_format_idc), aux_format_idc);
	if (aux_format_idc != 0) {
		int bit_depth_aux = get_ue16(&dec->_gb, 4) + 8;
		get_uv(&dec->_gb, 3 + bit_depth_aux * 2);
	}
	get_u1(&dec->_gb);
	if (dec->_gb.msb_cache != (size_t)1 << (SIZE_BIT - 1) || (dec->_gb.lsb_cache & (dec->_gb.lsb_cache - 1)) || (intptr_t)(dec->_gb.end - dec->_gb.CPB) > 0) // rbsp_trailing_bits
		return EBADMSG;
	return aux_format_idc != 0 ? ENOTSUP : 0; // unsupported if transparent
}
