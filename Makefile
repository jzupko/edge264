#
# Copyright (c) 2013-2014, Celticom / TVLabs
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 3. Neither the name of Celticom nor the names of its contributors may be used
#    to endorse or promote products derived from this software without specific
#    prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL CELTICOM BE LIABLE FOR ANY DIRECT,
# INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Author: Thibault Raffaillac <traf@kth.se>
#

#CFLAGS += -mssse3 -std=gnu99 -lSDL -DDISPLAY -2O

ifneq (,$(shell which clang))
CC = clang
else
  ifneq (,$(shell which gcc))
CC = gcc
  endif
endif

CFLAGS += -march=native -std=gnu99 -lSDL -DDISPLAY -O2 -Wall -Wno-unused-variable

ifeq ($(shell uname -s),Darwin)
CFLAGS += -DDARWIN -I/opt/local/include -L/opt/local/lib
endif

EDGE_SRCS = Edge264.c Edge264_common.h Edge264_decode.h \
            Edge264_CABAC.c Edge264_CABAC_init.c Edge264_decode_ssse3.c Makefile

all: edge264

edge264: Edge264_test.c $(EDGE_SRCS)
	$(CC) $(CFLAGS) -o $@ $<

test: edge264 test-video.264
	./edge264 < test-video.264
