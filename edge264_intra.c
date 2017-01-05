// TODO: Add testing of borders from ctx
// TODO: Optimise _mm_set_epi64?
// TODO: Add 1px unused line atop the first picture to avoid testing forbidden reads
// TODO: uninline loads?
// TODO: Make 4x4 two-pass too, and gather all _mm_setzero_si128()
// TODO: Decrement p before all!
// TODO: Compare execution times as inline vs noinline
// TODO: Reorder enums to separate hot&cold paths
// TODO: Reorder instructions to put load8_8bit last whenever possible
// TODO: Fix _mm_movpi64_epi64 with GCC

#include "edge264_common.h"

int decode_Residual4x4(__m128i, __m128i);
int decode_Residual8x8(__m128i, __m128i, __m128i, __m128i, __m128i, __m128i, __m128i, __m128i);

/**
 * Intra decoding involves so many shuffling tricks that it is better expressed
 * as native intrinsics, where each architecture can give its best.
 *
 * Choosing between the different possibilities of a same function is tricky,
 * in general I favor in order:
 * _ the shortest dependency chain (instructions are pipelined in parallel),
 * _ the smallest code+data (avoid excessive use of pshufb),
 * _ faster instructions (http://www.agner.org/optimize/#manual_instr_tab),
 * _ readable code (helped by Intel's astounding instrinsics naming...).
 */
static __attribute__((noinline)) __m128i load8_8bit(uint8_t *p, size_t stride, uint8_t *first) {
	__m64 m0 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 1 - 8), *(__m64 *)(first - 8));
	__m64 m1 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 3 - 8), *(__m64 *)(p + stride * 2 - 8));
	__m64 m2 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 5 - 8), *(__m64 *)(p + stride * 4 - 8));
	__m64 m3 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 7 - 8), *(__m64 *)(p + stride * 6 - 8));
	__m64 m4 = _mm_unpackhi_pi32(_mm_unpackhi_pi16(m3, m2), _mm_unpackhi_pi16(m1, m0));
	return _mm_unpacklo_epi8(_mm_movpi64_epi64(m4), _mm_setzero_si128());
}

static __attribute__((noinline)) __m128i load8_16bit(uint8_t *p, size_t stride, uint8_t *first) {
	__m128i x0 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 1 - 16), *(__m128i *)(first - 16));
	__m128i x1 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 3 - 16), *(__m128i *)(p + stride * 2 - 16));
	__m128i x2 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 5 - 16), *(__m128i *)(p + stride * 4 - 16));
	__m128i x3 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 7 - 16), *(__m128i *)(p + stride * 6 - 16));
	return _mm_unpackhi_epi64(_mm_unpackhi_epi32(x3, x2), _mm_unpackhi_epi32(x1, x0));
}

static inline __m128i lowpass(__m128i left, __m128i mid, __m128i right) {
	return _mm_avg_epu16(_mm_srli_epi16(_mm_add_epi16(left, right), 1), mid);
}



/**
 * For Intra_4x4 we share as much code as possible among 8/16bit, making two
 * separate functions only when the algorithms are too different.
 */
static int decode_Horizontal4x4_8bit(uint8_t *p, size_t stride) {
	static const v16qi shuf = {3, -1, 3, -1, 3, -1, 3, -1, -1, 11, -1, 11, -1, 11, -1, 11};
	__m128i x0 = _mm_set_epi64(*(__m64 *)(p + stride * 2 - 4), *(__m64 *)(p + stride * 1 - 4));
	__m128i x1 = _mm_set_epi64(*(__m64 *)(p + stride * 4 - 4), *(__m64 *)(p + stride * 3 - 4));
	__m128i x2 = _mm_shuffle_epi8(x0, (__m128i)shuf);
	__m128i x3 = _mm_shuffle_epi8(x1, (__m128i)shuf);
	return decode_Residual4x4(x2, x3);
}

static int decode_Horizontal4x4_16bit(uint8_t *p, size_t stride) {
	__m128i x0 = _mm_set_epi64(*(__m64 *)(p + stride * 1 - 8), *(__m64 *)(p + stride * 2 - 8));
	__m128i x1 = _mm_set_epi64(*(__m64 *)(p + stride * 3 - 8), *(__m64 *)(p + stride * 4 - 8));
	__m128i x2 = _mm_shufflelo_epi16(x0, _MM_SHUFFLE(3, 3, 3, 3));
	__m128i x3 = _mm_shufflelo_epi16(x1, _MM_SHUFFLE(3, 3, 3, 3));
	__m128i x4 = _mm_shufflehi_epi16(x2, _MM_SHUFFLE(3, 3, 3, 3));
	__m128i x5 = _mm_shufflehi_epi16(x3, _MM_SHUFFLE(3, 3, 3, 3));
	return decode_Residual4x4(x4, x5);
}

static int decode_DC4x4_8bit(__m64 m0) {
	__m64 DC = _mm_srli_pi16(_mm_add_pi16(_mm_sad_pu8(m0, _mm_setzero_si64()), _mm_set1_pi16(4)), 3);
	__m128i x0 = _mm_set1_epi64(_mm_shuffle_pi16(DC, _MM_SHUFFLE(0, 0, 0, 0)));
	return decode_Residual4x4(x0, x0);
}

static int decode_DC4x4_16bit(__m64 m0, __m64 m1) {
	__m64 DC = _mm_srli_si64(_mm_avg_pu16(m0, _mm_add_pi16(m1, _mm_set_pi16(3, 0, 0, 0))), 50);
	__m128i x0 = _mm_set1_epi64(_mm_shuffle_pi16(DC, _MM_SHUFFLE(0, 0, 0, 0)));
	return decode_Residual4x4(x0, x0);
}

static int decode_DiagonalDownLeft4x4(__m128i top) {
	__m128i x0 = _mm_srli_si128(top, 2);
	__m128i x1 = _mm_shufflehi_epi16(_mm_shuffle_epi32(top, _MM_SHUFFLE(3, 3, 2, 1)), _MM_SHUFFLE(1, 1, 1, 0));
	__m128i x2 = lowpass(top, x0, x1);
	__m128i x3 = _mm_srli_si128(x2, 2);
	__m128i x4 = (__m128i)_mm_shuffle_ps((__m128)x2, (__m128)x3, _MM_SHUFFLE(1, 0, 1, 0));
	__m128i x5 = (__m128i)_mm_shuffle_ps((__m128)x2, (__m128)x3, _MM_SHUFFLE(2, 1, 2, 1));
	return decode_Residual4x4(x4, x5);
}

static int decode_DiagonalDownRight4x4(__m128i lt, __m128i bot) {
	__m128i x0 = _mm_slli_si128(lt, 2);
	__m128i x1 = _mm_alignr_epi8(lt, bot, 12);
	__m128i x2 = lowpass(lt, x0, x1);
	__m128i x3 = _mm_slli_si128(x2, 2);
	__m128i x4 = (__m128i)_mm_shuffle_ps((__m128)x2, (__m128)x3, _MM_SHUFFLE(3, 2, 3, 2));
	__m128i x5 = (__m128i)_mm_shuffle_ps((__m128)x2, (__m128)x3, _MM_SHUFFLE(2, 1, 2, 1));
	return decode_Residual4x4(x4, x5);
}

static int decode_VerticalRight4x4(__m128i lt) {
	__m128i x0 = _mm_slli_si128(lt, 2);
	__m128i x1 = _mm_shuffle_epi32(lt, _MM_SHUFFLE(2, 1, 0, 0));
	__m128i x2 = _mm_avg_epu16(lt, x0);
	__m128i x3 = lowpass(lt, x0, x1);
	__m128i x4 = (__m128i)_mm_shuffle_ps((__m128)x3, (__m128)x2, _MM_SHUFFLE(3, 2, 1, 0));
	__m128i x5 = _mm_shufflelo_epi16(x3, _MM_SHUFFLE(2, 0, 0, 0));
	__m128i x6 = _mm_unpackhi_epi64(x2, x3);
	__m128i x7 = _mm_unpackhi_epi64(_mm_slli_si128(x4, 2), _mm_slli_si128(x5, 2));
	return decode_Residual4x4(x6, x7);
}

static int decode_HorizontalDown4x4(__m128i lt) {
	__m128i x0 = _mm_srli_si128(lt, 2);
	__m128i x1 = _mm_shuffle_epi32(lt, _MM_SHUFFLE(3, 3, 2, 1));
	__m128i x2 = _mm_avg_epu16(lt, x0);
	__m128i x3 = lowpass(lt, x0, x1);
	__m128i x4 = _mm_unpacklo_epi16(x2, x3);
	__m128i x5 = _mm_shuffle_epi32(_mm_unpackhi_epi64(x3, x4), _MM_SHUFFLE(1, 0, 2, 1));
	__m128i x6 = _mm_shuffle_epi32(x4, _MM_SHUFFLE(1, 0, 2, 1));
	return decode_Residual4x4(x5, x6);
}

static int decode_VerticalLeft4x4(__m128i top) {
	__m128i x0 = _mm_srli_si128(top, 2);
	__m128i x1 = _mm_shufflehi_epi16(_mm_shuffle_epi32(top, _MM_SHUFFLE(3, 3, 2, 1)), _MM_SHUFFLE(1, 1, 1, 0));
	__m128i x2 = _mm_avg_epu16(top, x0);
	__m128i x3 = lowpass(top, x0, x1);
	__m128i x4 = _mm_unpacklo_epi64(x2, x3);
	__m128i x5 = _mm_unpacklo_epi64(_mm_srli_si128(x2, 2), _mm_srli_si128(x3, 2));
	return decode_Residual4x4(x4, x5);
}

static int decode_HorizontalUp4x4_8bit(uint8_t *p, size_t stride) {
	__m64 m0 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 1 - 4), *(__m64 *)(p + stride * 2 - 4));
	__m64 m1 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 3 - 4), *(__m64 *)(p + stride * 4 - 4));
	__m64 m2 = _mm_unpackhi_pi16(m0, m1);
	__m128i x0 = _mm_unpacklo_epi8(_mm_movpi64_epi64(m2), _mm_setzero_si128());
	__m128i x1 = _mm_shufflelo_epi16(x0, _MM_SHUFFLE(3, 3, 2, 1));
	__m128i x2 = _mm_shufflelo_epi16(x0, _MM_SHUFFLE(3, 3, 3, 2));
	__m128i x3 = _mm_avg_epu16(x0, x1);
	__m128i x4 = lowpass(x0, x1, x2);
	__m128i x5 = _mm_unpacklo_epi16(x3, x4);
	__m128i x6 = _mm_shuffle_epi32(x5, _MM_SHUFFLE(2, 1, 1, 0));
	__m128i x7 = _mm_shuffle_epi32(x5, _MM_SHUFFLE(3, 3, 3, 2));
	return decode_Residual4x4(x6, x7);
}

static int decode_HorizontalUp4x4_16bit(uint8_t *p, size_t stride) {
   __m64 m0 = _mm_shuffle_pi16(*(__m64 *)(p + stride * 4 - 8), _MM_SHUFFLE(3, 3, 3, 3));
   __m64 m1 = _mm_alignr_pi8(m0, *(__m64 *)(p + stride * 3 - 8), 6);
   __m64 m2 = _mm_alignr_pi8(m1, *(__m64 *)(p + stride * 2 - 8), 6);
   __m64 m3 = _mm_alignr_pi8(m2, *(__m64 *)(p + stride * 1 - 8), 6);
   __m64 m4 = _mm_avg_pu16(m2, m3);
	__m64 m5 =  _mm_avg_pu16(_mm_srli_pi16(_mm_add_pi16(m1, m3), 1), m2);
   __m128i x0 = _mm_unpacklo_epi16(_mm_movpi64_epi64(m4), _mm_movpi64_epi64(m5));
   __m128i x1 = _mm_shuffle_epi32(x0, _MM_SHUFFLE(2, 1, 1, 0));
   __m128i x2 = _mm_shuffle_epi32(x0, _MM_SHUFFLE(3, 3, 3, 2));
   return decode_Residual4x4(x1, x2);
}



/**
 * Intra_8x8 has wide functions and many variations of the same prediction
 * modes, so we focus even more on shared code rather than performance.
 */
static int decode_Vertical8x8(__m128i topr, __m128i topm, __m128i topl) {
	__m128i x0 = lowpass(topr, topm, topl);
	return decode_Residual8x8(x0, x0, x0, x0, x0, x0, x0, x0);
}

static int decode_Horizontal8x8(__m128i left, __m128i bot) {
	__m128i x0 = _mm_alignr_epi8(left, bot, 14);
	__m128i x1 = _mm_alignr_epi8(x0, bot, 14);
	__m128i x2 = lowpass(left, x0, x1);
	__m128i x3 = _mm_unpackhi_epi16(x2, x2);
	__m128i x4 = _mm_unpacklo_epi16(x2, x2);
	__m128i x5 = _mm_shuffle_epi32(x3, _MM_SHUFFLE(3, 3, 3, 3));
	__m128i x6 = _mm_shuffle_epi32(x3, _MM_SHUFFLE(2, 2, 2, 2));
	__m128i x7 = _mm_shuffle_epi32(x3, _MM_SHUFFLE(1, 1, 1, 1));
	__m128i x8 = _mm_shuffle_epi32(x3, _MM_SHUFFLE(0, 0, 0, 0));
	__m128i x9 = _mm_shuffle_epi32(x4, _MM_SHUFFLE(3, 3, 3, 3));
	__m128i xA = _mm_shuffle_epi32(x4, _MM_SHUFFLE(2, 2, 2, 2));
	__m128i xB = _mm_shuffle_epi32(x4, _MM_SHUFFLE(1, 1, 1, 1));
	__m128i xC = _mm_shuffle_epi32(x4, _MM_SHUFFLE(0, 0, 0, 0));
	return decode_Residual8x8(x5, x6, x7, x8, x9, xA, xB, xC);
}

static int decode_DC8x8(__m128i topr, __m128i topm, __m128i topl, __m128i left, __m128i bot0, __m128i bot1) {
	__m128i h1 = _mm_set1_epi16(1);
	__m128i x0 = _mm_alignr_epi8(left, bot0, 14);
	__m128i x1 = _mm_alignr_epi8(x0, bot1, 14);
	__m128i x2 = lowpass(left, x0, x1);
	__m128i x3 = lowpass(topr, topm, topl);
	__m128i x4 = _mm_madd_epi16(_mm_add_epi16(_mm_add_epi16(x2, x3), h1), h1);
	__m128i x5 = _mm_hadd_epi32(x4, x4);
	__m128i x6 = _mm_srli_epi32(_mm_hadd_epi32(x5, x5), 4);
	__m128i DC = _mm_packs_epi32(x6, x6);
	return decode_Residual8x8(DC, DC, DC, DC, DC, DC, DC, DC);
}

static int decode_DiagonalDownLeft8x8(__m128i right, __m128i top, __m128i topl) {
	__m128i topr = _mm_alignr_epi8(right, top, 2);
	__m128i rightl = _mm_alignr_epi8(right, top, 14);
	__m128i rightr = _mm_shufflehi_epi16(_mm_srli_si128(right, 2), _MM_SHUFFLE(2, 2, 1, 0));
	__m128i x0 = lowpass(topl, top, topr);
	__m128i x1 = lowpass(rightl, right, rightr);
	__m128i x2 = _mm_alignr_epi8(x1, x0, 2);
	__m128i x3 = _mm_alignr_epi8(x1, x0, 4);
	__m128i x4 = _mm_srli_si128(x1, 2);
	__m128i x5 = _mm_shufflehi_epi16(_mm_shuffle_epi32(x1, _MM_SHUFFLE(3, 3, 2, 1)), _MM_SHUFFLE(1, 1, 1, 0));
	__m128i x6 = lowpass(x0, x2, x3);
	__m128i x7 = lowpass(x1, x4, x5);
	__m128i x8 = _mm_alignr_epi8(x7, x6, 2);
	__m128i x9 = _mm_alignr_epi8(x7, x6, 4);
	__m128i xA = _mm_alignr_epi8(x7, x6, 6);
	__m128i xB = _mm_alignr_epi8(x7, x6, 8);
	__m128i xC = _mm_alignr_epi8(x7, x6, 10);
	__m128i xD = _mm_alignr_epi8(x7, x6, 12);
	__m128i xE = _mm_alignr_epi8(x7, x6, 14);
	return decode_Residual8x8(x6, x8, x9, xA, xB, xC, xD, xE);
}

static int decode_DiagonalDownRight8x8(__m128i topr, __m128i top, __m128i left, __m128i bot) {
	__m128i bott = _mm_alignr_epi8(left, bot, 2);
	__m128i leftb = _mm_alignr_epi8(left, bot, 14);
	__m128i leftt = _mm_alignr_epi8(top, left, 2);
	__m128i topl = _mm_alignr_epi8(top, left, 14);
	__m128i x0 = lowpass(bot, bot, bott);
	__m128i x1 = lowpass(leftb, left, leftt);
	__m128i x2 = lowpass(topl, top, topr);
	__m128i x3 = _mm_slli_si128(x1, 2);
	__m128i x4 = _mm_alignr_epi8(x1, x0, 12);
	__m128i x5 = _mm_alignr_epi8(x2, x1, 14);
	__m128i x6 = _mm_alignr_epi8(x2, x1, 12);
	__m128i x7 = lowpass(x1, x3, x4);
	__m128i x8 = lowpass(x2, x5, x6);
	__m128i x9 = _mm_alignr_epi8(x8, x7, 14);
	__m128i xA = _mm_alignr_epi8(x8, x7, 12);
	__m128i xB = _mm_alignr_epi8(x8, x7, 10);
	__m128i xC = _mm_alignr_epi8(x8, x7, 8);
	__m128i xD = _mm_alignr_epi8(x8, x7, 6);
	__m128i xE = _mm_alignr_epi8(x8, x7, 4);
	__m128i xF = _mm_alignr_epi8(x8, x7, 2);
	return decode_Residual8x8(x8, x9, xA, xB, xC, xD, xE, xF);
}

static int decode_VerticalRight8x8(__m128i topr, __m128i top, __m128i left, __m128i bot) {
	__m128i leftb = _mm_alignr_epi8(left, bot, 14);
	__m128i leftt = _mm_alignr_epi8(top, left, 2);
	__m128i topl = _mm_alignr_epi8(top, left, 14);
	__m128i x0 = lowpass(leftb, left, leftt);
	__m128i x1 = lowpass(topl, top, topr);
	__m128i x2 = _mm_slli_si128(x0, 2);
	__m128i x3 = _mm_shuffle_epi32(x0, _MM_SHUFFLE(2, 1, 0, 0));
	__m128i x4 = _mm_alignr_epi8(x1, x0, 14);
	__m128i x5 = _mm_alignr_epi8(x1, x0, 12);
	__m128i x6 = _mm_avg_epu16(x1, x4);
	__m128i x7 = lowpass(x0, x2, x3);
	__m128i x8 = lowpass(x1, x4, x5);
	__m128i x9 = _mm_alignr_epi8(x6, x7, 14);
	__m128i xA = _mm_alignr_epi8(x8, x7 = _mm_slli_si128(x7, 2), 14);
	__m128i xB = _mm_alignr_epi8(x9, x7 = _mm_slli_si128(x7, 2), 14);
	__m128i xC = _mm_alignr_epi8(xA, x7 = _mm_slli_si128(x7, 2), 14);
	__m128i xD = _mm_alignr_epi8(xB, x7 = _mm_slli_si128(x7, 2), 14);
	__m128i xE = _mm_alignr_epi8(xC, _mm_slli_si128(x7, 2), 14);
	return decode_Residual8x8(x6, x8, x9, xA, xB, xC, xD, xE);
}

static int decode_HorizontalDown8x8(__m128i top, __m128i left, __m128i bot) {
	__m128i leftb = _mm_alignr_epi8(left, bot, 14);
	__m128i leftbb = _mm_alignr_epi8(leftb, bot, 14);
	__m128i topll = _mm_alignr_epi8(top, left, 12);
	__m128i topl = _mm_alignr_epi8(top, left, 14);
	__m128i x0 = lowpass(leftbb, leftb, left);
	__m128i x1 = lowpass(topll, topl, top);
	__m128i x2 = _mm_alignr_epi8(x1, x0, 2);
	__m128i x3 = _mm_alignr_epi8(x1, x0, 4);
	__m128i x4 = _mm_srli_si128(x1, 2);
	__m128i x5 = _mm_shuffle_epi32(x1, _MM_SHUFFLE(3, 3, 2, 1));
	__m128i x6 = _mm_avg_epu16(x0, x2);
	__m128i x7 = lowpass(x0, x2, x3);
	__m128i x8 = lowpass(x1, x4, x5);
	__m128i x9 = _mm_unpackhi_epi16(x6, x7);
	__m128i xA = _mm_unpacklo_epi16(x6, x7);
	__m128i xB = _mm_alignr_epi8(x8, x9, 12);
	__m128i xC = _mm_alignr_epi8(x8, x9, 8);
	__m128i xD = _mm_alignr_epi8(x8, x9, 4);
	__m128i xE = _mm_alignr_epi8(x9, xA, 12);
	__m128i xF = _mm_alignr_epi8(x9, xA, 8);
	__m128i xG = _mm_alignr_epi8(x9, xA, 4);
	return decode_Residual8x8(xB, xC, xD, x9, xE, xF, xG, xA);
}

static int decode_VerticalLeft8x8(__m128i right, __m128i top, __m128i topl) {
	__m128i topr = _mm_alignr_epi8(right, top, 2);
	__m128i rightl = _mm_alignr_epi8(right, top, 14);
	__m128i rightr = _mm_shufflehi_epi16(_mm_srli_si128(right, 2), _MM_SHUFFLE(2, 2, 1, 0));
	__m128i x0 = lowpass(topl, top, topr);
	__m128i x1 = lowpass(rightl, right, rightr);
	__m128i x2 = _mm_alignr_epi8(x1, x0, 2);
	__m128i x3 = _mm_alignr_epi8(x1, x0, 4);
	__m128i x4 = _mm_srli_si128(x1, 2);
	__m128i x5 = _mm_shuffle_epi32(x1, _MM_SHUFFLE(3, 3, 2, 1));
	__m128i x6 = _mm_avg_epu16(x0, x2);
	__m128i x7 = _mm_avg_epu16(x1, x4);
	__m128i x8 = lowpass(x0, x2, x3);
	__m128i x9 = lowpass(x1, x4, x5);
	__m128i xA = _mm_alignr_epi8(x7, x6, 2);
	__m128i xB = _mm_alignr_epi8(x9, x8, 2);
	__m128i xC = _mm_alignr_epi8(x7, x6, 4);
	__m128i xD = _mm_alignr_epi8(x9, x8, 4);
	__m128i xE = _mm_alignr_epi8(x7, x6, 6);
	__m128i xF = _mm_alignr_epi8(x9, x8, 6);
	return decode_Residual8x8(x6, x8, xA, xB, xC, xD, xE, xF);
}

static int decode_HorizontalUp8x8(__m128i btfel, __m128i pot) {
	__m128i tfel = _mm_alignr_epi8(btfel, pot, 14);
	__m128i bbtfel = _mm_shufflehi_epi16(_mm_srli_si128(btfel, 2), _MM_SHUFFLE(2, 2, 1, 0));
	__m128i x0 = lowpass(tfel, btfel, bbtfel);
	__m128i x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
	__m128i x2 = _mm_shufflehi_epi16(_mm_shuffle_epi32(x0, _MM_SHUFFLE(3, 3, 2, 1)), _MM_SHUFFLE(1, 1, 1, 0));
	__m128i x3 = _mm_avg_epu16(x0, x1);
	__m128i x4 = lowpass(x0, x1, x2);
	__m128i x5 = _mm_unpacklo_epi16(x3, x4);
	__m128i x6 = _mm_unpackhi_epi16(x3, x4);
	__m128i x7 = _mm_alignr_epi8(x6, x5, 4);
	__m128i x8 = _mm_alignr_epi8(x6, x5, 8);
	__m128i x9 = _mm_alignr_epi8(x6, x5, 12);
	__m128i xA = _mm_shuffle_epi32(x6, _MM_SHUFFLE(3, 3, 2, 1));
	__m128i xB = _mm_shuffle_epi32(x6, _MM_SHUFFLE(3, 3, 3, 2));
	__m128i xC = _mm_shuffle_epi32(x6, _MM_SHUFFLE(3, 3, 3, 3));
	return decode_Residual8x8(x5, x7, x8, x9, x6, xA, xB, xC);
}



/**
 * Intra_16x16
 */
static int predict_Plane16x16_8bit(uint8_t *p, size_t stride)
{
	static const v16qi mul0 = {-8, -7, -6, -5, -4, -3, -2, -1, 1, 2, 3, 4, 5, 6, 7, 8};
	static const v16qi mul1 = {8, 7, 6, 5, 4, 3, 2, 1, -1, -2, -3, -4, -5, -6, -7, -8};
	static const v8hi mul2 = {-7, -6, -5, -4, -3, -2, -1, 0};
	static const v8hi mul3 = {1, 2, 3, 4, 5, 6, 7, 8};
	static const v8hi h8 = {8, 8, 8, 8, 8, 8, 8, 8};
	
	// load all neighbouring samples
	int p15 = p[15];
	__m128i top = _mm_set_epi64(*(__m64 *)(p + 8), *(__m64 *)(p - 1));
	__m128i l0 = _mm_alignr_epi8(top, *(__m128i *)(p += stride - 16), 15);
	__m128i l1 = _mm_alignr_epi8(l0, *(__m128i *)(p += stride), 15);
	__m128i l2 = _mm_alignr_epi8(l1, *(__m128i *)(p += stride), 15);
	__m128i l3 = _mm_alignr_epi8(l2, *(__m128i *)(p += stride), 15);
	__m128i l4 = _mm_alignr_epi8(l3, *(__m128i *)(p += stride), 15);
	__m128i l5 = _mm_alignr_epi8(l4, *(__m128i *)(p += stride), 15);
	__m128i l6 = _mm_alignr_epi8(l5, *(__m128i *)(p += stride), 15);
	__m128i l7 = _mm_alignr_epi8(l6, *(__m128i *)(p += stride * 2), 15);
	__m128i l8 = _mm_alignr_epi8(l7, *(__m128i *)(p += stride), 15);
	__m128i l9 = _mm_alignr_epi8(l8, *(__m128i *)(p += stride), 15);
	__m128i lA = _mm_alignr_epi8(l9, *(__m128i *)(p += stride), 15);
	__m128i lB = _mm_alignr_epi8(lA, *(__m128i *)(p += stride), 15);
	__m128i lC = _mm_alignr_epi8(lB, *(__m128i *)(p += stride), 15);
	__m128i lD = _mm_alignr_epi8(lC, *(__m128i *)(p += stride), 15);
	__m128i left = _mm_alignr_epi8(lD, *(__m128i *)(p += stride), 15);
	
	// sum them and compute a, b, c (with care for overflow)
	__m128i x0 = _mm_maddubs_epi16(top, (__m128i)mul0);
	__m128i x1 = _mm_maddubs_epi16(left, (__m128i)mul1);
	__m128i x2 = _mm_hadd_epi16(x1, x0);
	__m128i x3 = _mm_add_epi16(x2, _mm_shuffle_epi32(x2, _MM_SHUFFLE(2, 3, 0, 1)));
	__m128i HV = _mm_hadd_epi16(x3, x3); // VVHHVVHH, 15 significant bits
	__m128i x4 = _mm_add_epi16(HV, _mm_srai_epi16(HV, 2)); // (5 * HV) >> 2
	__m128i x5 = _mm_srai_epi16(_mm_add_epi16(x4, (__m128i)h8), 4); // (5 * HV + 32) >> 6
	__m128i a = _mm_set1_epi16((p15 + p[15] + 1) * 16); // 12 significant bits
	__m128i b = _mm_shuffle_epi32(x5, _MM_SHUFFLE(3, 3, 1, 1)); // 11 significant bits
	__m128i c = _mm_shuffle_epi32(x5, _MM_SHUFFLE(2, 2, 0, 0));
	
	// compute the first row of prediction vectors
	__m128i c1 = _mm_slli_epi16(c, 1);
	__m128i c2 = _mm_slli_epi16(c, 2);
	((__m128i *)ctx->pred_buffer)[16] = c1;
	__m128i x6 = _mm_sub_epi16(_mm_sub_epi16(a, c), _mm_add_epi16(c1, c2)); // a - c * 7 + 16
	__m128i x7 = _mm_add_epi16(_mm_mullo_epi16(b, (__m128i)mul2), x6);
	__m128i x8 = _mm_add_epi16(_mm_mullo_epi16(b, (__m128i)mul3), x6);
	__m128i x9 = _mm_add_epi16(x7, c);
	__m128i xA = _mm_add_epi16(x8, c);
	__m128i p0 = _mm_unpacklo_epi64(x7, x9);
	__m128i p1 = _mm_unpackhi_epi64(x7, x9);
	__m128i p2 = _mm_unpacklo_epi64(x8, xA);
	__m128i p3 = _mm_unpackhi_epi64(x8, xA);
	
	// store them
	((__m128i *)ctx->pred_buffer)[0] = p0;
	((__m128i *)ctx->pred_buffer)[1] = p1;
	((__m128i *)ctx->pred_buffer)[4] = p2;
	((__m128i *)ctx->pred_buffer)[5] = p3;
	((__m128i *)ctx->pred_buffer)[2] = p0 = _mm_add_epi16(p0, c2);
	((__m128i *)ctx->pred_buffer)[3] = p1 = _mm_add_epi16(p1, c2);
	((__m128i *)ctx->pred_buffer)[6] = p2 = _mm_add_epi16(p2, c2);
	((__m128i *)ctx->pred_buffer)[7] = p3 = _mm_add_epi16(p3, c2);
	((__m128i *)ctx->pred_buffer)[8] = p0 = _mm_add_epi16(p0, c2);
	((__m128i *)ctx->pred_buffer)[9] = p1 = _mm_add_epi16(p1, c2);
	((__m128i *)ctx->pred_buffer)[12] = p2 = _mm_add_epi16(p2, c2);
	((__m128i *)ctx->pred_buffer)[13] = p3 = _mm_add_epi16(p3, c2);
	((__m128i *)ctx->pred_buffer)[10] = _mm_add_epi16(p0, c2);
	((__m128i *)ctx->pred_buffer)[11] = _mm_add_epi16(p1, c2);
	((__m128i *)ctx->pred_buffer)[14] = _mm_add_epi16(p2, c2);
	((__m128i *)ctx->pred_buffer)[15] = _mm_add_epi16(p3, c2);
	return 0;
}

int predict_Plane16x16_16bit(uint8_t *p, size_t stride)
{
	static const v16qi inv = {14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1};
	static const v8hi mul0 = {40, 35, 30, 25, 20, 15, 10, 5};
	static const v4si mul1 = {-7, -6, -5, -4};
	static const v4si mul2 = {-3, -2, -1, 0};
	static const v4si mul3 = {1, 2, 3, 4};
	static const v4si mul4 = {5, 6, 7, 8};
	static const v4si s32 = {32, 32, 32, 32};
	
	// load all neighbouring samples
	int p15 = p[15];
	__m128i t0 = _mm_loadu_si128((__m128i *)(p - 2));
	__m128i t1 = _mm_shuffle_epi8(*(__m128i *)(p + 16), (__m128i)inv);
	__m128i l0 = _mm_alignr_epi8(t0, *(__m128i *)(p += stride - 16), 14);
	__m128i l8 = _mm_srli_si128(*(__m128i *)(p + stride * 8), 14);
	__m128i l1 = _mm_alignr_epi8(l0, *(__m128i *)(p += stride), 14);
	__m128i l9 = _mm_alignr_epi8(l8, *(__m128i *)(p + stride * 8), 14);
	__m128i l2 = _mm_alignr_epi8(l1, *(__m128i *)(p += stride), 14);
	__m128i lA = _mm_alignr_epi8(l9, *(__m128i *)(p + stride * 8), 14);
	__m128i l3 = _mm_alignr_epi8(l2, *(__m128i *)(p += stride), 14);
	__m128i lB = _mm_alignr_epi8(lA, *(__m128i *)(p + stride * 8), 14);
	__m128i l4 = _mm_alignr_epi8(l3, *(__m128i *)(p += stride), 14);
	__m128i lC = _mm_alignr_epi8(lB, *(__m128i *)(p + stride * 8), 14);
	__m128i l5 = _mm_alignr_epi8(l4, *(__m128i *)(p += stride), 14);
	__m128i lD = _mm_alignr_epi8(lC, *(__m128i *)(p + stride * 8), 14);
	__m128i l6 = _mm_alignr_epi8(l5, *(__m128i *)(p += stride), 14);
	__m128i lE = _mm_alignr_epi8(lD, *(__m128i *)(p += stride * 8), 14);
	__m128i l7 = _mm_shuffle_epi8(l6, (__m128i)inv);
	__m128i lF = _mm_alignr_epi8(lE, *(__m128i *)(p += stride), 14);
	
	// sum them and compute a, b, c
	__m128i x0 = _mm_madd_epi16(_mm_sub_epi16(t1, t0), (__m128i)mul0);
	__m128i x1 = _mm_madd_epi16(_mm_sub_epi16(lF, l7), (__m128i)mul0);
	__m128i x2 = _mm_hadd_epi32(x0, x1);
	__m128i HV = _mm_add_epi32(x2, _mm_shuffle_epi32(x2, _MM_SHUFFLE(2, 3, 0, 1)));
	__m128i x3 = _mm_srai_epi32(_mm_add_epi32(HV, (__m128i)s32), 6);
	__m128i a = _mm_set1_epi32((p15 + p[15] + 1) * 16);
	__m128i b = _mm_shuffle_epi32(x3, _MM_SHUFFLE(1, 0, 1, 0));
	__m128i c = _mm_shuffle_epi32(x3, _MM_SHUFFLE(3, 2, 3, 2));
	
	// compute the first row of prediction vectors
	((__m128i *)ctx->pred_buffer)[16] = c;
	__m128i x4 = _mm_sub_epi32(_mm_add_epi32(a, c), _mm_slli_epi32(c, 3)); // a - c * 7 + 16
	__m128i p0 = _mm_add_epi32(_mm_mullo_epi32(b, (__m128i)mul1), x4);
	__m128i p1 = _mm_add_epi32(_mm_mullo_epi32(b, (__m128i)mul2), x4);
	__m128i p2 = _mm_add_epi32(_mm_mullo_epi32(b, (__m128i)mul3), x4);
	__m128i p3 = _mm_add_epi32(_mm_mullo_epi32(b, (__m128i)mul4), x4);
	
	// store them
	((__m128i *)ctx->pred_buffer)[0] = p0;
	((__m128i *)ctx->pred_buffer)[1] = p1;
	((__m128i *)ctx->pred_buffer)[4] = p2;
	((__m128i *)ctx->pred_buffer)[5] = p3;
	((__m128i *)ctx->pred_buffer)[2] = p0 = _mm_add_epi32(p0, c);
	((__m128i *)ctx->pred_buffer)[3] = p1 = _mm_add_epi32(p1, c);
	((__m128i *)ctx->pred_buffer)[6] = p2 = _mm_add_epi32(p2, c);
	((__m128i *)ctx->pred_buffer)[7] = p3 = _mm_add_epi32(p3, c);
	((__m128i *)ctx->pred_buffer)[8] = p0 = _mm_add_epi32(p0, c);
	((__m128i *)ctx->pred_buffer)[9] = p1 = _mm_add_epi32(p1, c);
	((__m128i *)ctx->pred_buffer)[12] = p2 = _mm_add_epi32(p2, c);
	((__m128i *)ctx->pred_buffer)[13] = p3 = _mm_add_epi32(p3, c);
	((__m128i *)ctx->pred_buffer)[10] = _mm_add_epi32(p0, c);
	((__m128i *)ctx->pred_buffer)[11] = _mm_add_epi32(p1, c);
	((__m128i *)ctx->pred_buffer)[14] = _mm_add_epi32(p2, c);
	((__m128i *)ctx->pred_buffer)[15] = _mm_add_epi32(p3, c);
	return 0;
}



static int predict_Plane8x16_8bit(uint8_t *p, size_t stride)
{
	static const v16qi mul0 = {-8, -6, -4, -2, 0, 2, 4, 6, 8, 0, 0, 0, 0, 0, 0, 0};
	static const v16qi mul1 = {8, 7, 6, 5, 4, 3, 2, 1, -1, -2, -3, -4, -5, -6, -7, -8};
	static const v8hi mul2 = {-3, -2, -1, 0, 1, 2, 3, 4};
	static const v8hi h2 = {2, 2, 2, 2, 2, 2, 2, 2};
	static const v8hi h8 = {8, 8, 8, 8, 8, 8, 8, 8};
	
	// load all neighbouring samples
	int p7 = p[7];
	__m128i top = _mm_loadu_si128((__m128i *)(p - 1));
	__m64 l0 = _mm_alignr_pi8(_mm_movepi64_pi64(top), *(__m64 *)(p += stride - 8), 7);
	__m64 l8 = *(__m64 *)(p + stride * 8 + 7);
	__m64 l1 = _mm_alignr_pi8(l0, *(__m64 *)(p += stride), 7);
	__m64 l9 = _mm_alignr_pi8(l8, *(__m64 *)(p + stride * 8), 7);
	__m64 l2 = _mm_alignr_pi8(l1, *(__m64 *)(p += stride), 7);
	__m64 lA = _mm_alignr_pi8(l9, *(__m64 *)(p + stride * 8), 7);
	__m64 l3 = _mm_alignr_pi8(l2, *(__m64 *)(p += stride), 7);
	__m64 lB = _mm_alignr_pi8(lA, *(__m64 *)(p + stride * 8), 7);
	__m64 l4 = _mm_alignr_pi8(l3, *(__m64 *)(p += stride), 7);
	__m64 lC = _mm_alignr_pi8(lB, *(__m64 *)(p + stride * 8), 7);
	__m64 l5 = _mm_alignr_pi8(l4, *(__m64 *)(p += stride), 7);
	__m64 lD = _mm_alignr_pi8(lC, *(__m64 *)(p + stride * 8), 7);
	__m64 l6 = _mm_alignr_pi8(l5, *(__m64 *)(p += stride), 7);
	__m64 lE = _mm_alignr_pi8(lD, *(__m64 *)(p += stride * 8), 7);
	__m64 lF = _mm_alignr_pi8(lE, *(__m64 *)(p += stride), 7);
	__m128i left = _mm_set_epi64(l6, lF);
	
	// sum them and compute a, b, c (with care for overflow)
	__m128i x0 = _mm_maddubs_epi16(top, (__m128i)mul0);
	__m128i x1 = _mm_maddubs_epi16(left, (__m128i)mul1);
	__m128i x2 = _mm_hadd_epi16(x1, x0);
	__m128i x3 = _mm_add_epi16(x2, _mm_shuffle_epi32(x2, _MM_SHUFFLE(2, 3, 0, 1)));
	__m128i HV = _mm_hadd_epi16(x3, x3); // VVHHVVHH, 15 significant bits
	__m128i H = _mm_shuffle_epi32(HV, _MM_SHUFFLE(3, 3, 1, 1));
	__m128i V = _mm_shuffle_epi32(HV, _MM_SHUFFLE(2, 2, 0, 0));
	__m128i x4 = _mm_add_epi16(H, _mm_srai_epi16(HV, 4)); // (34 * H) >> 4
	__m128i x5 = _mm_add_epi16(V, _mm_srai_epi16(HV, 2)); // (5 * V) >> 2
	__m128i a = _mm_set1_epi16((p7 + p[7] + 1) * 16); // 12 significant bits
	__m128i b = _mm_srai_epi16(_mm_add_epi16(x4, (__m128i)h2), 2);
	__m128i c = _mm_srai_epi16(_mm_add_epi16(x5, (__m128i)h8), 4);
	
	// compute the first row of prediction vectors
	__m128i c1 = _mm_slli_epi16(c, 1);
	__m128i c2 = _mm_slli_epi16(c, 2);
	((__m128i *)ctx->pred_buffer)[16] = c1;
	__m128i x6 = _mm_sub_epi16(_mm_sub_epi16(a, c), _mm_add_epi16(c1, c2)); // a - c * 7 + 16
	__m128i x7 = _mm_add_epi16(_mm_mullo_epi16(b, (__m128i)mul2), x6);
	__m128i x8 = _mm_add_epi16(x7, c);
	__m128i p0 = _mm_unpacklo_epi64(x7, x8);
	__m128i p1 = _mm_unpackhi_epi64(x7, x8);
	
	// store them
	((__m128i *)ctx->pred_buffer)[0] = p0;
	((__m128i *)ctx->pred_buffer)[1] = p1;
	((__m128i *)ctx->pred_buffer)[2] = p0 = _mm_add_epi16(p0, c2);
	((__m128i *)ctx->pred_buffer)[3] = p1 = _mm_add_epi16(p1, c2);
	((__m128i *)ctx->pred_buffer)[4] = p0 = _mm_add_epi16(p0, c2);
	((__m128i *)ctx->pred_buffer)[5] = p1 = _mm_add_epi16(p1, c2);
	((__m128i *)ctx->pred_buffer)[6] = _mm_add_epi16(p0, c2);
	((__m128i *)ctx->pred_buffer)[7] = _mm_add_epi16(p1, c2);
	return 0;
}



static int predict_Plane8x8_8bit(uint8_t *p, size_t stride)
{
	static const v16qi shuf = {0, 1, 2, 3, 4, 5, 6, 7, 7, 8, 9, 10, 12, 13, 14, 15};
	static const v16qi mul0 = {8, 6, 4, 2, -2, -4, -6, -8, -8, -6, -4, -2, 2, 4, 6, 8};
	static const v8hi mul1 = {-3, -2, -1, 0, 1, 2, 3, 4};
	static const v8hi h2 = {2, 2, 2, 2, 2, 2, 2, 2};
	
	// load all neighbouring samples
	int p7 = p[7];
	__m64 top = *(__m64 *)p;
	__m64 l0 = *(__m64 *)(p - 1);
	__m64 l1 = _mm_alignr_pi8(l0, *(__m64 *)(p += stride - 8), 7);
	__m64 l2 = _mm_alignr_pi8(l1, *(__m64 *)(p += stride), 7);
	__m64 l3 = _mm_alignr_pi8(l2, *(__m64 *)(p += stride), 7);
	__m64 l4 = _mm_alignr_pi8(l3, *(__m64 *)(p += stride * 2), 7);
	__m64 l5 = _mm_alignr_pi8(l4, *(__m64 *)(p += stride), 7);
	__m64 l6 = _mm_alignr_pi8(l5, *(__m64 *)(p += stride), 7);
	__m64 left = _mm_alignr_pi8(l6, *(__m64 *)(p += stride), 7);
	__m128i lt = _mm_shuffle_epi8(_mm_set_epi64(top, left), (__m128i)shuf);
	
	// sum them and compute a, b, c (with care for overflow)
	__m128i x0 = _mm_maddubs_epi16(lt, (__m128i)mul0);
	__m128i x1 = _mm_add_epi16(x0, _mm_shuffle_epi32(x0, _MM_SHUFFLE(2, 3, 0, 1)));
	__m128i HV = _mm_hadd_epi16(x1, x1); // 2 * VVHHVVHH, 14 significant bits
	__m128i x2 = _mm_add_epi16(HV, _mm_srai_epi16(HV, 4)); // (34 * HV) >> 4
	__m128i x3 = _mm_srai_epi16(_mm_add_epi16(x2, (__m128i)h2), 2); // (34 * HV + 32) >> 6
	__m128i a = _mm_set1_epi16((p7 + p[7] + 1) * 16); // 12 significant bits
	__m128i b = _mm_shuffle_epi32(x3, _MM_SHUFFLE(3, 3, 1, 1)); // 12 significant bits
	__m128i c = _mm_shuffle_epi32(x3, _MM_SHUFFLE(2, 2, 0, 0));
	
	// compute the first row of prediction vectors
	__m128i c1 = _mm_slli_epi16(c, 1);
	((__m128i *)ctx->pred_buffer)[16] = c1;
	__m128i x4 = _mm_sub_epi16(_mm_sub_epi16(a, c), c1); // a - c * 3 + 16
	__m128i x5 = _mm_add_epi16(_mm_mullo_epi16(b, (__m128i)mul1), x4);
	__m128i x6 = _mm_add_epi16(x5, c);
	__m128i p0 = _mm_unpacklo_epi64(x5, x6);
	__m128i p1 = _mm_unpackhi_epi64(x5, x6);
	
	// store them
	__m128i c2 = _mm_slli_epi16(c, 2);
	((__m128i *)ctx->pred_buffer)[0] = p0;
	((__m128i *)ctx->pred_buffer)[1] = p1;
	((__m128i *)ctx->pred_buffer)[2] = _mm_add_epi16(p0, c2);
	((__m128i *)ctx->pred_buffer)[3] = _mm_add_epi16(p1, c2);
	return 0;
}



static __attribute__((noinline)) int decode_8bit(uint8_t *p, size_t stride, int mode, int BitDepth, __m128i zero)
{
	static const v16qi shufC = {0, -1, 1, -1, 2, -1, 3, -1, 3, -1, 3, -1, 3, -1, 3, -1};
	__m64 m0, m1, m2, m3, m4;
	__m128i x0, x1, x2, x3, x4;
	
	__builtin_expect(mode <= HORIZONTAL_UP_8x8, 1);
	switch (mode) {
	
	// Intra4x4 modes
	case VERTICAL_4x4:
		x0 = _mm_unpacklo_epi8(_mm_set1_epi32(*(int32_t *)p), zero);
		return decode_Residual4x4(x0, x0);
	case HORIZONTAL_4x4:
		return decode_Horizontal4x4_8bit(p, stride);
	case DC_4x4:
		m0 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 4 - 4), *(__m64 *)(p + stride * 3 - 4));
		m1 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 2 - 4), *(__m64 *)(p + stride * 1 - 4));
		m2 = _mm_unpackhi_pi32(_mm_unpackhi_pi16(m0, m1), *(__m64 *)(p - 4));
		return decode_DC4x4_8bit(m2);
	case DC_A_4x4:
		return decode_DC4x4_8bit(_mm_set1_pi32(*(int32_t *)p));
	case DC_B_4x4:
		m0 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 4 - 4), *(__m64 *)(p + stride * 3 - 4));
		m1 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 2 - 4), *(__m64 *)(p + stride * 1 - 4));
		m2 = _mm_shuffle_pi16(_mm_unpackhi_pi16(m0, m1), _MM_SHUFFLE(3, 2, 3, 2));
		return decode_DC4x4_8bit(m2);
	case DC_AB_4x4:
		return decode_DC4x4_8bit(_mm_set1_pi8(128));
	case DIAGONAL_DOWN_LEFT_4x4:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		return decode_DiagonalDownLeft4x4(x0);
	case DIAGONAL_DOWN_LEFT_C_4x4:
		x0 = _mm_shuffle_epi8(_mm_cvtsi64_si128(*(int64_t *)p), (__m128i)shufC);
		return decode_DiagonalDownLeft4x4(x0);
	case DIAGONAL_DOWN_RIGHT_4x4:
		m0 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 3 - 4), *(__m64 *)(p + stride * 2 - 4));
		m1 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 1 - 4), *(__m64 *)(p + stride * 0 - 4));
		m2 = _mm_unpackhi_pi32(_mm_unpackhi_pi16(m0, m1), *(__m64 *)(p - 4));
		x0 = _mm_unpacklo_epi8(_mm_movpi64_epi64(m2), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 4 - 8)), zero);
		return decode_DiagonalDownRight4x4(x0, x1);
	case VERTICAL_RIGHT_4x4:
		m0 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 3 - 4), *(__m64 *)(p + stride * 2 - 4));
		m1 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 1 - 4), *(__m64 *)(p + stride * 0 - 4));
		m2 = _mm_unpackhi_pi32(_mm_unpackhi_pi16(m0, m1), *(__m64 *)(p - 4));
		x0 = _mm_unpacklo_epi8(_mm_movpi64_epi64(m2), zero);
		return decode_VerticalRight4x4(x0);
	case HORIZONTAL_DOWN_4x4:
		m0 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 4 - 4), *(__m64 *)(p + stride * 3 - 4));
		m1 = _mm_unpacklo_pi8(*(__m64 *)(p + stride * 2 - 4), *(__m64 *)(p + stride * 1 - 4));
		m2 = _mm_unpackhi_pi32(_mm_unpackhi_pi16(m0, m1), *(__m64 *)(p - 5));
		x0 = _mm_unpacklo_epi8(_mm_movpi64_epi64(m2), zero);
		return decode_HorizontalDown4x4(x0);
	case VERTICAL_LEFT_4x4:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		return decode_VerticalLeft4x4(x0);
	case VERTICAL_LEFT_C_4x4:
		x0 = _mm_shuffle_epi8(_mm_cvtsi64_si128(*(int64_t *)p), (__m128i)shufC);
		return decode_VerticalLeft4x4(x0);
	case HORIZONTAL_UP_4x4:
		return decode_HorizontalUp4x4_8bit(p, stride);
	
	// Intra8x8 modes
	case VERTICAL_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 1)), zero);
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p - 1)), zero);
		return decode_Vertical8x8(x1, x0, x2);
	case VERTICAL_C_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p - 1)), zero);
		return decode_Vertical8x8(x1, x0, x2);
	case VERTICAL_D_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 1)), zero);
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_Vertical8x8(x1, x0, x2);
	case VERTICAL_CD_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_Vertical8x8(x1, x0, x2);
	
	case HORIZONTAL_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x1 = load8_8bit(p, stride, p);
		return decode_Horizontal8x8(x1, x0);
	case HORIZONTAL_D_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x1 = load8_8bit(p, stride, p + stride);
		return decode_Horizontal8x8(x1, x0);
	
	case DC_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 1)), zero);
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p - 1)), zero);
		x3 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x4 = load8_8bit(p, stride, p);
		return decode_DC8x8(x1, x0, x2, x3, x4, x4);
	case DC_C_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p - 1)), zero);
		x3 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x4 = load8_8bit(p, stride, p);
		return decode_DC8x8(x1, x0, x2, x3, x4, x4);
	case DC_D_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 1)), zero);
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		x3 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x4 = load8_8bit(p, stride, p + stride);
		return decode_DC8x8(x1, x0, x2, x3, x4, x4);
	case DC_CD_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		x3 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x4 = load8_8bit(p, stride, p + stride);
		return decode_DC8x8(x1, x0, x2, x3, x4, x4);
	case DC_A_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 1)), zero);
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p - 1)), zero);
		x3 = _mm_shuffle_epi32(x2, _MM_SHUFFLE(0, 0, 0, 0));
		x4 = _mm_slli_si128(x2, 14);
		return decode_DC8x8(x1, x0, x2, x1, x3, x4);
	case DC_AC_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p - 1)), zero);
		x3 = _mm_shuffle_epi32(x2, _MM_SHUFFLE(0, 0, 0, 0));
		x4 = _mm_slli_si128(x2, 14);
		return decode_DC8x8(x1, x0, x2, x1, x3, x4);
	case DC_AD_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 1)), zero);
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		x3 = _mm_shuffle_epi32(x2, _MM_SHUFFLE(0, 0, 0, 0));
		x4 = _mm_slli_si128(x2, 14);
		return decode_DC8x8(x1, x0, x2, x1, x3, x4);
	case DC_ACD_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		x3 = _mm_shuffle_epi32(x2, _MM_SHUFFLE(0, 0, 0, 0));
		x4 = _mm_slli_si128(x2, 14);
		return decode_DC8x8(x1, x0, x2, x1, x3, x4);
	case DC_B_8x8:
		x3 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x4 = load8_8bit(p, stride, p);
		x0 = _mm_alignr_epi8(x4, x3, 14);
		x1 = _mm_alignr_epi8(x0, x3, 14);
		return decode_DC8x8(x4, x0, x1, x4, x3, x3);
	case DC_BD_8x8:
		x3 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x4 = load8_8bit(p, stride, p + stride);
		x0 = _mm_alignr_epi8(x4, x3, 14);
		x1 = _mm_alignr_epi8(x0, x3, 14);
		return decode_DC8x8(x4, x0, x1, x4, x3, x3);
	case DC_AB_8x8:
		x0 = _mm_set1_epi16(128);
		return decode_DC8x8(x0, x0, x0, x0, x0, x0);
	
	case DIAGONAL_DOWN_LEFT_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 8)), zero);
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p - 1)), zero);
		return decode_DiagonalDownLeft8x8(x1, x0, x2);
	case DIAGONAL_DOWN_LEFT_C_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shuffle_epi32(_mm_shufflehi_epi16(x0, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p - 1)), zero);
		return decode_DiagonalDownLeft8x8(x1, x0, x2);
	case DIAGONAL_DOWN_LEFT_D_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 8)), zero);
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_DiagonalDownLeft8x8(x1, x0, x2);
	case DIAGONAL_DOWN_LEFT_CD_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shuffle_epi32(_mm_shufflehi_epi16(x0, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_DiagonalDownLeft8x8(x1, x0, x2);
	
	case DIAGONAL_DOWN_RIGHT_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 1)), zero);
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x3 = load8_8bit(p, stride, p);
		return decode_DiagonalDownRight8x8(x1, x0, x3, x2);
	case DIAGONAL_DOWN_RIGHT_C_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x3 = load8_8bit(p, stride, p);
		return decode_DiagonalDownRight8x8(x1, x0, x3, x2);
	
	case VERTICAL_RIGHT_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 1)), zero);
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x3 = load8_8bit(p, stride, p);
		return decode_VerticalRight8x8(x1, x0, x3, x2);
	case VERTICAL_RIGHT_C_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x3 = load8_8bit(p, stride, p);
		return decode_VerticalRight8x8(x1, x0, x3, x2);
	
	case HORIZONTAL_DOWN_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + stride * 8 - 8)), zero);
		x2 = load8_8bit(p, stride, p);
		return decode_HorizontalDown8x8(x0, x2, x1);
	
	case VERTICAL_LEFT_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 8)), zero);
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p - 1)), zero);
		return decode_VerticalLeft8x8(x1, x0, x2);
	case VERTICAL_LEFT_C_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shuffle_epi32(_mm_shufflehi_epi16(x0, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
		x2 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p - 1)), zero);
		return decode_VerticalLeft8x8(x1, x0, x2);
	case VERTICAL_LEFT_D_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p + 8)), zero);
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_VerticalLeft8x8(x1, x0, x2);
	case VERTICAL_LEFT_CD_8x8:
		x0 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)p), zero);
		x1 = _mm_shuffle_epi32(_mm_shufflehi_epi16(x0, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_VerticalLeft8x8(x1, x0, x2);
	
	case HORIZONTAL_UP_8x8:
		m0 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 1 - 8), *(__m64 *)(p + stride * 2 - 8));
		m1 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 3 - 8), *(__m64 *)(p + stride * 4 - 8));
		m2 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 5 - 8), *(__m64 *)(p + stride * 6 - 8));
		m3 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 7 - 8), *(__m64 *)(p + stride * 8 - 8));
		m4 = _mm_unpackhi_pi32(_mm_unpackhi_pi16(m0, m1), _mm_unpackhi_pi16(m2, m3));
		x0 = _mm_unpacklo_epi8(_mm_movpi64_epi64(m4), zero);
		x1 = _mm_unpacklo_epi8(_mm_cvtsi64_si128(*(int64_t *)(p - 8)), zero);
		return decode_HorizontalUp8x8(x0, x1);
	case HORIZONTAL_UP_D_8x8:
		m0 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 1 - 8), *(__m64 *)(p + stride * 2 - 8));
		m1 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 3 - 8), *(__m64 *)(p + stride * 4 - 8));
		m2 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 5 - 8), *(__m64 *)(p + stride * 6 - 8));
		m3 = _mm_unpackhi_pi8(*(__m64 *)(p + stride * 7 - 8), *(__m64 *)(p + stride * 8 - 8));
		m4 = _mm_unpackhi_pi32(_mm_unpackhi_pi16(m0, m1), _mm_unpackhi_pi16(m2, m3));
		x0 = _mm_unpacklo_epi8(_mm_movpi64_epi64(m4), zero);
		x1 = _mm_slli_si128(x0, 14);
		return decode_HorizontalUp8x8(x0, x1);
	
	// Intra16x16 and chroma modes
	case PLANE_16x16:
		x0 = (__m128i)ctx->pred_buffer[ctx->BlkIdx];
		x1 = _mm_add_epi16(x0, (__m128i)ctx->pred_buffer[16]);
		x2 = _mm_packus_epi16(_mm_srai_epi16(x0, 5), _mm_srai_epi16(x1, 5));
		x3 = _mm_unpacklo_epi8(x2, zero);
		x4 = _mm_unpackhi_epi8(x2, zero);
		return decode_Residual4x4(x3, x4);
	}
	return 0;
}



static __attribute__((noinline)) int decode_16bit(uint8_t *p, size_t stride, int mode, int BitDepth, __m128i zero)
{
	__m64 m0, m1, m2, m3, m4, m5;
	__m128i x0, x1, x2, x3, x4, x5, x6, x7;
	
	__builtin_expect(mode <= HORIZONTAL_UP_8x8, 1);
	switch (mode) {
	
	// Intra4x4 modes
	case VERTICAL_4x4:
		x0 = _mm_set1_epi64(*(__m64 *)p);
		return decode_Residual4x4(x0, x0);
	case HORIZONTAL_4x4:
		return decode_Horizontal4x4_16bit(p, stride);
	case DC_4x4:
		m0 = *(__m64 *)p;
		m1 = _mm_hadd_pi16(m0, m0);
		m2 = _mm_hadd_pi16(m1, m1);
		m3 = _mm_add_pi16(*(__m64 *)(p + stride - 8), *(__m64 *)(p + stride * 2 - 8));
		m4 = _mm_add_pi16(m3, *(__m64 *)(p + stride * 3 - 8));
		m5 = _mm_add_pi16(m4, *(__m64 *)(p + stride * 4 - 8));
		return decode_DC4x4_16bit(m2, m5);
	case DC_A_4x4:
		m0 = *(__m64 *)p;
		m1 = _mm_hadd_pi16(m0, m0);
		m2 = _mm_hadd_pi16(m1, m1);
		return decode_DC4x4_16bit(m2, m2);
	case DC_B_4x4:
		m3 = _mm_add_pi16(*(__m64 *)(p + stride - 8), *(__m64 *)(p + stride * 2 - 8));
		m4 = _mm_add_pi16(m3, *(__m64 *)(p + stride * 3 - 8));
		m5 = _mm_add_pi16(m4, *(__m64 *)(p + stride * 4 - 8));
		return decode_DC4x4_16bit(m5, m5);
	case DC_AB_4x4:
		m0 = _mm_set1_pi16(2 << BitDepth);
		return decode_DC4x4_16bit(m0, m0);
	case DIAGONAL_DOWN_LEFT_4x4:
		return decode_DiagonalDownLeft4x4(_mm_loadu_si128((__m128i *)p));
	case DIAGONAL_DOWN_LEFT_C_4x4:
		x0 = _mm_shufflehi_epi16(_mm_set1_epi64(*(__m64 *)p), _MM_SHUFFLE(3, 3, 3, 3));
		return decode_DiagonalDownLeft4x4(x0);
	case DIAGONAL_DOWN_RIGHT_4x4:
		m0 = _mm_unpackhi_pi16(*(__m64 *)(p + stride * 3 - 8), *(__m64 *)(p + stride * 2 - 8));
		m1 = _mm_unpackhi_pi16(*(__m64 *)(p + stride * 1 - 8), *(__m64 *)(p + stride * 0 - 8));
		x0 = _mm_set_epi64(*(__m64 *)p, _mm_unpackhi_pi32(m0, m1));
		x1 = _mm_cvtsi64_si128(*(int64_t *)(p + stride * 4 - 8));
		return decode_DiagonalDownRight4x4(x0, x1);
	case VERTICAL_RIGHT_4x4:
		m0 = _mm_unpackhi_pi16(*(__m64 *)(p + stride * 3 - 8), *(__m64 *)(p + stride * 2 - 8));
		m1 = _mm_unpackhi_pi16(*(__m64 *)(p + stride * 1 - 8), *(__m64 *)(p - stride * 0 - 8));
		x0 = _mm_set_epi64(*(__m64 *)p, _mm_unpackhi_pi32(m0, m1));
		return decode_VerticalRight4x4(x0);
	case HORIZONTAL_DOWN_4x4:
		m0 = _mm_unpackhi_pi16(*(__m64 *)(p + stride * 4 - 8), *(__m64 *)(p + stride * 3 - 8));
		m1 = _mm_unpackhi_pi16(*(__m64 *)(p + stride * 2 - 8), *(__m64 *)(p + stride * 1 - 8));
		x0 = _mm_set_epi64(*(__m64 *)(p - 2), _mm_unpackhi_pi32(m0, m1));
		return decode_HorizontalDown4x4(x0);
	case VERTICAL_LEFT_4x4:
		return decode_VerticalLeft4x4(_mm_loadu_si128((__m128i *)p));
	case VERTICAL_LEFT_C_4x4:
		x0 = _mm_shufflehi_epi16(_mm_set1_epi64(*(__m64 *)p), _MM_SHUFFLE(3, 3, 3, 3));
		return decode_VerticalLeft4x4(x0);
	case HORIZONTAL_UP_4x4:
		return decode_HorizontalUp4x4_16bit(p, stride);
	
	// Intra8x8 modes
	case VERTICAL_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_lddqu_si128((__m128i *)(p + 2));
		x2 = _mm_lddqu_si128((__m128i *)(p - 2));
		return decode_Vertical8x8(x1, x0, x2);
	case VERTICAL_C_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_lddqu_si128((__m128i *)(p - 2));
		return decode_Vertical8x8(x1, x0, x2);
	case VERTICAL_D_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_lddqu_si128((__m128i *)(p + 2));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_Vertical8x8(x1, x0, x2);
	case VERTICAL_CD_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_Vertical8x8(x1, x0, x2);
	
	case HORIZONTAL_8x8:
		x0 = *(__m128i *)(p + stride * 8 - 16);
		x1 = load8_16bit(p, stride, p);
		return decode_Horizontal8x8(x1, x0);
	case HORIZONTAL_D_8x8:
		x0 = *(__m128i *)(p + stride * 8 - 16);
		x1 = load8_16bit(p, stride, p + stride);
		return decode_Horizontal8x8(x1, x0);
	
	case DC_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_lddqu_si128((__m128i *)(p + 2));
		x2 = _mm_lddqu_si128((__m128i *)(p - 2));
		x3 = *(__m128i *)(p + stride * 8 - 16);
		x4 = load8_16bit(p, stride, p);
		return decode_DC8x8(x1, x0, x2, x3, x4, x4);
	case DC_C_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_lddqu_si128((__m128i *)(p - 2));
		x3 = *(__m128i *)(p + stride * 8 - 16);
		x4 = load8_16bit(p, stride, p);
		return decode_DC8x8(x1, x0, x2, x3, x4, x4);
	case DC_D_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_lddqu_si128((__m128i *)(p + 2));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		x3 = *(__m128i *)(p + stride * 8 - 16);
		x4 = load8_16bit(p, stride, p);
		return decode_DC8x8(x1, x0, x2, x3, x4, x4);
	case DC_CD_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		x3 = *(__m128i *)(p + stride * 8 - 16);
		x4 = load8_16bit(p, stride, p);
		return decode_DC8x8(x1, x0, x2, x3, x4, x4);
	case DC_A_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_lddqu_si128((__m128i *)(p + 2));
		x2 = _mm_lddqu_si128((__m128i *)(p - 2));
		x3 = _mm_shuffle_epi32(x2, _MM_SHUFFLE(0, 0, 0, 0));
		x4 = _mm_slli_si128(x2, 14);
		return decode_DC8x8(x1, x0, x2, x1, x3, x4);
	case DC_AC_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_lddqu_si128((__m128i *)(p - 2));
		x3 = _mm_shuffle_epi32(x2, _MM_SHUFFLE(0, 0, 0, 0));
		x4 = _mm_slli_si128(x2, 14);
		return decode_DC8x8(x1, x0, x2, x1, x3, x4);
	case DC_AD_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_lddqu_si128((__m128i *)(p + 2));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		x3 = _mm_shuffle_epi32(x2, _MM_SHUFFLE(0, 0, 0, 0));
		x4 = _mm_slli_si128(x2, 14);
		return decode_DC8x8(x1, x0, x2, x1, x3, x4);
	case DC_ACD_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		x3 = _mm_shuffle_epi32(x2, _MM_SHUFFLE(0, 0, 0, 0));
		x4 = _mm_slli_si128(x2, 14);
		return decode_DC8x8(x1, x0, x2, x1, x3, x4);
	case DC_B_8x8:
		x3 = *(__m128i *)(p + stride * 8 - 16);
		x4 = load8_16bit(p, stride, p);
		x0 = _mm_alignr_epi8(x4, x3, 14);
		x1 = _mm_alignr_epi8(x0, x3, 14);
		return decode_DC8x8(x4, x0, x1, x4, x3, x3);
	case DC_BD_8x8:
		x3 = *(__m128i *)(p + stride * 8 - 16);
		x4 = load8_16bit(p, stride, p + stride);
		x0 = _mm_alignr_epi8(x4, x3, 14);
		x1 = _mm_alignr_epi8(x0, x3, 14);
		return decode_DC8x8(x4, x0, x1, x4, x3, x3);
	case DC_AB_8x8:
		x0 = _mm_set1_epi16(128);
		return decode_DC8x8(x0, x0, x0, x0, x0, x0);
	
	case DIAGONAL_DOWN_LEFT_8x8:
		x0 = *(__m128i *)p;
		x1 = *(__m128i *)(p + 16);
		x2 = _mm_lddqu_si128((__m128i *)(p - 2));
		return decode_DiagonalDownLeft8x8(x1, x0, x2);
	case DIAGONAL_DOWN_LEFT_C_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shuffle_epi32(_mm_shufflehi_epi16(x0, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
		x2 = _mm_lddqu_si128((__m128i *)(p - 2));
		return decode_DiagonalDownLeft8x8(x1, x0, x2);
	case DIAGONAL_DOWN_LEFT_D_8x8:
		x0 = *(__m128i *)p;
		x1 = *(__m128i *)(p + 16);
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_DiagonalDownLeft8x8(x1, x0, x2);
	case DIAGONAL_DOWN_LEFT_CD_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shuffle_epi32(_mm_shufflehi_epi16(x0, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_DiagonalDownLeft8x8(x1, x0, x2);
	
	case DIAGONAL_DOWN_RIGHT_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_lddqu_si128((__m128i *)(p + 2));
		x2 = *(__m128i *)(p + stride * 8 - 16);
		x3 = load8_16bit(p, stride, p);
		return decode_DiagonalDownRight8x8(x1, x0, x3, x2);
	case DIAGONAL_DOWN_RIGHT_C_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = *(__m128i *)(p + stride * 8 - 16);
		x3 = load8_16bit(p, stride, p);
		return decode_DiagonalDownRight8x8(x1, x0, x3, x2);
	
	case VERTICAL_RIGHT_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_lddqu_si128((__m128i *)(p + 2));
		x2 = *(__m128i *)(p + stride * 8 - 16);
		x3 = load8_16bit(p, stride, p);
		return decode_VerticalRight8x8(x1, x0, x3, x2);
	case VERTICAL_RIGHT_C_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shufflehi_epi16(_mm_srli_si128(x0, 2), _MM_SHUFFLE(2, 2, 1, 0));
		x2 = *(__m128i *)(p + stride * 8 - 16);
		x3 = load8_16bit(p, stride, p);
		return decode_VerticalRight8x8(x1, x0, x3, x2);
	
	case HORIZONTAL_DOWN_8x8:
		x0 = *(__m128i *)p;
		x1 = *(__m128i *)(p + stride * 8 - 16);
		x2 = load8_16bit(p, stride, p);
		return decode_HorizontalDown8x8(x0, x2, x1);
	
	case VERTICAL_LEFT_8x8:
		x0 = *(__m128i *)p;
		x1 = *(__m128i *)(p + 16);
		x2 = _mm_lddqu_si128((__m128i *)(p - 2));
		return decode_VerticalLeft8x8(x1, x0, x2);
	case VERTICAL_LEFT_C_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shuffle_epi32(_mm_shufflehi_epi16(x0, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
		x2 = _mm_lddqu_si128((__m128i *)(p - 2));
		return decode_VerticalLeft8x8(x1, x0, x2);
	case VERTICAL_LEFT_D_8x8:
		x0 = *(__m128i *)p;
		x1 = *(__m128i *)(p + 16);
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_VerticalLeft8x8(x1, x0, x2);
	case VERTICAL_LEFT_CD_8x8:
		x0 = *(__m128i *)p;
		x1 = _mm_shuffle_epi32(_mm_shufflehi_epi16(x0, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
		x2 = _mm_shufflelo_epi16(_mm_slli_si128(x0, 2), _MM_SHUFFLE(3, 2, 1, 1));
		return decode_VerticalLeft8x8(x1, x0, x2);
	
	case HORIZONTAL_UP_8x8:
		x0 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 1 - 16), *(__m128i *)(p + stride * 2 - 16));
		x1 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 3 - 16), *(__m128i *)(p + stride * 4 - 16));
		x2 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 5 - 16), *(__m128i *)(p + stride * 6 - 16));
		x3 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 7 - 16), *(__m128i *)(p + stride * 8 - 16));
		x4 = _mm_unpackhi_epi64(_mm_unpackhi_epi32(x0, x1), _mm_unpackhi_epi32(x2, x3));
		x5 = *(__m128i *)(p - 16);
		return decode_HorizontalUp8x8(x4, x5);
	case HORIZONTAL_UP_D_8x8:
		x0 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 1 - 16), *(__m128i *)(p + stride * 2 - 16));
		x1 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 3 - 16), *(__m128i *)(p + stride * 4 - 16));
		x2 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 5 - 16), *(__m128i *)(p + stride * 6 - 16));
		x3 = _mm_unpackhi_epi16(*(__m128i *)(p + stride * 7 - 16), *(__m128i *)(p + stride * 8 - 16));
		x4 = _mm_unpackhi_epi64(_mm_unpackhi_epi32(x0, x1), _mm_unpackhi_epi32(x2, x3));
		x5 = _mm_slli_si128(x4, 14);
		return decode_HorizontalUp8x8(x4, x5);
	
	// Intra16x16 and chroma modes
	case PLANE_16x16:
		x0 = (__m128i)ctx->pred_buffer[16];
		x1 = (__m128i)ctx->pred_buffer[ctx->BlkIdx];
		x2 = _mm_add_epi32(x1, x0);
		x3 = _mm_add_epi32(x2, x0);
		x4 = _mm_add_epi32(x3, x0);
		x5 = _mm_add_epi32(x4, x0);
		x6 = _mm_min_epi16(_mm_packus_epi32(_mm_srai_epi32(x2, 5), _mm_srai_epi32(x3, 5)), (__m128i)ctx->cbf_maskA); // FIXME
		x7 = _mm_min_epi16(_mm_packus_epi32(_mm_srai_epi32(x4, 5), _mm_srai_epi32(x5, 5)), (__m128i)ctx->cbf_maskA); // FIXME
		return decode_Residual4x4(x6, x7);
	}
	return 0;
}



inline int decode_samples() {
	int BlkIdx = ctx->BlkIdx;
	int BitDepth = ctx->BitDepth;
	size_t stride = ctx->stride;
	uint8_t *p = ctx->plane + ctx->plane_offsets[BlkIdx] - stride;
	return (BitDepth == 8 ? decode_8bit : decode_16bit)
		(p, stride, ctx->PredMode[BlkIdx], BitDepth, _mm_setzero_si128());
}
