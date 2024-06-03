### Hot news 🎉: I'll present the main coding techniques used in edge264 at [FOSDEM'24](https://fosdem.org/2024/schedule/event/fosdem-2024-2931-innovations-in-h-264-avc-software-decoding-architecture-and-optimization-of-a-block-based-video-decoder-to-reach-10-faster-speed-and-3x-code-reduction-over-the-state-of-the-art-/), Open Media room, Brussels, 4 February 2024. See you there!

edge264
=======

This is a minimalist software decoder for the H.264 video codec, written from scratch to experiment new programming techniques in order to improve performance and code simplicity over existing decoders.
It is in open beta at the moment, please fill issues in particular for missing API features (and how you would like them implemented), or planned features that are most important to you.


Supported features
------------------

* Progressive High Profile and level 6.2
* MVC 3D support
* Any resolution up to 8K UHD
* 8-bit 4:2:0 planar YUV output
* CAVLC/CABAC
* I/P/B frames
* Deblocking
* 4x4 and 8x8 transforms
* Slices and Arbitrary Slice Order
* Per-slice reference lists
* Memory Management Control Operations
* Long-term reference pictures


Planned features
----------------

* SEI messages
* Error concealment
* ARM support
* PAFF and MBAFF
* AVX-2 optimizations
* 4:0:0, 4:2:2 and 4:4:4
* 9-14 bit depths with possibility of different luma/chroma depths
* Transform-bypass for macroblocks with QP==0
* Slice-multithreading


Compiling and testing
---------------------

edge264 is built and tested with GNU GCC and LLVM Clang, supports 32/64 bit architectures, and requires 128 bit SIMD support. Processor support is currently limited to Intel x86 or x64 with at least SSSE3. [GLFW3](https://www.glfw.org/) development headers should be installed to compile `edge264_play`. `gcc-9` is recommended since it provides the fastest performance in practice.
The build process will output an object file (e.g. `edge264-gcc-9.o`), which you may then use to link to your code.

```sh
$ make CC=gcc-9 # best performance
$ ffmpeg -i video.mp4 -vcodec copy -bsf h264_mp4toannexb -an video.264 # optional, converts from MP4 format
$ ./edge264_play-gcc-9 video.264 # add -b to benchmark without display
```

When debugging, the make flag `TRACE=1` enables printing headers to stdout in HTML format, and `TRACE=2` adds the dumping of all other symbols to stderr (*very large*). An automated test program is also provided, that browses files in a given directory, decoding each `<video>.264` and comparing its output with the pair `<video>.yuv` if found. On the set of official [conformance bitstreams](https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/), 89 files are known to decode perfectly, the rest using yet unsupported features.

```sh
$ ./edge264_test-gcc-9 --help
```


API documentation
-----------------

Here is a complete example that opens an input file from command line and dumps its decoded frames in planar YUV order to standard output.

```c
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "edge264.h"

int main(int argc, char *argv[]) {
	int f = open(argv[1], O_RDONLY);
	struct stat st;
	fstat(f, &st);
	uint8_t *buf = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, f, 0);
	Edge264_stream *s = Edge264_alloc();
	s->CPB = buf + 3 + (buf[2] == 0); // skip the [0]001 delimiter
	s->end = buf + st.st_size;
	int res;
	do {
		res = Edge264_decode_NAL(s);
		while (!Edge264_get_frame(s, res == -3)) { // drain remaining frames when at end of buffer
			for (int y = 0; y < s->height_Y; y++)
				write(1, s->samples[0] + y * s->stride_Y, s->width_Y);
			for (int y = 0; y < s->height_C; y++)
				write(1, s->samples[1] + y * s->stride_C, s->width_C);
			for (int y = 0; y < s->height_C; y++)
				write(1, s->samples[2] + y * s->stride_C, s->width_C);
		}
	} while (!res);
	Edge264_free(&s);
	munmap(buf, st.st_size);
	close(f);
	return 0;
}
```


#### `Edge264_stream *Edge264_alloc()`

Allocate and return a decoding context, that is used to pass and receive parameters.
The private decoding context is actually hidden at negative offsets from the pointer returned.

#### `int Edge264_decode_NAL(Edge264_stream *s)`

Decode a single NAL unit, for which `s->CPB` should point to its first byte (containing `nal_unit_type`) and `s->end` should point to the first byte past the buffer.
After decoding the NAL, `s->CPB` is automatically advanced past the next start code (for Annex B streams).
Return codes are:

* **-3** if the function was called while `s->CPB >= s->end`
* **-2** if the Decoded Picture Buffer is full and `Edge264_get_frame` should be called before proceeding
* **-1** if the function was called with `s == NULL`
* **0** on success
* **1** on unsupported stream (decoding may proceed but could return zero frames)
* **2** on decoding error (decoding may proceed but could show visual artefacts, if you can check with another decoder that the stream is actually flawless, please consider filling a bug report 🙏)

#### `int Edge264_get_frame(Edge264_stream *s, int drain)`

Check the Decoded Picture Buffer for a pending displayable frame, and pass it in `s`.
While reference frames may be decoded ahead of their actual display (ex. B-Pyramid technique), all frames are buffered for reordering before being released for display:

* Decoding a non-reference frame releases it and all frames set to be displayed before it.
* Decoding a key frame releases all stored frames (but not the key frame itself which might be reordered later).
* Exceeding the maximum number of frames held for reordering releases the next frame in display order.
* Lacking an available frame buffer releases the next non-reference frame in display order (to salvage its buffer) and all reference frames displayed before it.
* Setting `drain` considers all frames ready for display, which may help reduce latency if you know that no frame reordering will occur (e.g. for videoconferencing or at end of stream). This is especially useful since the base spec offers no way to signal that a stored frame is ready for display, so many streams will fill the frame buffer before actually getting frames.

Return codes are:

* **-2** if there is no frame pending for display
* **-1** if the function was called with `s == NULL`
* **0** on success (one frame is returned)

#### `void Edge264_free(Edge264_stream **s)`

Deallocate the entire decoding context, and unset the stream pointer.

#### `const uint8_t *Edge264_find_start_code(int n, const uint8_t *CPB, const uint8_t *end)`

Scan memory for the next three-byte 00n pattern, returning a pointer to the first following byte (or `end` if no pattern was found).


Key takeaways
-------------

* [Minimalistic API](edge264.h) with FFI-friendly design (5 functions and 1 structure).
* [The input bitstream](edge264_bitstream.c) is unescaped on the fly using vector code, avoiding a full preprocessing pass to remove escape sequences, and thus reducing memory reads/writes.
* [Error detection](edge264.c) is performed once in each type of NAL unit (search for `return` statements), by clamping all input values to their expected ranges, then expecting `rbsp_trailing_bit` afterwards (with _very high_ probability of catching an error if the stream is corrupted). This design choice is discussed in [A case about parsing errors](https://traffaillac.github.io/parsing.html).
* [The bitstream caches](edge264_internal.h) for CAVLC and CABAC (search for `rbsp_reg`) are stored in two size_t variables each, mapped on Global Register Variables if possible, speeding up the _very frequent_ calls to input functions. The main context pointer is also assigned to a GRV, to help reduce the binary size (\~200k).
* [The main decoding loop](edge264_slice.c) is carefully designed with the smallest code and fewest number of conditional branches, to ease its readability and upgradeability. Its architecture is a forward pipeline loosely resembling hardware decoders, using tail calls to pass execution between code blocks.
* [The decoding of input symbols](edge264_slice.c) is interspersed with their parsing (instead of parsing to a `struct` then decoding the data). It deduplicates branches and loops that are present in both parsing and decoding, and even eliminates the need to store some symbols (e.g. mb_type, sub_mb_type, mb_qp_delta).
* [Neighbouring values](edge264_internal.h) are retrieved using precomputed memory offsets (search for `neighbouring offsets`) rather than intermediate caches. It spares the code for initializing caches and storing them back afterwards, thus reducing memory writes overall.
* [Loops with nested conditions](edge264_slice.c) are implemented with bitmasks (`while (mask) { i = ctz(mask); ...; mask &= mask - 1; }` instead of `for (int i = 0; i < 32; i++) if (f(i)) { ...; }`). They are used to reduce branch mispredictions when conditionally parsing motion vectors and DCT coefficients.
* [GCC's Vector Extensions](edge264_internal.h) are used extensively in the entire decoder, to exploit _any_ opportunity for vectorizing, and to reduce future efforts for porting edge264 to new architectures.
* Machine-specific portions of the decoder are implemented with C intrinsics instead of assembly code. This is especially useful when designing kernels maximizing the use of vector registers, to avoid micro-managing spills on the stack (e.g. [8x8 IDCT](edge264_residual.c), [16x16 Inter predictors](edge264_inter.c)). It also simplifies the build process, and makes the code more readable by aligning sequences of identical instructions, rather than interspersing them with move instructions.
