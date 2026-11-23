CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O2 -std=c99
STB_CFLAGS ?= -O2 -std=c99 -Wno-unused-function -Wno-sign-compare
LDFLAGS ?= -lutil -lm
PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
VERSION := 1.1.0

TARGET := s6-fbsplash
SRC := src/s6-fbsplash.c
STB_SRC := src/stb_impl.c
STB_OBJ := stb_impl.o

.PHONY: all test install uninstall clean dist

all: $(TARGET)

$(STB_OBJ): $(STB_SRC) vendor/stb_image.h
	$(CC) $(STB_CFLAGS) -c -o $@ $<

$(TARGET): $(SRC) $(STB_OBJ)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(STB_OBJ) $(LDFLAGS)

test: $(TARGET)
	@bash tests/test_fbsplash.sh ./$(TARGET)

install: $(TARGET)
	@install -d "$(DESTDIR)$(BINDIR)"
	@install -m 0755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	@echo "✔ Installed $(TARGET) to $(DESTDIR)$(BINDIR)"

uninstall:
	@rm -f "$(DESTDIR)$(BINDIR)/$(TARGET)"
	@echo "✔ Uninstalled $(TARGET) from $(DESTDIR)$(BINDIR)"

clean:
	@rm -rf $(TARGET) $(STB_OBJ) dist

dist: clean
	@mkdir -p dist
	@tar --transform="s,^\.,s6-fbsplash-$(VERSION)," \
		--exclude='./.git*' --exclude='./dist*' \
		-czf "dist/s6-fbsplash-$(VERSION).tar.gz" .
	@(cd dist && sha256sum "s6-fbsplash-$(VERSION).tar.gz" > "s6-fbsplash-$(VERSION).tar.gz.sha256")
	@echo "✔ Built dist/s6-fbsplash-$(VERSION).tar.gz"
