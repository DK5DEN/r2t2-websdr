CC      ?= gcc
CFLAGS  ?= -O2 -ffast-math -Wall -Wextra -Wno-unused-parameter -std=gnu99
LDLIBS  := -lfftw3f -lm

ifeq ($(shell uname -m),armv7l)
CFLAGS  += -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -ftree-vectorize
endif

PREFIX  ?= /opt/r2t2sdr
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)

r2t2sdr: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: r2t2sdr
	install -d $(PREFIX)/www
	install -m 755 r2t2sdr $(PREFIX)/r2t2sdr
	install -m 644 www/*.html www/*.css www/*.js www/*.svg $(PREFIX)/www/
	test -f /etc/r2t2sdr.conf || install -m 644 dist/r2t2sdr.conf /etc/r2t2sdr.conf
	install -m 644 dist/r2t2sdr.service /etc/systemd/system/r2t2sdr.service
	systemctl daemon-reload

clean:
	rm -f r2t2sdr src/*.o

.PHONY: install clean
