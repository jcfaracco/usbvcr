# Standalone usb_redir_hello – no external library dependencies.
CC      ?= gcc
CFLAGS  += -Wall -Wextra -O2 -g

TARGET = usb_redir_hello

all: $(TARGET)

$(TARGET): usb_redir_hello.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
