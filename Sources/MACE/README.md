# MACE

The MACE 3:1 and 6:1 decoder, taken from FFmpeg's `libavcodec/mace.c`
(n7.1). This directory is licensed under the GNU Lesser General Public
License 2.1 or later (`COPYING.LGPLv2.1`); the rest of Blue Box is MIT
licensed and uses it only through `include/mace_decoder.h`. `mace_step` at
the end of `mace.c` is Blue Box's addition: the decoder's state machine one
code at a time, for the MACE encoder in `Sources/Toolbox/os9rest_sound.c`.
