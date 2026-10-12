# MemRsrc probe (68k). Configure with Retro68's toolchain file, e.g.
#   cmake -S Tests/Probes -B build-probes \
#     -DCMAKE_TOOLCHAIN_FILE=<Retro68-build>/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
# or just run Tests/Probes/build.sh.
add_application(MemRsrc
	probe.c
	probe.r
	CREATOR "CLpr"
	)
set_target_properties(MemRsrc PROPERTIES
	# MemError()/ResError() read low memory (0x220/0xA60); gcc thinks that is a
	# null-pointer array access.
	COMPILE_OPTIONS "-Wall;-Wno-array-bounds;-ffunction-sections"
	LINK_FLAGS "-Wl,-gc-sections")
