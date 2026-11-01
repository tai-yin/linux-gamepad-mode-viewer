CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += $(shell pkg-config --cflags gtk+-3.0)
LDLIBS  += $(shell pkg-config --libs gtk+-3.0)

TARGET := gamepad-mode-gui

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
