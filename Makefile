CC = gcc
CFLAGS = -std=c11 -O2 -Wall `pkg-config --cflags wayland-client libzstd` -Iproto
LIBS = `pkg-config --libs wayland-client libzstd` -lm -lpthread

PROTO_XML = proto/wlr-screencopy-unstable-v1.xml
PROTO_HDR = proto/wlr-screencopy-unstable-v1-client-protocol.h
PROTO_SRC = proto/wlr-screencopy-unstable-v1-client-protocol.c

OBJS = src/main.o src/capture.o src/diff.o src/compress.o src/scale.o src/transport.o src/input.o $(PROTO_SRC:.c=.o)
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
