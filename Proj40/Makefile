# ludp - Lightweight User-space Dynamic Probe (F6/F7 module)
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -fno-strict-aliasing -pthread
LDFLAGS ?= -pthread

LIB_SRCS = ludp.c probes.c x86dec.c elf.c trampoline.S
LIB_OBJS = $(LIB_SRCS:.S=.o)
LIB_OBJS := $(LIB_OBJS:.c=.o)

all: libctx.so target demo verify

libludp.a: $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

libctx.so: ctxlib.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ ctxlib.c

target: target.c libctx.so
	$(CC) $(CFLAGS) -fno-plt -fno-builtin -Wl,--no-as-needed -o $@ target.c -L. -lctx -Wl,-rpath,'$$ORIGIN' -lm -ldl $(LDFLAGS)

demo: demo.c libludp.a
	$(CC) $(CFLAGS) -o $@ demo.c libludp.a $(LDFLAGS)

verify: verify.c libludp.a
	$(CC) $(CFLAGS) -o $@ verify.c libludp.a $(LDFLAGS)

%.o: %.c ludp.h ludp_internal.h
	$(CC) $(CFLAGS) -c -o $@ $<

trampoline.o: trampoline.S
	$(CC) -c -o $@ $<

test: all
	./demo stress 4 6
	./demo bench 1000000

clean:
	rm -f target demo libludp.a *.o

.PHONY: all test clean
