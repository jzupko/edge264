// TODO: Reload values from memory in 8x8 to prevent spills
// TODO: Add pmadd weighting in the functions

#include "edge264_common.h"

int decode_Residual4x4(__m128i, __m128i);
int decode_Residual8x8(__m128i, __m128i, __m128i, __m128i, __m128i, __m128i, __m128i, __m128i);
int decode_Residual8x8_8bit(__m128i, __m128i, __m128i, __m128i, __m128i, __m128i, __m128i, __m128i);
int decode_ResidualDC4x4();
int decode_ResidualDC2x2();
int decode_ResidualDC2x4();
int luma4x4_qpel28(__m128i, __m128i, __m128i, __m128i, __m128i, __m128i);
int ponderation_4x4(__m128i, __m128i);
int ponderation_8x8();
int ponderation_16x16();



/**
 * This function sums opposite pairs of values, and computes (a-5b+20c+16)/32,
 * clipped to [0;sample_max].
 * The sum is actually computed as (((a-b)/4-(b-c))/4+c+1)/2, the last shift
 * being carried after clipping above zero to use pavg.
 */
static inline __attribute__((always_inline)) __m128i filter_6tap(__m128i x0,
	__m128i x1, __m128i x2, __m128i x3, __m128i x4, __m128i x5, __m128i zero)
{
	__m128i a = _mm_add_epi16(x0, x5);
	__m128i b = _mm_add_epi16(x1, x4);
	__m128i c = _mm_add_epi16(x2, x3);
	__m128i x6 = _mm_srai_epi16(_mm_sub_epi16(a, b), 2);
	__m128i x7 = _mm_srai_epi16(_mm_sub_epi16(x6, _mm_sub_epi16(b, c)), 2);
	__m128i x8 = _mm_max_epi16(_mm_add_epi16(c, x7), zero);
	return _mm_min_epi16(_mm_avg_epu16(x8, zero), (__m128i)ctx->clip_Y);
}

static inline __attribute__((always_inline)) __m128i noclip_6tap(__m128i x0,
	__m128i x1, __m128i x2, __m128i x3, __m128i x4, __m128i x5, __m128i zero)
{
	__m128i a = _mm_add_epi16(x0, x5);
	__m128i b = _mm_add_epi16(x1, x4);
	__m128i c = _mm_add_epi16(x2, x3);
	__m128i x6 = _mm_srai_epi16(_mm_sub_epi16(a, b), 2);
	__m128i x7 = _mm_srai_epi16(_mm_sub_epi16(x6, _mm_sub_epi16(b, c)), 2);
	return _mm_avg_epu16(_mm_max_epi16(_mm_add_epi16(c, x7), zero), zero);
}

/**
 * The 2D 6tap filter is carried in two phases.
 * First we shift Up and sum values in one dimension (horizontal or vertical),
 * then we shift Down accumulated values and sum them in the other dimension.
 */
static inline __attribute__((always_inline)) __m128i filter_36tapU_8bit(
	__m128i x0, __m128i x1, __m128i x2, __m128i x3, __m128i x4, __m128i x5)
{
	__m128i a = _mm_add_epi16(x0, x5);
	__m128i b = _mm_add_epi16(x1, x4);
	__m128i c = _mm_add_epi16(x2, x3);
	__m128i x6 = _mm_sub_epi16(a, b);
	__m128i x7 = _mm_sub_epi16(b, c);
	return _mm_add_epi16(x6, _mm_slli_epi16(_mm_sub_epi16(_mm_slli_epi16(c, 2), x7), 2));
}

static inline __attribute__((always_inline)) __m128i filter_36tapU_scalar(
	int s0, int s1, int s2, int s3, int s4, int s5)
{
	// GCC and clang do an amazing job at optimizing this already
	return _mm_cvtsi32_si128((s0+s5) - (s1+s4) * 5 - (s2+s3) * 20);
}

static inline __attribute__((always_inline)) __m128i filter_36tapD_8bit(__m128i x0,
	__m128i x1, __m128i x2, __m128i x3, __m128i x4, __m128i x5, __m128i zero)
{
	__m128i a = _mm_add_epi16(x0, x5);
	__m128i b = _mm_add_epi16(x1, x4);
	__m128i c = _mm_add_epi16(x2, x3);
	__m128i x6 = _mm_srai_epi16(_mm_sub_epi16(a, b), 2);
	__m128i x7 = _mm_srai_epi16(_mm_sub_epi16(x6, _mm_sub_epi16(b, c)), 2);
	__m128i x8 = _mm_max_epi16(_mm_srai_epi16(_mm_add_epi16(c, x7), 5), zero);
	return _mm_min_epi16(_mm_avg_epu16(x8, zero), (__m128i)ctx->clip_Y);
}

/**
 * This is a one-liner function to implement qpel12/21/23/32 atop qpel22, by
 * computing a 6-tap value from an intermediate 36-tap sum, and averaging it
 * with the original qpel22 value.
 */
static inline __attribute__((always_inline)) __m128i avg_6tapD_8bit(
	__m128i sum, __m128i avg, __m128i zero)
{
	__m128i x0 = _mm_avg_epu16(_mm_max_epi16(_mm_srai_epi16(sum, 4), zero), zero);
	return _mm_avg_epu16(_mm_min_epi16(x0, (__m128i)ctx->clip_Y), avg);
}



/**
 * Inter 4x4 prediction takes one (or two) 9x9 matrix of 8/16bit luma samples
 * as input, and outputs a 4x4 matrix of 16bit samples to residual decoding,
 * passed in two 4x2 registers.
 * Loads are generally done by 4x1 matrices, denoted as sRC in the code (R=row,
 * C=left column), or 8x1 matrices, denoted as lRC. Operations are done on 4x2
 * matrices, denoted as mRC. Each one is extracted with a single pshufps on
 * (lR0, l{R+1}0) or (lR1, l{R+1}1). We never read outside the input matrix to
 * avoid additional code/documentation up in the parser.
 *
 * The following approaches were tried for implementing the filters:
 * _ pmadd four rows with [1,-5,20,20,-5,1,0,0,0], [0,1,-5,20,20,-5,1,0,0],
 *   [0,0,1,-5,20,20,-5,1,0] and [0,0,0,1,-5,20,20,-5,1], then phadd and shift
 *   them to get a horizontally-filtered 4x4 matrix. While this is short and
 *   easy to read, both pmadd and phadd are slow even on later architectures.
 * _ doing the 2D filter by accumulating 4x4 matrices by coefficient (out of
 *   1,-5,20,25,-100,400), then summing and shifting them down to a 4x4 result.
 *   Although fun to code, this was not very clever and needed a lot of live
 *   registers at any time (thus many stack spills).
 * _ computing half-sample interpolations first and averaging them in qpel
 *   code. While it used very little code, it incurred a lot of reads/writes
 *   of temporary data and wasted many redundant operations.
 *
 * As with Intra decoding, we pass zero to all functions to prevent compilers
 * from inserting pxor everywhere (they do), and also nstride=-stride and
 * q=p+stride*4 to prevent them from doing sub-efficient math on addresses.
 * While it is impossible for functions to return multiple values in registers
 * (stupid ABI), we cannot put redundant loads in functions and duplicate a lot
 * of code. The same goes for filter_6tap, which would force all live registers
 * on stack if not inlined. Although I_HATE_MACROS, they are very useful here
 * to reduce 16 (big) functions down to 7. This is a lot of code, but all qpel
 * 4x4 interpolations are done in registers without sub-functions!
 */
int inter4x4_qpel00_8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, uint8_t *q) {
	__m128i s22 = _mm_cvtsi32_si128(*(int *)(p              ));
	__m128i s32 = _mm_cvtsi32_si128(*(int *)(p +  stride    ));
	__m128i s42 = _mm_cvtsi32_si128(*(int *)(p +  stride * 2));
	__m128i s52 = _mm_cvtsi32_si128(*(int *)(q + nstride    ));
	__m128i m22 = _mm_unpacklo_epi8(_mm_unpacklo_epi32(s22, s32), zero);
	__m128i m42 = _mm_unpacklo_epi8(_mm_unpacklo_epi32(s42, s52), zero);
	return ponderation_4x4(m22, m42);
}

#define INTER4x4_QPEL_01_02_03(QPEL, P0, P1)\
	int inter4x4_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, uint8_t *q) {\
		__m128i l20 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p               - 2)), zero);\
		__m128i l21 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p               - 1)), zero);\
		__m128i l30 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     - 2)), zero);\
		__m128i l31 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     - 1)), zero);\
		__m128i l40 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 - 2)), zero);\
		__m128i l41 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 - 1)), zero);\
		__m128i l50 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q + nstride     - 2)), zero);\
		__m128i l51 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q + nstride     - 1)), zero);\
		__m128i m20 = (__m128i)_mm_shuffle_ps((__m128)l20, (__m128)l30, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m21 = (__m128i)_mm_shuffle_ps((__m128)l21, (__m128)l31, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m22 = (__m128i)_mm_shuffle_ps((__m128)l20, (__m128)l30, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m23 = (__m128i)_mm_shuffle_ps((__m128)l21, (__m128)l31, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m24 = (__m128i)_mm_shuffle_ps((__m128)l20, (__m128)l30, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m25 = (__m128i)_mm_shuffle_ps((__m128)l21, (__m128)l31, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m40 = (__m128i)_mm_shuffle_ps((__m128)l40, (__m128)l50, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m41 = (__m128i)_mm_shuffle_ps((__m128)l41, (__m128)l51, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m42 = (__m128i)_mm_shuffle_ps((__m128)l40, (__m128)l50, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m43 = (__m128i)_mm_shuffle_ps((__m128)l41, (__m128)l51, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m44 = (__m128i)_mm_shuffle_ps((__m128)l40, (__m128)l50, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m45 = (__m128i)_mm_shuffle_ps((__m128)l41, (__m128)l51, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i h0 = filter_6tap(m20, m21, m22, m23, m24, m25, zero);\
		__m128i h1 = filter_6tap(m40, m41, m42, m43, m44, m45, zero);\
		return ponderation_4x4(P0, P1);\
	}\

INTER4x4_QPEL_01_02_03(qpel01, _mm_avg_epu16(h0, m22), _mm_avg_epu16(h1, m42))
INTER4x4_QPEL_01_02_03(qpel02, h0, h1)
INTER4x4_QPEL_01_02_03(qpel03, _mm_avg_epu16(h0, m23), _mm_avg_epu16(h1, m43))

#define INTER4x4_QPEL_10_20_30(QPEL, P0, P1)\
	int inter4x4_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, uint8_t *q) {\
		__m128i s02 = _mm_cvtsi32_si128(*(int *)(p + nstride * 2));\
		__m128i s12 = _mm_cvtsi32_si128(*(int *)(p + nstride    ));\
		__m128i s22 = _mm_cvtsi32_si128(*(int *)(p              ));\
		__m128i s32 = _mm_cvtsi32_si128(*(int *)(p +  stride    ));\
		__m128i s42 = _mm_cvtsi32_si128(*(int *)(p +  stride * 2));\
		__m128i s52 = _mm_cvtsi32_si128(*(int *)(q + nstride    ));\
		__m128i s62 = _mm_cvtsi32_si128(*(int *)(q              ));\
		__m128i s72 = _mm_cvtsi32_si128(*(int *)(q +  stride    ));\
		__m128i s82 = _mm_cvtsi32_si128(*(int *)(q +  stride * 2));\
		__m128i m02 = _mm_unpacklo_epi8(_mm_unpacklo_epi32(s02, s12), zero);\
		__m128i m22 = _mm_unpacklo_epi8(_mm_unpacklo_epi32(s22, s32), zero);\
		__m128i m42 = _mm_unpacklo_epi8(_mm_unpacklo_epi32(s42, s52), zero);\
		__m128i m62 = _mm_unpacklo_epi8(_mm_unpacklo_epi32(s62, s72), zero);\
		__m128i m12 = _mm_alignr_epi8(m22, m02, 8);\
		__m128i m32 = _mm_alignr_epi8(m42, m22, 8);\
		__m128i m52 = _mm_alignr_epi8(m62, m42, 8);\
		__m128i m72 = _mm_alignr_epi8(_mm_unpacklo_epi8(s82, zero), m62, 8);\
		__m128i v0 = filter_6tap(m02, m12, m22, m32, m42, m52, zero);\
		__m128i v1 = filter_6tap(m22, m32, m42, m52, m62, m72, zero);\
		return ponderation_4x4(P0, P1);\
	}\

INTER4x4_QPEL_10_20_30(qpel10, _mm_avg_epu16(v0, m22), _mm_avg_epu16(v1, m42))
INTER4x4_QPEL_10_20_30(qpel20, v0, v1)
INTER4x4_QPEL_10_20_30(qpel30, _mm_avg_epu16(v0, m32), _mm_avg_epu16(v1, m52))

#define INTER4x4_QPEL_11_13(QPEL, C, OFFSET)\
	int inter4x4_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, uint8_t *q) {\
		__m128i l20 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p               - 2)), zero);\
		__m128i l21 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p               - 1)), zero);\
		__m128i l30 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     - 2)), zero);\
		__m128i l31 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     - 1)), zero);\
		__m128i l40 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 - 2)), zero);\
		__m128i l41 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 - 1)), zero);\
		__m128i l50 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q + nstride     - 2)), zero);\
		__m128i l51 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q + nstride     - 1)), zero);\
		__m128i m20 = (__m128i)_mm_shuffle_ps((__m128)l20, (__m128)l30, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m21 = (__m128i)_mm_shuffle_ps((__m128)l21, (__m128)l31, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m22 = (__m128i)_mm_shuffle_ps((__m128)l20, (__m128)l30, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m23 = (__m128i)_mm_shuffle_ps((__m128)l21, (__m128)l31, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m24 = (__m128i)_mm_shuffle_ps((__m128)l20, (__m128)l30, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m25 = (__m128i)_mm_shuffle_ps((__m128)l21, (__m128)l31, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m40 = (__m128i)_mm_shuffle_ps((__m128)l40, (__m128)l50, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m41 = (__m128i)_mm_shuffle_ps((__m128)l41, (__m128)l51, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m42 = (__m128i)_mm_shuffle_ps((__m128)l40, (__m128)l50, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m43 = (__m128i)_mm_shuffle_ps((__m128)l41, (__m128)l51, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m44 = (__m128i)_mm_shuffle_ps((__m128)l40, (__m128)l50, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m45 = (__m128i)_mm_shuffle_ps((__m128)l41, (__m128)l51, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i h0 = filter_6tap(m20, m21, m22, m23, m24, m25, zero);\
		__m128i h1 = filter_6tap(m40, m41, m42, m43, m44, m45, zero);\
		__m128i s0##C = _mm_cvtsi32_si128(*(int *)(p + nstride * 2 + OFFSET));\
		__m128i s1##C = _mm_cvtsi32_si128(*(int *)(p + nstride     + OFFSET));\
		__m128i s6##C = _mm_cvtsi32_si128(*(int *)(q               + OFFSET));\
		__m128i s7##C = _mm_cvtsi32_si128(*(int *)(q +  stride     + OFFSET));\
		__m128i s8##C = _mm_cvtsi32_si128(*(int *)(q +  stride * 2 + OFFSET));\
		__m128i m0##C = _mm_unpacklo_epi8(_mm_unpacklo_epi32(s0##C, s1##C), zero);\
		__m128i m6##C = _mm_unpacklo_epi8(_mm_unpacklo_epi32(s6##C, s7##C), zero);\
		__m128i m1##C = _mm_alignr_epi8(m2##C, m0##C, 8);\
		__m128i m3##C = _mm_alignr_epi8(m4##C, m2##C, 8);\
		__m128i m5##C = _mm_alignr_epi8(m6##C, m4##C, 8);\
		__m128i m7##C = _mm_alignr_epi8(_mm_unpacklo_epi8(s8##C, zero), m6##C, 8);\
		__m128i v0 = filter_6tap(m0##C, m1##C, m2##C, m3##C, m4##C, m5##C, zero);\
		__m128i v1 = filter_6tap(m2##C, m3##C, m4##C, m5##C, m6##C, m7##C, zero);\
		return ponderation_4x4(_mm_avg_epu16(v0, h0), _mm_avg_epu16(v1, h1));\
	}\

INTER4x4_QPEL_11_13(qpel11, 2, 0)
INTER4x4_QPEL_11_13(qpel13, 3, 1)

#define INTER4x4_QPEL_31_33(QPEL, C, OFFSET)\
	int inter4x4_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, uint8_t *q) {\
		__m128i l30 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     - 2)), zero);\
		__m128i l31 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     - 1)), zero);\
		__m128i l40 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 - 2)), zero);\
		__m128i l41 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 - 1)), zero);\
		__m128i l50 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q + nstride     - 2)), zero);\
		__m128i l51 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q + nstride     - 1)), zero);\
		__m128i l60 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q               - 2)), zero);\
		__m128i l61 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q               - 1)), zero);\
		__m128i m30 = (__m128i)_mm_shuffle_ps((__m128)l30, (__m128)l40, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m31 = (__m128i)_mm_shuffle_ps((__m128)l31, (__m128)l41, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m32 = (__m128i)_mm_shuffle_ps((__m128)l30, (__m128)l40, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m33 = (__m128i)_mm_shuffle_ps((__m128)l31, (__m128)l41, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m34 = (__m128i)_mm_shuffle_ps((__m128)l30, (__m128)l40, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m35 = (__m128i)_mm_shuffle_ps((__m128)l31, (__m128)l41, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m50 = (__m128i)_mm_shuffle_ps((__m128)l50, (__m128)l60, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m51 = (__m128i)_mm_shuffle_ps((__m128)l51, (__m128)l61, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m52 = (__m128i)_mm_shuffle_ps((__m128)l50, (__m128)l60, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m53 = (__m128i)_mm_shuffle_ps((__m128)l51, (__m128)l61, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m54 = (__m128i)_mm_shuffle_ps((__m128)l50, (__m128)l60, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m55 = (__m128i)_mm_shuffle_ps((__m128)l51, (__m128)l61, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i h0 = filter_6tap(m30, m31, m32, m33, m34, m35, zero);\
		__m128i h1 = filter_6tap(m50, m51, m52, m53, m54, m55, zero);\
		__m128i s0##C = _mm_cvtsi32_si128(*(int *)(p + nstride * 2 + OFFSET));\
		__m128i s1##C = _mm_cvtsi32_si128(*(int *)(p + nstride     + OFFSET));\
		__m128i s2##C = _mm_cvtsi32_si128(*(int *)(p               + OFFSET));\
		__m128i s7##C = _mm_cvtsi32_si128(*(int *)(q +  stride     + OFFSET));\
		__m128i s8##C = _mm_cvtsi32_si128(*(int *)(q +  stride * 2 + OFFSET));\
		__m128i m1##C = _mm_unpacklo_epi8(_mm_unpacklo_epi32(s1##C, s2##C), zero);\
		__m128i m7##C = _mm_unpacklo_epi8(_mm_unpacklo_epi32(s7##C, s8##C), zero);\
		__m128i m0##C = _mm_unpacklo_epi64(_mm_unpacklo_epi8(s0##C, zero), m1##C);\
		__m128i m2##C = _mm_alignr_epi8(m3##C, m1##C, 8);\
		__m128i m4##C = _mm_alignr_epi8(m5##C, m3##C, 8);\
		__m128i m6##C = _mm_alignr_epi8(m7##C, m5##C, 8);\
		__m128i v0 = filter_6tap(m0##C, m1##C, m2##C, m3##C, m4##C, m5##C, zero);\
		__m128i v1 = filter_6tap(m2##C, m3##C, m4##C, m5##C, m6##C, m7##C, zero);\
		return ponderation_4x4(_mm_avg_epu16(v0, h0), _mm_avg_epu16(v1, h1));\
	}\

INTER4x4_QPEL_31_33(qpel31, 2, 0)
INTER4x4_QPEL_31_33(qpel33, 3, 1)

#define INTER4x4_QPEL_21_22_23(QPEL, P0, P1)\
	int inter4x4_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, uint8_t *q) {\
		__m128i l00 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride * 2 - 2)), zero);\
		__m128i l10 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride     - 2)), zero);\
		__m128i l20 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p               - 2)), zero);\
		__m128i l30 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     - 2)), zero);\
		__m128i l40 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 - 2)), zero);\
		__m128i l50 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q + nstride     - 2)), zero);\
		__m128i l60 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q               - 2)), zero);\
		__m128i l70 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q +  stride     - 2)), zero);\
		__m128i l80 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q +  stride * 2 - 2)), zero);\
		int q08 = p[nstride * 2 + 6];\
		int q18 = p[nstride     + 6];\
		int q28 = p[              6];\
		int q38 = p[ stride     + 6];\
		int q48 = p[ stride * 2 + 6];\
		int q58 = q[nstride     + 6];\
		int q68 = q[              6];\
		int q78 = q[ stride     + 6];\
		int q88 = q[ stride * 2 + 6];\
		__m128i x00 = filter_36tapU_8bit(l00, l10, l20, l30, l40, l50);\
		__m128i x01 = _mm_alignr_epi8(filter_36tapU_scalar(q08, q18, q28, q38, q48, q58), x00, 2);\
		__m128i x10 = filter_36tapU_8bit(l10, l20, l30, l40, l50, l60);\
		__m128i x11 = _mm_alignr_epi8(filter_36tapU_scalar(q18, q28, q38, q48, q58, q68), x10, 2);\
		__m128i x20 = filter_36tapU_8bit(l20, l30, l40, l50, l60, l70);\
		__m128i x21 = _mm_alignr_epi8(filter_36tapU_scalar(q28, q38, q48, q58, q68, q78), x20, 2);\
		__m128i x30 = filter_36tapU_8bit(l30, l40, l50, l60, l70, l80);\
		__m128i x31 = _mm_alignr_epi8(filter_36tapU_scalar(q38, q48, q58, q68, q78, q88), x30, 2);\
		__m128i y00 = (__m128i)_mm_shuffle_ps((__m128)x00, (__m128)x10, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i y01 = (__m128i)_mm_shuffle_ps((__m128)x01, (__m128)x11, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i y02 = (__m128i)_mm_shuffle_ps((__m128)x00, (__m128)x10, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i y03 = (__m128i)_mm_shuffle_ps((__m128)x01, (__m128)x11, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i y04 = (__m128i)_mm_shuffle_ps((__m128)x00, (__m128)x10, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i y05 = (__m128i)_mm_shuffle_ps((__m128)x01, (__m128)x11, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i y20 = (__m128i)_mm_shuffle_ps((__m128)x20, (__m128)x30, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i y21 = (__m128i)_mm_shuffle_ps((__m128)x21, (__m128)x31, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i y22 = (__m128i)_mm_shuffle_ps((__m128)x20, (__m128)x30, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i y23 = (__m128i)_mm_shuffle_ps((__m128)x21, (__m128)x31, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i y24 = (__m128i)_mm_shuffle_ps((__m128)x20, (__m128)x30, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i y25 = (__m128i)_mm_shuffle_ps((__m128)x21, (__m128)x31, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i vh0 = filter_36tapD_8bit(y00, y01, y02, y03, y04, y05, zero);\
		__m128i vh1 = filter_36tapD_8bit(y20, y21, y22, y23, y24, y25, zero);\
		return ponderation_4x4(P0, P1);\
	}\

INTER4x4_QPEL_21_22_23(qpel21, avg_6tapD_8bit(y02, vh0, zero), avg_6tapD_8bit(y22, vh1, zero))
INTER4x4_QPEL_21_22_23(qpel22, vh0, vh1)
INTER4x4_QPEL_21_22_23(qpel23, avg_6tapD_8bit(y03, vh0, zero), avg_6tapD_8bit(y23, vh1, zero))

#define INTER4x4_QPEL_12_32(QPEL, P0, P1)\
	int inter4x4_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, uint8_t *q) {\
		__m128i l00 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride * 2 - 2)), zero);\
		__m128i l01 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride * 2 - 1)), zero);\
		__m128i l10 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride     - 2)), zero);\
		__m128i l11 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride     - 1)), zero);\
		__m128i m00 = (__m128i)_mm_shuffle_ps((__m128)l00, (__m128)l10, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m01 = (__m128i)_mm_shuffle_ps((__m128)l01, (__m128)l11, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m02 = (__m128i)_mm_shuffle_ps((__m128)l00, (__m128)l10, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m03 = (__m128i)_mm_shuffle_ps((__m128)l01, (__m128)l11, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m04 = (__m128i)_mm_shuffle_ps((__m128)l00, (__m128)l10, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m05 = (__m128i)_mm_shuffle_ps((__m128)l01, (__m128)l11, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i x00 = filter_36tapU_8bit(m00, m01, m02, m03, m04, m05);\
		__m128i l20 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p               - 2)), zero);\
		__m128i l21 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p               - 1)), zero);\
		__m128i l30 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     - 2)), zero);\
		__m128i l31 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     - 1)), zero);\
		__m128i m20 = (__m128i)_mm_shuffle_ps((__m128)l20, (__m128)l30, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m21 = (__m128i)_mm_shuffle_ps((__m128)l21, (__m128)l31, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m22 = (__m128i)_mm_shuffle_ps((__m128)l20, (__m128)l30, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m23 = (__m128i)_mm_shuffle_ps((__m128)l21, (__m128)l31, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m24 = (__m128i)_mm_shuffle_ps((__m128)l20, (__m128)l30, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m25 = (__m128i)_mm_shuffle_ps((__m128)l21, (__m128)l31, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i x20 = filter_36tapU_8bit(m20, m21, m22, m23, m24, m25);\
		__m128i l40 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 - 2)), zero);\
		__m128i l41 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 - 1)), zero);\
		__m128i l50 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q + nstride     - 2)), zero);\
		__m128i l51 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q + nstride     - 1)), zero);\
		__m128i m40 = (__m128i)_mm_shuffle_ps((__m128)l40, (__m128)l50, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m41 = (__m128i)_mm_shuffle_ps((__m128)l41, (__m128)l51, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m42 = (__m128i)_mm_shuffle_ps((__m128)l40, (__m128)l50, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m43 = (__m128i)_mm_shuffle_ps((__m128)l41, (__m128)l51, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m44 = (__m128i)_mm_shuffle_ps((__m128)l40, (__m128)l50, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m45 = (__m128i)_mm_shuffle_ps((__m128)l41, (__m128)l51, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i x40 = filter_36tapU_8bit(m40, m41, m42, m43, m44, m45);\
		__m128i l60 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q               - 2)), zero);\
		__m128i l61 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q               - 1)), zero);\
		__m128i l70 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q +  stride     - 2)), zero);\
		__m128i l71 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q +  stride     - 1)), zero);\
		__m128i m60 = (__m128i)_mm_shuffle_ps((__m128)l60, (__m128)l70, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m61 = (__m128i)_mm_shuffle_ps((__m128)l61, (__m128)l71, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m62 = (__m128i)_mm_shuffle_ps((__m128)l60, (__m128)l70, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m63 = (__m128i)_mm_shuffle_ps((__m128)l61, (__m128)l71, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m64 = (__m128i)_mm_shuffle_ps((__m128)l60, (__m128)l70, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m65 = (__m128i)_mm_shuffle_ps((__m128)l61, (__m128)l71, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i x60 = filter_36tapU_8bit(m60, m61, m62, m63, m64, m65);\
		__m128i l80 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q +  stride * 2 - 2)), zero);\
		__m128i l81 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q +  stride * 2 - 1)), zero);\
		__m128i m70 = (__m128i)_mm_shuffle_ps((__m128)l70, (__m128)l80, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m71 = (__m128i)_mm_shuffle_ps((__m128)l71, (__m128)l81, _MM_SHUFFLE(1, 0, 1, 0));\
		__m128i m72 = (__m128i)_mm_shuffle_ps((__m128)l70, (__m128)l80, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m73 = (__m128i)_mm_shuffle_ps((__m128)l71, (__m128)l81, _MM_SHUFFLE(2, 1, 2, 1));\
		__m128i m74 = (__m128i)_mm_shuffle_ps((__m128)l70, (__m128)l80, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i m75 = (__m128i)_mm_shuffle_ps((__m128)l71, (__m128)l81, _MM_SHUFFLE(3, 2, 3, 2));\
		__m128i x70 = filter_36tapU_8bit(m70, m71, m72, m73, m74, m75);\
		__m128i x10 = _mm_alignr_epi8(x20, x00, 8);\
		__m128i x30 = _mm_alignr_epi8(x40, x20, 8);\
		__m128i x50 = _mm_alignr_epi8(x60, x40, 8);\
		__m128i hv0 = filter_36tapD_8bit(x00, x10, x20, x30, x40, x50, zero);\
		__m128i hv1 = filter_36tapD_8bit(x20, x30, x40, x50, x60, x70, zero);\
		return ponderation_4x4(avg_6tapD_8bit(P0, hv0, zero), avg_6tapD_8bit(P1, hv1, zero));\
	}\

INTER4x4_QPEL_12_32(qpel12, x20, x40)
INTER4x4_QPEL_12_32(qpel32, x30, x50)



/**
 * Inter 8x8 prediction takes one (or two) 13x13 matrix and yields a 16bit 8x8
 * result. This is simpler than 4x4 since each register is a 8x1 line, so it
 * needs less shuffling. Unrolling would mean a lot of copy/paste and code,
 * thus we use loops and leave it to compilers to decide (they do a bit at O3).
 */
int inter8x8_qpel00_8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *restrict p, __m128i *restrict dst) {
	uint8_t *q = p + stride * 4;
	dst[0] = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p              )), zero);
	dst[1] = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride    )), zero);
	dst[2] = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2)), zero);
	dst[3] = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q + nstride    )), zero);
	dst[4] = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q              )), zero);
	dst[5] = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q +  stride    )), zero);
	dst[6] = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q +  stride * 2)), zero);
	dst[7] = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(q +  stride * 3)), zero);
	return ponderation_8x8();
}

#define INTER8x8_QPEL_01_02_03(QPEL, P)\
	int inter8x8_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, __m128i *dst) {\
		for (int i = 0; i < 8; i++, p += stride) {\
			__m128i l05 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + 3)), zero);\
			__m128i l00 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p - 2)), zero);\
			__m128i x0 = _mm_srli_si128(l05, 6);\
			__m128i l01 = _mm_alignr_epi8(x0, l00, 2);\
			__m128i l02 = _mm_alignr_epi8(x0, l00, 2);\
			__m128i l03 = _mm_alignr_epi8(x0, l00, 2);\
			__m128i l04 = _mm_alignr_epi8(x0, l00, 2);\
			__m128i h = filter_6tap(l00, l01, l02, l03, l04, l05, zero);\
			dst[i] = P;\
		}\
		return ponderation_8x8();\
	}\

INTER8x8_QPEL_01_02_03(qpel01, _mm_avg_epu16(h, l02))
INTER8x8_QPEL_01_02_03(qpel02, h)
INTER8x8_QPEL_01_02_03(qpel03, _mm_avg_epu16(h, l03))

#define INTER8x8_QPEL_10_20_30(QPEL, P)\
	int inter8x8_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, __m128i *dst) {\
		__m128i l00 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride * 2)), zero);\
		__m128i l10 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride    )), zero);\
		__m128i l20 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p              )), zero);\
		__m128i l30 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride    )), zero);\
		__m128i l40 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2)), zero);\
		for (int i = 0; i < 8; i++) {\
			p += stride;\
			__m128i l50 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + stride * 2)), zero);\
			__m128i v = filter_6tap(l00, l10, l20, l30, l40, l50, zero);\
			dst[i] = P;\
			l00 = l10, l10 = l20, l20 = l30, l30 = l40, l40 = l50;\
		}\
		return ponderation_8x8();\
	}\

INTER8x8_QPEL_10_20_30(qpel10, _mm_avg_epu16(v, l20))
INTER8x8_QPEL_10_20_30(qpel20, v)
INTER8x8_QPEL_10_20_30(qpel30, _mm_avg_epu16(v, l30))

#define INTER8x8_QPEL_11_31(QPEL, R)\
	int inter8x8_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, __m128i *dst) {\
		__m128i shuf2 = (__m128i)(v16qi){2, -1, 3, -1, 4, -1, 5, -1, 6, -1, 7, -1, 11, -1, 12, -1};\
		__m128i shufA = (__m128i)(v16qi){13, -1, 14, -1, 15, -1, -1, -1, -1, -1, -1, -1, 0, -1, 1, -1};\
		__m128i l02 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride * 2)), zero);\
		__m128i l12 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride    )), zero);\
		__m128i x0 = _mm_set_epi64(*(__m64 *)(p              + 3), *(__m64 *)(p              - 2));\
		__m128i x1 = _mm_set_epi64(*(__m64 *)(p + stride     + 3), *(__m64 *)(p + stride     - 2));\
		__m128i x2 = _mm_set_epi64(*(__m64 *)(p + stride * 2 + 3), *(__m64 *)(p + stride * 2 - 2));\
		__m128i l22 = _mm_shuffle_epi8(x0, shuf2);\
		__m128i l2A = _mm_shuffle_epi8(x0, shufA);\
		__m128i l32 = _mm_shuffle_epi8(x1, shuf2);\
		__m128i l3A = _mm_shuffle_epi8(x1, shufA);\
		__m128i l42 = _mm_shuffle_epi8(x2, shuf2);\
		__m128i l4A = _mm_shuffle_epi8(x2, shufA);\
		for (int i = 0; i < 8; i++) {\
			p += stride;\
			__m128i l##R##0 = _mm_alignr_epi8(l##R##2, l##R##A, 12);\
			__m128i l##R##1 = _mm_alignr_epi8(l##R##2, l##R##A, 14);\
			__m128i l##R##3 = _mm_alignr_epi8(l##R##A, l##R##2, 2);\
			__m128i l##R##4 = _mm_alignr_epi8(l##R##A, l##R##2, 4);\
			__m128i l##R##5 = _mm_alignr_epi8(l##R##A, l##R##2, 6);\
			__m128i h = filter_6tap(l##R##0, l##R##1, l##R##2, l##R##3, l##R##4, l##R##5, zero);\
			__m128i x3 = _mm_set_epi64(*(__m64 *)(p + stride * 2 + 3), *(__m64 *)(p + stride * 2 - 2));\
			__m128i l52 = _mm_shuffle_epi8(x3, shuf2);\
			__m128i l5A = _mm_shuffle_epi8(x3, shufA);\
			__m128i v = filter_6tap(l02, l12, l22, l32, l42, l52, zero);\
			dst[i] = _mm_avg_epu16(v, h);\
			l02 = l12;\
			l12 = l22;\
			l22 = l32, l2A = l3A;\
			l32 = l42, l3A = l4A;\
			l42 = l52, l4A = l5A;\
		}\
		return ponderation_8x8();\
	}\

INTER8x8_QPEL_11_31(qpel11, 2)
INTER8x8_QPEL_11_31(qpel31, 3)

#define INTER8x8_QPEL_13_33(QPEL, R)\
	int inter8x8_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, __m128i *dst) {\
		__m128i shuf3 = (__m128i)(v16qi){3, -1, 4, -1, 5, -1, 6, -1, 7, -1, 11, -1, 12, -1, 13, -1};\
		__m128i shufB = (__m128i)(v16qi){14, -1, 15, -1, -1, -1, -1, -1, -1, -1, 0, -1, 1, -1, 2, -1};\
		__m128i l03 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride * 2 + 1)), zero);\
		__m128i l13 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride     + 1)), zero);\
		__m128i x0 = _mm_set_epi64(*(__m64 *)(p              + 3), *(__m64 *)(p              - 2));\
		__m128i x1 = _mm_set_epi64(*(__m64 *)(p + stride     + 3), *(__m64 *)(p + stride     - 2));\
		__m128i x2 = _mm_set_epi64(*(__m64 *)(p + stride * 2 + 3), *(__m64 *)(p + stride * 2 - 2));\
		__m128i l23 = _mm_shuffle_epi8(x0, shuf3);\
		__m128i l2B = _mm_shuffle_epi8(x0, shufB);\
		__m128i l33 = _mm_shuffle_epi8(x1, shuf3);\
		__m128i l3B = _mm_shuffle_epi8(x1, shufB);\
		__m128i l43 = _mm_shuffle_epi8(x2, shuf3);\
		__m128i l4B = _mm_shuffle_epi8(x2, shufB);\
		for (int i = 0; i < 8; i++) {\
			p += stride;\
			__m128i l##R##0 = _mm_alignr_epi8(l##R##3, l##R##B, 10);\
			__m128i l##R##1 = _mm_alignr_epi8(l##R##3, l##R##B, 12);\
			__m128i l##R##2 = _mm_alignr_epi8(l##R##3, l##R##B, 14);\
			__m128i l##R##4 = _mm_alignr_epi8(l##R##B, l##R##3, 2);\
			__m128i l##R##5 = _mm_alignr_epi8(l##R##B, l##R##3, 4);\
			__m128i h = filter_6tap(l##R##0, l##R##1, l##R##2, l##R##3, l##R##4, l##R##5, zero);\
			__m128i x3 = _mm_set_epi64(*(__m64 *)(p + stride * 2 + 3), *(__m64 *)(p + stride * 2 - 2));\
			__m128i l53 = _mm_shuffle_epi8(x3, shuf3);\
			__m128i l5B = _mm_shuffle_epi8(x3, shufB);\
			__m128i v = filter_6tap(l03, l13, l23, l33, l43, l53, zero);\
			dst[i] = _mm_avg_epu16(v, h);\
			l03 = l13;\
			l13 = l23;\
			l23 = l33, l2B = l3B;\
			l33 = l43, l3B = l4B;\
			l43 = l53, l4B = l5B;\
		}\
		return ponderation_8x8();\
	}\

INTER8x8_QPEL_13_33(qpel13, 2)
INTER8x8_QPEL_13_33(qpel33, 3)

#define INTER8x8_QPEL_21_22_23(QPEL, P)\
	int inter8x8_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, __m128i *dst) {\
		__m128i l00 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride * 2 - 2)), zero);\
		__m128i l05 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride * 2 + 3)), zero);\
		__m128i l10 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride     - 2)), zero);\
		__m128i l15 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride     + 3)), zero);\
		__m128i l20 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p               - 2)), zero);\
		__m128i l25 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p               + 3)), zero);\
		__m128i l30 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     - 2)), zero);\
		__m128i l35 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     + 3)), zero);\
		__m128i l40 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 - 2)), zero);\
		__m128i l45 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 + 3)), zero);\
		for (int i = 0; i < 8; i++) {\
			p += stride;\
			__m128i l50 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + stride * 2 - 2)), zero);\
			__m128i l55 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + stride * 2 + 3)), zero);\
			__m128i x05 = filter_36tapU_8bit(l05, l15, l25, l35, l45, l55);\
			__m128i x00 = filter_36tapU_8bit(l00, l10, l20, l30, l40, l50);\
			__m128i x08 = _mm_srli_si128(x05, 6);\
			__m128i x01 = _mm_alignr_epi8(x08, x00, 2);\
			__m128i x02 = _mm_alignr_epi8(x08, x00, 4);\
			__m128i x03 = _mm_alignr_epi8(x08, x00, 6);\
			__m128i x04 = _mm_alignr_epi8(x08, x00, 8);\
			__m128i vh = filter_36tapD_8bit(x00, x01, x02, x03, x04, x05, zero);\
			dst[i] = P;\
			l00 = l10, l05 = l15;\
			l10 = l20, l15 = l25;\
			l20 = l30, l25 = l35;\
			l30 = l40, l35 = l45;\
			l40 = l50, l45 = l55;\
		}\
		return ponderation_8x8();\
	}\

INTER8x8_QPEL_21_22_23(qpel21, avg_6tapD_8bit(x02, vh, zero))
INTER8x8_QPEL_21_22_23(qpel22, vh)
INTER8x8_QPEL_21_22_23(qpel23, avg_6tapD_8bit(x03, vh, zero))

#define INTER8x8_QPEL_12_32(QPEL, P)\
	int inter8x8_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, __m128i *dst) {\
		__m128i l05 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride * 2 + 3)), zero);\
		__m128i l00 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride * 2 - 2)), zero);\
		__m128i l08 = _mm_srli_si128(l05, 6);\
		__m128i l01 = _mm_alignr_epi8(l08, l00, 2);\
		__m128i l02 = _mm_alignr_epi8(l08, l00, 4);\
		__m128i l03 = _mm_alignr_epi8(l08, l00, 6);\
		__m128i l04 = _mm_alignr_epi8(l08, l00, 8);\
		__m128i x00 = filter_36tapU_8bit(l00, l01, l02, l03, l04, l05);\
		__m128i l15 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride     + 3)), zero);\
		__m128i l10 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride     - 2)), zero);\
		__m128i l18 = _mm_srli_si128(l15, 6);\
		__m128i l11 = _mm_alignr_epi8(l18, l10, 2);\
		__m128i l12 = _mm_alignr_epi8(l18, l10, 4);\
		__m128i l13 = _mm_alignr_epi8(l18, l10, 6);\
		__m128i l14 = _mm_alignr_epi8(l18, l10, 8);\
		__m128i x10 = filter_36tapU_8bit(l10, l11, l12, l13, l14, l15);\
		__m128i l25 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p               + 3)), zero);\
		__m128i l20 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p               - 2)), zero);\
		__m128i l28 = _mm_srli_si128(l25, 6);\
		__m128i l21 = _mm_alignr_epi8(l28, l20, 2);\
		__m128i l22 = _mm_alignr_epi8(l28, l20, 4);\
		__m128i l23 = _mm_alignr_epi8(l28, l20, 6);\
		__m128i l24 = _mm_alignr_epi8(l28, l20, 8);\
		__m128i x20 = filter_36tapU_8bit(l20, l21, l22, l23, l24, l25);\
		__m128i l35 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     + 3)), zero);\
		__m128i l30 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride     - 2)), zero);\
		__m128i l38 = _mm_srli_si128(l35, 6);\
		__m128i l31 = _mm_alignr_epi8(l38, l30, 2);\
		__m128i l32 = _mm_alignr_epi8(l38, l30, 4);\
		__m128i l33 = _mm_alignr_epi8(l38, l30, 6);\
		__m128i l34 = _mm_alignr_epi8(l38, l30, 8);\
		__m128i x30 = filter_36tapU_8bit(l30, l31, l32, l33, l34, l35);\
		__m128i l45 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 + 3)), zero);\
		__m128i l40 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p +  stride * 2 - 2)), zero);\
		__m128i l48 = _mm_srli_si128(l45, 6);\
		__m128i l41 = _mm_alignr_epi8(l48, l40, 2);\
		__m128i l42 = _mm_alignr_epi8(l48, l40, 4);\
		__m128i l43 = _mm_alignr_epi8(l48, l40, 6);\
		__m128i l44 = _mm_alignr_epi8(l48, l40, 8);\
		__m128i x40 = filter_36tapU_8bit(l40, l41, l42, l43, l44, l45);\
		for (int i = 0; i < 8; i++) {\
			p += stride;\
			__m128i l55 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + stride * 2 + 3)), zero);\
			__m128i l50 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + stride * 2 - 2)), zero);\
			__m128i l58 = _mm_srli_si128(l55, 6);\
			__m128i l51 = _mm_alignr_epi8(l58, l50, 2);\
			__m128i l52 = _mm_alignr_epi8(l58, l50, 4);\
			__m128i l53 = _mm_alignr_epi8(l58, l50, 6);\
			__m128i l54 = _mm_alignr_epi8(l58, l50, 8);\
			__m128i x50 = filter_36tapU_8bit(l50, l51, l52, l53, l54, l55);\
			__m128i hv = filter_36tapD_8bit(x00, x10, x20, x30, x40, x50, zero);\
			dst[i] = avg_6tapD_8bit(P, hv, zero);\
			x00 = x10, x10 = x20, x20 = x30, x30 = x40, x40 = x50;\
		}\
		return ponderation_8x8();\
	}\

INTER8x8_QPEL_12_32(qpel12, x20)
INTER8x8_QPEL_12_32(qpel32, x30)



/**
 * Inter 16x16 takes one (or two) 21x21 matrix and yields a 16x16 matrix.
 * Contrary to 4x4 and 8x8 it won't go to residual afterwards, so we store it
 * directly in place. We denote as dRC a 16x1 matrix of 8bit values loaded from
 * memory.
 * Here more than previous code, we are confronted with very high register
 * pressure and will rather reload values from memory than keeping them live.
 */
int inter16x16_qpel00_8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *restrict p, size_t dstStride, __m128i *restrict dst) {
	ssize_t nStride = -dstStride;
	*(__m128i *)(dst                ) = _mm_lddqu_si128((__m128i *)(p              ));
	*(__m128i *)(dst + dstStride    ) = _mm_lddqu_si128((__m128i *)(p +  stride    ));
	*(__m128i *)(dst + dstStride * 2) = _mm_lddqu_si128((__m128i *)(p +  stride * 2));
	p += stride * 4;
	dst += dstStride * 4;
	*(__m128i *)(dst +   nStride    ) = _mm_lddqu_si128((__m128i *)(p + nstride    ));
	*(__m128i *)(dst                ) = _mm_lddqu_si128((__m128i *)(p              ));
	*(__m128i *)(dst + dstStride    ) = _mm_lddqu_si128((__m128i *)(p +  stride    ));
	*(__m128i *)(dst + dstStride * 2) = _mm_lddqu_si128((__m128i *)(p +  stride * 2));
	p += stride * 4;
	dst += dstStride * 4;
	*(__m128i *)(dst +   nStride    ) = _mm_lddqu_si128((__m128i *)(p + nstride    ));
	*(__m128i *)(dst                ) = _mm_lddqu_si128((__m128i *)(p              ));
	*(__m128i *)(dst + dstStride    ) = _mm_lddqu_si128((__m128i *)(p +  stride    ));
	*(__m128i *)(dst + dstStride * 2) = _mm_lddqu_si128((__m128i *)(p +  stride * 2));
	p += stride * 4;
	dst += dstStride * 4;
	*(__m128i *)(dst +   nStride    ) = _mm_lddqu_si128((__m128i *)(p + nstride    ));
	*(__m128i *)(dst                ) = _mm_lddqu_si128((__m128i *)(p              ));
	*(__m128i *)(dst + dstStride    ) = _mm_lddqu_si128((__m128i *)(p +  stride    ));
	*(__m128i *)(dst + dstStride * 2) = _mm_lddqu_si128((__m128i *)(p +  stride * 2));
	*(__m128i *)(dst + dstStride * 3) = _mm_lddqu_si128((__m128i *)(p +  stride * 3));
	return ponderation_16x16();
}

#define INTER16x16_QPEL_01_02_03(QPEL, P)\
	int inter16x16_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, size_t dstStride, __m128i *dst) {\
		for (int i = 0; i < 16; i++, p += stride, dst += dstStride) {\
			__m128i d00 = _mm_lddqu_si128((__m128i *)(p - 2));\
			__m128i l0D = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + 11)), zero);\
			__m128i l00 = _mm_unpacklo_epi8(d00, zero);\
			__m128i l08 = _mm_unpackhi_epi8(d00, zero);\
			__m128i l0G = _mm_srli_si128(l0D, 6);\
			__m128i l01 = _mm_alignr_epi8(l08, l00, 2);\
			__m128i l02 = _mm_alignr_epi8(l08, l00, 4);\
			__m128i l03 = _mm_alignr_epi8(l08, l00, 6);\
			__m128i l04 = _mm_alignr_epi8(l08, l00, 8);\
			__m128i l05 = _mm_alignr_epi8(l08, l00, 10);\
			__m128i l09 = _mm_alignr_epi8(l0G, l08, 2);\
			__m128i l0A = _mm_alignr_epi8(l0G, l08, 4);\
			__m128i l0B = _mm_alignr_epi8(l0G, l08, 6);\
			__m128i l0C = _mm_alignr_epi8(l0G, l08, 8);\
			__m128i h0 = noclip_6tap(l00, l01, l02, l03, l04, l05, zero);\
			__m128i h1 = noclip_6tap(l08, l09, l0A, l0B, l0C, l0D, zero);\
			__m128i h = _mm_packus_epi16(h0, h1);\
			*(__m128i *)dst = P;\
		}\
		return ponderation_16x16();\
	}\

INTER16x16_QPEL_01_02_03(qpel01, _mm_avg_epu8(h, _mm_lddqu_si128((__m128i *)p)))
INTER16x16_QPEL_01_02_03(qpel02, h)
INTER16x16_QPEL_01_02_03(qpel03, _mm_avg_epu8(h, _mm_lddqu_si128((__m128i *)(p + 1))))

#define INTER16x16_QPEL_10_20_30(QPEL, P)\
	int inter16x16_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, size_t dstStride, __m128i *dst) {\
		__m128i d00 = _mm_lddqu_si128((__m128i *)(p + nstride * 2));\
		__m128i d10 = _mm_lddqu_si128((__m128i *)(p + nstride    ));\
		__m128i d20 = _mm_lddqu_si128((__m128i *)(p + nstride * 2));\
		__m128i d30 = _mm_lddqu_si128((__m128i *)(p + nstride * 2));\
		__m128i d40 = _mm_lddqu_si128((__m128i *)(p + nstride * 2));\
		__m128i l00 = _mm_unpacklo_epi8(d00, zero);\
		__m128i l08 = _mm_unpackhi_epi8(d00, zero);\
		__m128i l10 = _mm_unpacklo_epi8(d10, zero);\
		__m128i l18 = _mm_unpackhi_epi8(d10, zero);\
		__m128i l20 = _mm_unpacklo_epi8(d20, zero);\
		__m128i l28 = _mm_unpackhi_epi8(d20, zero);\
		__m128i l30 = _mm_unpacklo_epi8(d30, zero);\
		__m128i l38 = _mm_unpackhi_epi8(d30, zero);\
		__m128i l40 = _mm_unpacklo_epi8(d40, zero);\
		__m128i l48 = _mm_unpackhi_epi8(d40, zero);\
		for (int i = 0; i < 16; i++, dst += dstStride) {\
			p += stride;\
			__m128i d50 = _mm_lddqu_si128((__m128i *)(p + stride * 2));\
			__m128i l50 = _mm_unpacklo_epi8(d50, zero);\
			__m128i l58 = _mm_unpackhi_epi8(d50, zero);\
			__m128i v0 = noclip_6tap(l00, l10, l20, l30, l40, l50, zero);\
			__m128i v1 = noclip_6tap(l08, l18, l28, l38, l48, l58, zero);\
			__m128i v = _mm_packus_epi16(v0, v1);\
			*(__m128i *)dst = P;\
			l00 = l10, l08 = l18;\
			l10 = l20, l18 = l28;\
			l20 = l30, l28 = l38;\
			l30 = l40, l38 = l48;\
			l40 = l50, l48 = l58;\
		}\
		return ponderation_16x16();\
	}\

INTER16x16_QPEL_10_20_30(qpel10, _mm_avg_epu8(v, _mm_lddqu_si128((__m128i *)p)))
INTER16x16_QPEL_10_20_30(qpel20, v)
INTER16x16_QPEL_10_20_30(qpel30, _mm_avg_epu8(v, _mm_lddqu_si128((__m128i *)(p + stride))))

#define INTER16x16_QPEL_11_13(QPEL, R, S, OFFSET)\
	int inter16x16_ ## QPEL ## _8bit(__m128i zero, size_t stride, ssize_t nstride, uint8_t *p, size_t dstStride, __m128i *dst) {\
		__m128i d0##R = _mm_lddqu_si128((__m128i *)(p + nstride * 2 + OFFSET));\
		__m128i d1##R = _mm_lddqu_si128((__m128i *)(p + nstride     + OFFSET));\
		__m128i l0##R = _mm_unpacklo_epi8(d0##R, zero);\
		__m128i l0##S = _mm_unpackhi_epi8(d0##R, zero);\
		__m128i l1##R = _mm_unpacklo_epi8(d1##R, zero);\
		__m128i l1##S = _mm_unpackhi_epi8(d1##R, zero);\
		for (int i = 0; i < 16; i++, dst += dstStride) {\
			p += stride;\
			__m128i d20 = _mm_lddqu_si128((__m128i *)(p + nstride - 2));\
			__m128i l2D = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)(p + nstride + 11)), zero);\
			__m128i l20 = _mm_unpacklo_epi8(d20, zero);\
			__m128i l28 = _mm_unpackhi_epi8(d20, zero);\
			__m128i l2G = _mm_srli_si128(l2D, 6);\
			__m128i l21 = _mm_alignr_epi8(l28, l20, 2);\
			__m128i l22 = _mm_alignr_epi8(l28, l20, 4);\
			__m128i l23 = _mm_alignr_epi8(l28, l20, 6);\
			__m128i l24 = _mm_alignr_epi8(l28, l20, 8);\
			__m128i l25 = _mm_alignr_epi8(l28, l20, 10);\
			__m128i l29 = _mm_alignr_epi8(l2G, l28, 2);\
			__m128i l2A = _mm_alignr_epi8(l2G, l28, 4);\
			__m128i l2B = _mm_alignr_epi8(l2G, l28, 6);\
			__m128i l2C = _mm_alignr_epi8(l2G, l28, 8);\
			__m128i h0 = noclip_6tap(l20, l21, l22, l23, l24, l25, zero);\
			__m128i h1 = noclip_6tap(l28, l29, l2A, l2B, l2C, l2D, zero);\
			__m128i h = _mm_packus_epi16(h0, h1);\
			__m128i d3##R = _mm_lddqu_si128((__m128i *)(p              + OFFSET));\
			__m128i d4##R = _mm_lddqu_si128((__m128i *)(p + stride     + OFFSET));\
			__m128i d5##R = _mm_lddqu_si128((__m128i *)(p + stride * 2 + OFFSET));\
			__m128i l3##R = _mm_unpacklo_epi8(d3##R, zero);\
			__m128i l3##S = _mm_unpackhi_epi8(d3##R, zero);\
			__m128i l4##R = _mm_unpacklo_epi8(d4##R, zero);\
			__m128i l4##S = _mm_unpackhi_epi8(d4##R, zero);\
			__m128i l5##R = _mm_unpacklo_epi8(d5##R, zero);\
			__m128i l5##S = _mm_unpackhi_epi8(d5##R, zero);\
			__m128i v0 = noclip_6tap(l0##R, l1##R, l2##R, l3##R, l4##R, l5##R, zero);\
			__m128i v1 = noclip_6tap(l0##S, l1##S, l2##S, l3##S, l4##S, l5##S, zero);\
			*(__m128i *)dst = _mm_avg_epu8(_mm_packus_epi16(v0, v1), h);\
			l0##R = l1##R, l0##S = l1##S;\
			l1##R = l2##R, l1##S = l2##S;\
		}\
		return ponderation_16x16();\
	}\

INTER16x16_QPEL_11_13(qpel11, 2, A, 0)
INTER16x16_QPEL_11_13(qpel13, 3, B, 1)
