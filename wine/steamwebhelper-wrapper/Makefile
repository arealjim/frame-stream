# From notpop/steam-on-m1-wine (MIT, LICENSE beside this file); changed: SRC path, CC.
#
# Cross-compile the steamwebhelper wrapper from macOS for Windows x86_64.
#
#   make         build ./steamwebhelper.exe
#   make clean   remove build artefacts
#
# Requires Homebrew's mingw-w64 toolchain:
#   brew install mingw-w64

TARGET  ?= steamwebhelper.exe
CC       = x86_64-w64-mingw32-gcc   # make defines CC, so ?= never applied (changed from notpop's)
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -municode -DUNICODE -D_UNICODE
LDFLAGS ?= -static -lshell32 -mwindows
SRC      = steamwebhelper-wrapper.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
