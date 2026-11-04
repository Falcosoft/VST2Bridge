# VST2Bridge with mingw-w64 (Debian / Ubuntu: apt install gcc-mingw-w64):  make
#   vst2bridge-x64.dll     64-bit bridge; the patcher copies it as <plugin>_x64.dll next to a 32-bit plugin
#   vst2bridge-x86.dll     32-bit bridge; the patcher copies it as <plugin>_x86.dll next to a 64-bit plugin
#   vst2bridge-host32.exe  helper that runs a 32-bit plugin (used by the 64-bit bridge)
#   vst2bridge-host64.exe  helper that runs a 64-bit plugin (used by the 32-bit bridge)
# make test  builds the test plugin and host and runs them under Wine (see test/run_tests.sh)
CC64 ?= x86_64-w64-mingw32-gcc
CC32 ?= i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -I.
HDRS = vst2.h protocol.h marshal.h
OUT = vst2bridge-x64.dll vst2bridge-x86.dll vst2bridge-host32.exe vst2bridge-host64.exe

all: $(OUT)
vst2bridge-x64.dll: stub.c stub.def $(HDRS)
	$(CC64) $(CFLAGS) -shared -o $@ stub.c stub.def -static-libgcc -s
vst2bridge-x86.dll: stub.c stub.def $(HDRS)
	$(CC32) $(CFLAGS) -shared -o $@ stub.c stub.def -static-libgcc -Wl,--enable-stdcall-fixup -s
vst2bridge-host32.exe: host.c $(HDRS)
	$(CC32) $(CFLAGS) -mwindows -o $@ host.c -static -s
vst2bridge-host64.exe: host.c $(HDRS)
	$(CC64) $(CFLAGS) -mwindows -o $@ host.c -static -s
test: all
	bash test/run_tests.sh
clean:
	rm -f $(OUT)
.PHONY: all test clean
