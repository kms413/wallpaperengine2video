CC      ?= gcc
CFLAGS  ?= -O2 -fPIC -Wall
SHIM    := lib/we_clock.so

.PHONY: all install clean

all: $(SHIM)

$(SHIM): lib/we_clock.c
	$(CC) $(CFLAGS) -shared -o $@ $< -lpthread

# 同时安装 CLI 与垫片（默认前缀 ~/.local）
install: $(SHIM)
	install -Dm755 wallpaperengine2video $(DESTDIR)$(HOME)/.local/bin/wallpaperengine2video
	install -Dm755 $(SHIM) $(DESTDIR)$(HOME)/.local/bin/lib/we_clock.so

clean:
	rm -f $(SHIM)
