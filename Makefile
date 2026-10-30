# MHGU Recomp build
# Windows: MSYS2 UCRT64 (clang, lld, make; see README.md). Linux: clang (planned).
#
#   make gen GAME=<folder with exefs/ and romfs/>   recompile the game code from your own dump into generated/
#   make -j4                                        build build/mhgurecomp(.exe) and the game code library, with its
#                                                   DLLs and the game files (build/data): start build/mhgurecomp.exe
# build/ holds only the game folder (what a player copies); the compiler's files go to obj/.
UNAME := $(shell uname -s)
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else
PLATFORM := linux
endif

GAME     ?= ../mhgu/game
GEN      ?= generated
B        ?= build
O        ?= obj
CC       := clang
CXX      := clang++
GENARCH  ?= -march=bdver3 -mtune=bdver3 -mno-avx2 -mno-bmi2 -mno-movbe  # AMD A10-7800 (Steamroller) / Non-AVX2
GENFLAGS := -O1 -fno-strict-aliasing -ffp-contract=off -fno-math-errno -Wno-return-type -Wno-unused-label \
            -Wno-parentheses-equality -Wno-unused-value -fvisibility=hidden -Iruntime $(GENARCH)

GEN_SRC  := $(wildcard $(GEN)/main/*.c) $(wildcard $(GEN)/sdk/*.c)
GEN_OBJ  := $(patsubst $(GEN)/%.c,$(O)/gen/%.o,$(GEN_SRC)) $(O)/gen/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) $(wildcard runtime/net/*.c) \
            runtime/nvn/objects.c runtime/nvn/cmdbuf.c runtime/nvn/nvn_args.c runtime/nvn/tegra.c runtime/nvn/formats.c \
            runtime/nvn/backend_null.c runtime/nvn/astc.c runtime/nvn/maxwell/translate.c $(wildcard runtime/nvn/vulkan/*.c) \
            runtime/platform/$(PLATFORM).c
# (simd_int.c / neon.c, helpers of the recompiled code, live in the executable: the game code library imports them,
# and runtime/interp.c uses them too)
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) $(wildcard runtime/nvn/*/*.h)

# FFmpeg (the game's movies, runtime/hle/movie.c): by default a small static build of a pinned FFmpeg release, made
# here from its source (downloaded once to third_party/ffmpeg/, checked against FFMPEG_SHA256) with only what the movies
# use: MP4 holding H.264 video and AAC audio. Nothing to install or ship (Linux distributions carry different FFmpeg
# versions, and an executable linked to one doesn't start with another), and no GPL parts: FFmpeg's LGPL 2.1 only.
# FFMPEG=system links the system's own avformat / avcodec / avutil instead.
FFMPEG        ?= bundled
FFMPEG_VER    := 9.0.2
FFMPEG_SHA256 := 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
FFMPEG_TAR    := third_party/ffmpeg/ffmpeg-$(FFMPEG_VER).tar.xz
FFMPEG_DIR    := $(O)/ffmpeg-$(FFMPEG_VER)
FFMPEG_LIBS   := $(addprefix $(FFMPEG_DIR)/lib/,libavformat.a libavcodec.a libavutil.a)
FFMPEG_CONF   := --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
                 --disable-debug --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
                 --enable-static --disable-shared --enable-protocol=file --enable-demuxer=mov \
                 --enable-parser=h264,aac --enable-decoder=h264,aac --disable-avx2 --disable-avx512 --extra-cflags="$(GENARCH)"
# (nasm: FFmpeg's assembly for x86; without it the plain C versions, fast enough for 960x540 movies)
FFMPEG_CONF   += $(if $(shell command -v nasm 2>/dev/null),,--disable-x86asm)
ifeq ($(FFMPEG),system)
AV_LIBS  := -lavformat -lavcodec -lavutil
AV_DEPS  :=
else
AV_DEPS  := $(FFMPEG_LIBS)
AV_LIBS  :=
endif

ifeq ($(PLATFORM),windows)
EXE      := $(B)/mhgurecomp.exe
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_RAND_S -D_POSIX_THREAD_SAFE_FUNCTIONS
GENFLAGS += -D_FILE_OFFSET_BITS=64
# (the GCC runtime - libstdc++, libgcc, winpthreads - linked in: three DLLs fewer to ship, and their licenses ask no
# more than their notices then; build/licenses has those)
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 -lSDL3 $(AV_LIBS) -lvulkan-1 -lshaderc_shared -lwinmm -lole32 -lshell32 -luuid -lws2_32 \
            -lbcrypt -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic \
            -Wl,--export-all-symbols -Wl,--out-implib,$(O)/mhgurecomp.dll.a
GAMELIB  := $(B)/mhgugame.dll
GAMELIB_LDFLAGS := -shared -fuse-ld=lld -static-libgcc $(O)/mhgurecomp.dll.a
else
EXE      := $(B)/mhgurecomp
OSFLAGS  :=
GENFLAGS += -fPIC
FFMPEG_CONF += --enable-pic
LDFLAGS  := -fuse-ld=lld -rdynamic -lSDL3 $(AV_LIBS) -lvulkan -lshaderc_shared -lpthread -ldl -lm
GAMELIB  := $(B)/mhgugame.so
GAMELIB_LDFLAGS := -shared -fuse-ld=lld
endif

# GAMELIB_ID: what the game code library is compiled against. A library with another id is refused at startup.
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/neon.py tools/nso.py tools/gamelib_entry.c \
                runtime/hle/hle_symbols.txt runtime/cpu.h runtime/vfp.h runtime/neon_rt.h runtime/gamelib.h
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | sha256sum | cut -c1-16)

RT_OBJ   = $(patsubst runtime/%.c,$(O)/rt/%.o,$(RT_SRC)) $(RT_CXX) $(O)/rt/overlay.o $(IMGUI_OBJ)
# Dear ImGui with cimgui's C API (third_party/cimgui: Forge plugins' ImGui, runtime/forge_imgui.c, overlay.cpp)
IMGUI    := third_party/cimgui
IMGUI_SRC := $(IMGUI)/cimgui.cpp $(addprefix $(IMGUI)/imgui/,imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui_demo.cpp)
IMGUI_SRC += $(if $(filter 1,$(NULL_GPU)),,$(IMGUI)/imgui/backends/imgui_impl_vulkan.cpp)
IMGUI_OBJ := $(patsubst $(IMGUI)/%.cpp,$(O)/imgui/%.o,$(IMGUI_SRC))
IMGUIFLAGS := -O2 -g -DCIMGUI_NO_EXPORT -I$(IMGUI) -I$(IMGUI)/imgui $(GENARCH)
# (vendored VulkanMemoryAllocator fallback: -I on a missing dir is ignored, so MSYS2 builds
# with the system vulkan-memory-allocator package are unaffected)
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime -Ithird_party/vk_mem_alloc -I$(IMGUI) $(OSFLAGS) $(GENARCH)
# NULL_GPU=1: no renderer (runtime/nvn/backend_null.c: the game runs, nothing is drawn)
ifeq ($(NULL_GPU),1)
RTFLAGS  += -DMHGU_NULL_BACKEND
RT_SRC   := $(filter-out runtime/nvn/vulkan/%,$(RT_SRC))
RT_CXX   :=
else
RT_CXX   := $(O)/rt/nvn/vulkan/vma.o
endif

all: $(EXE) $(if $(strip $(GEN_SRC)),$(GAMELIB)) $(if $(filter windows,$(PLATFORM)),bundle) data licenses

$(O)/gen/%.o: $(GEN)/%.c runtime/cpu.h runtime/vfp.h runtime/neon_rt.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@


# the id as a file, rewritten only when it changes: both sides of the library interface rebuild together
$(O)/gamelib.id: FORCE
	@mkdir -p $(O); [ "$$(cat $@ 2>/dev/null)" = '$(GAMELIB_ID)' ] || echo '$(GAMELIB_ID)' > $@

$(O)/gen/tools/gamelib_entry.o: tools/gamelib_entry.c $(O)/gamelib.id
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -c $< -o $@
$(O)/rt/gamelib.o: $(O)/gamelib.id
$(O)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'

$(O)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

$(O)/rt/forge_imgui.o: runtime/forge_imgui_thunks.inc $(IMGUI)/cimgui.h
$(O)/rt/forge_imgui.o: RTFLAGS += -DCIMGUI_NO_EXPORT -Wno-unused-variable

$(O)/rt/overlay.o: runtime/overlay.cpp runtime/overlay.h runtime/forge.h
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUIFLAGS) $(if $(filter 1,$(NULL_GPU)),-DMHGU_NULL_BACKEND) -Wall -Iruntime -c $< -o $@

$(O)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUIFLAGS) -w -c $< -o $@

$(O)/rt/nvn/vulkan/vma.o: runtime/nvn/vulkan/vma.cpp
	@mkdir -p $(dir $@)
	$(CXX) -O2 -w $(GENARCH) -Ithird_party/vk_mem_alloc -c $< -o $@

$(EXE): $(RT_OBJ) $(AV_DEPS)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $^ $(LDFLAGS)

# the bundled FFmpeg (see FFMPEG above): download, check, configure, build into $(FFMPEG_DIR)
$(FFMPEG_TAR):
	@mkdir -p $(dir $@)
	curl -fL --retry 3 -o $@.part https://ffmpeg.org/releases/ffmpeg-$(FFMPEG_VER).tar.xz
	echo "$(FFMPEG_SHA256)  $@.part" | sha256sum -c -
	mv -f $@.part $@

$(FFMPEG_DIR)/.built: $(FFMPEG_TAR)
	rm -rf $(FFMPEG_DIR) && mkdir -p $(FFMPEG_DIR)/src
	tar -xJf $< -C $(FFMPEG_DIR)/src --strip-components=1
	cd $(FFMPEG_DIR)/src && ./configure --prefix="$(abspath $(FFMPEG_DIR))" --cc=$(CC) $(FFMPEG_CONF) > ../configure.log \
	    || { tail -20 ../configure.log; exit 1; }
	$(MAKE) -C $(FFMPEG_DIR)/src install > $(FFMPEG_DIR)/build.log || { tail -30 $(FFMPEG_DIR)/build.log; exit 1; }
	touch $@
	@echo "ffmpeg: FFmpeg $(FFMPEG_VER) (H.264 + AAC in MP4) built in $(FFMPEG_DIR)"

$(FFMPEG_LIBS): $(FFMPEG_DIR)/.built ;

ifneq ($(FFMPEG),system)
$(O)/rt/hle/movie.o: $(FFMPEG_DIR)/.built
$(O)/rt/hle/movie.o: RTFLAGS += -I$(FFMPEG_DIR)/include
endif

# The licenses a copy of the game folder must carry, in $(B)/licenses: this program's own (LICENSE) and its
# THIRD_PARTY_NOTICES (code compiled in: LAS Recomp, Dear ImGui, cimgui, VulkanMemoryAllocator), FFmpeg's LGPL (in the
# executable), and on Windows those of the MSYS2 packages behind the DLLs bundled and the libraries linked in
# (bundle: tools/bundle_dlls.py, which lists them all with their versions and sources in licenses/README.txt).
licenses: $(B)/licenses/LICENSE.txt $(B)/licenses/THIRD_PARTY_NOTICES.txt $(if $(AV_DEPS),$(B)/licenses/FFmpeg.txt)
$(B)/licenses/%.txt: %
	@mkdir -p $(dir $@)
	cp $< $@
$(B)/licenses/FFmpeg.txt: $(FFMPEG_DIR)/.built
	@mkdir -p $(dir $@)
	@{ echo "This program contains FFmpeg $(FFMPEG_VER) (libavformat, libavcodec, libavutil), linked statically, under the"; \
	   echo "GNU Lesser General Public License 2.1 or later (below). Its source code is the release at"; \
	   echo "https://ffmpeg.org/releases/ffmpeg-$(FFMPEG_VER).tar.xz, built with the options in this program's Makefile (FFMPEG_CONF);"; \
	   echo "the program's own source is at https://github.com/morerokk/MHGURecomp, to rebuild it with a changed FFmpeg."; \
	   echo; cat $(FFMPEG_DIR)/src/COPYING.LGPLv2.1; } > $@

$(GAMELIB): $(GEN_OBJ) | $(EXE)
	@mkdir -p $(O)
	$(CC) $(GAMELIB_LDFLAGS) -o $(O)/gamelib.tmp $^
	mv -f $(O)/gamelib.tmp $@

PY := $(firstword $(wildcard tools/venv/Scripts/python.exe) $(wildcard tools/venv/bin/python) python3)

# The build folder is the game folder: mhgurecomp.exe starts from it on its own (runtime/main.c), with
# - bundle (Windows): the MSYS2 DLLs it needs beside it (tools/bundle_dlls.py; not the GPU driver's vulkan-1.dll)
# - data: the game files from $(GAME) in $(B)/data/exefs and $(B)/data/romfs, once (hard links where the file system
#   allows them - no second copy on disk, and deleting them leaves $(GAME) alone - else copies).
# Nothing else: a player copies it as a whole into their game folder (or over it: their settings, caches, mods/ and
# log.txt are written only when the game runs). run-game.bat runs it with those in run/ (MHGU_HOME), not here.
# (--with: MSYS2 packages linked into the executable or into a bundled DLL, whose licenses go along too: the static
# GCC runtime, VulkanMemoryAllocator, and glslang / SPIRV-Tools inside shaderc's DLL)
LICENSE_PKGS := libstdc++ libgcc libwinpthread vulkan-memory-allocator glslang spirv-tools
bundle: $(EXE) $(if $(strip $(GEN_SRC)),$(GAMELIB))
	@$(PY) tools/bundle_dlls.py "$$(cygpath -m "$$(dirname "$$(command -v $(CC))")")" $(B) $^ \
	    --licenses $(B)/licenses --with $(LICENSE_PKGS)

data: $(B)/data/.complete
$(B)/data/.complete:
	@if [ ! -f "$(GAME)/exefs/main" ] || [ ! -d "$(GAME)/romfs" ]; then \
	    echo "data: no game files in $(GAME) (make GAME=<folder with exefs/ and romfs/>): $(B)/data left empty"; \
	else \
	    mkdir -p $(B)/data && for d in exefs romfs; do \
	        rm -rf "$(B)/data/$$d.tmp"; \
	        cp -rl "$(GAME)/$$d" "$(B)/data/$$d.tmp" 2>/dev/null || { rm -rf "$(B)/data/$$d.tmp"; \
	            echo "data: no hard links here, copying $$d"; cp -r "$(GAME)/$$d" "$(B)/data/$$d.tmp"; } || exit 1; \
	        rm -rf "$(B)/data/$$d" && mv "$(B)/data/$$d.tmp" "$(B)/data/$$d" || exit 1; \
	    done && touch $@ && echo "data: game files in $(B)/data"; \
	fi

gen:
	cd tools && ../$(PY) gen.py sdk ../$(GAME)/exefs/sdk 0x02400000 ../$(GEN)/sdk --hle ../runtime/hle/hle_symbols.txt
	cd tools && ../$(PY) gen.py main ../$(GAME)/exefs/main 0x00200000 ../$(GEN)/main

# differential test of the recompiler against Unicorn (tools/test_lift.py)
test-lift:
	cd tools && ../$(PY) test_lift.py ../$(GAME)/exefs/main 0x00200000 --tests 6000
	cd tools && ../$(PY) test_lift.py ../$(GAME)/exefs/sdk 0x02400000 --tests 6000

runtime-objs: $(RT_OBJ)

clean:  # (keeps $(B)/data, and the settings, caches and saves wherever the game ran)
	rm -rf $(O) $(B)/*.dll $(B)/*.so $(EXE) $(B)/vkprobe.exe

.PHONY: all gen test-lift runtime-objs clean bundle data licenses FORCE
