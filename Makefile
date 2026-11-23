CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O2 -std=c99
LDFLAGS ?= -lutil
PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
VERSION := 1.0.0

TARGET := s6-fbsplash
SRC := src/s6-fbsplash.c

.PHONY: all test install uninstall clean dist

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

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
	@rm -rf $(TARGET) dist

dist: clean
	@mkdir -p dist
	@tar --transform="s,^\.,s6-fbsplash-$(VERSION)," \
		--exclude='./.git*' --exclude='./dist*' \
		-czf "dist/s6-fbsplash-$(VERSION).tar.gz" .
	@(cd dist && sha256sum "s6-fbsplash-$(VERSION).tar.gz" > "s6-fbsplash-$(VERSION).tar.gz.sha256")
	@echo "✔ Built dist/s6-fbsplash-$(VERSION).tar.gz"
