# VST2Bridge with mingw-w64 (Debian / Ubuntu: apt install gcc-mingw-w64):  make
#   vst2bridge-x64.dll     64-bit bridge; the patcher copies it as <plugin>_x64.dll next to a 32-bit plugin
#   vst2bridge-x86.dll     32-bit bridge; the patcher copies it as <plugin>_x86.dll next to a 64-bit plugin
#   vst2bridge-x64.clap    64-bit CLAP (stub.c is linked in); the patcher copies it as <plugin>_x64.clap
#   vst2bridge-x86.clap    32-bit CLAP (stub.c is linked in); the patcher copies it as <plugin>_x86.clap
#   vst2bridge-host32.exe  helper that runs a 32-bit plugin (used by the 64-bit bridge)
#   vst2bridge-host64.exe  helper that runs a 64-bit plugin (used by the 32-bit bridge)
# Linux CLAP (gcc, libxcb1-dev):  make linux  ->  vst2bridge-linux.clap  (the patcher copies it as <plugin>.clap)
# macOS CLAP (clang on a Mac):    make macos  ->  vst2bridge-macos       (universal arm64 + x86_64; the patcher puts it
#                                                  into a bundle <plugin>.clap)
# Both run the helpers above under Wine (stub.c is linked into the CLAP).
# make test  builds the test plugin and hosts and runs them under Wine (see test/run_tests.sh)
CC64 ?= x86_64-w64-mingw32-gcc
CC32 ?= i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -I.
HDRS = vst2.h vst2bridge.h protocol.h marshal.h plat.h
OUT = vst2bridge-x64.dll vst2bridge-x86.dll vst2bridge-x64.clap vst2bridge-x86.clap vst2bridge-host32.exe vst2bridge-host64.exe

all: $(OUT)
vst2bridge-x64.dll: stub.c stub.def $(HDRS)
	$(CC64) $(CFLAGS) -shared -o $@ stub.c stub.def -static-libgcc -s
vst2bridge-x86.dll: stub.c stub.def $(HDRS)
	$(CC32) $(CFLAGS) -shared -o $@ stub.c stub.def -static-libgcc -Wl,--enable-stdcall-fixup -s
vst2bridge-x64.clap: clap.c stub.c $(HDRS) clap/clap.h
	$(CC64) $(CFLAGS) -shared -o $@ clap.c stub.c -static-libgcc -static -s
vst2bridge-x86.clap: clap.c stub.c $(HDRS) clap/clap.h
	$(CC32) $(CFLAGS) -shared -o $@ clap.c stub.c -static-libgcc -static -Wl,--enable-stdcall-fixup -s
vst2bridge-host32.exe: host.c $(HDRS)
	$(CC32) $(CFLAGS) -mwindows -o $@ host.c -static -lws2_32 -s
vst2bridge-host64.exe: host.c $(HDRS)
	$(CC64) $(CFLAGS) -mwindows -o $@ host.c -static -lws2_32 -s
POSIX_SRC = clap.c stub.c
POSIX_HDRS = $(HDRS) clap/clap.h
linux: vst2bridge-linux.clap
vst2bridge-linux.clap: $(POSIX_SRC) $(POSIX_HDRS)
	gcc $(CFLAGS) -Wno-format-truncation -shared -fPIC -fvisibility=hidden -o $@ $(POSIX_SRC) -lxcb -lpthread -s
macos: vst2bridge-macos
vst2bridge-macos: $(POSIX_SRC) $(POSIX_HDRS)
	clang $(CFLAGS) -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -fvisibility=hidden -bundle -o $@ $(POSIX_SRC)
test: all
	bash test/run_tests.sh
clean:
	rm -f $(OUT) vst2bridge-linux.clap vst2bridge-macos
.PHONY: all linux macos test clean
