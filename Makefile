CC = gcc
CFLAGS = -std=c11 -O2 -Wall `pkg-config --cflags wayland-client` -Iproto
LIBS = `pkg-config --libs wayland-client` -lm

PROTO_XML = proto/wlr-screencopy-unstable-v1.xml
PROTO_HDR = proto/wlr-screencopy-unstable-v1-client-protocol.h
PROTO_SRC = proto/wlr-screencopy-unstable-v1-client-protocol.c

OBJS = src/main.o src/capture.o src/kitty.o $(PROTO_SRC:.c=.o)
TARGET = kgp-test-bin

all: $(TARGET)

$(PROTO_HDR): $(PROTO_XML)
	wayland-scanner client-header $< $@

$(PROTO_SRC): $(PROTO_XML)
	wayland-scanner private-code $< $@

$(TARGET): $(PROTO_HDR) $(PROTO_SRC) $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(PROTO_HDR) $(PROTO_SRC)
