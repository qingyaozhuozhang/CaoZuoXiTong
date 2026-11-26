# 轻量级用户态动态探针：只构建 F1–F3 所需的核心库与验证程序
CC       ?= gcc
AR       ?= ar
CFLAGS   ?= -O2 -g -Wall -Wextra -fno-strict-aliasing -pthread
LDFLAGS  ?= -pthread

TARGET := $(shell $(CC) -dumpmachine)
ifeq ($(findstring aarch64,$(TARGET)),aarch64)
ARCH_OBJS = arm64.o trampoline_arm64.o
ABI_TEST = test_abi_arm64.S
else ifeq ($(findstring x86_64,$(TARGET)),x86_64)
ARCH_OBJS = x86dec.o trampoline.o
ABI_TEST = test_abi.S
DECODER_TEST = test_decode
else
$(error Unsupported compiler target: $(TARGET))
endif
LIB_OBJS = ludp.o probes.o elf.o $(ARCH_OBJS)
TEST_BINS = $(DECODER_TEST) test_f123 test_f45 test_thread_state bench_f123 demo_f123

.PHONY: all test benchmark clean
all: libludp.a libtest.so $(TEST_BINS)

libludp.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

libtest.so: testlib.c
	$(CC) $(CFLAGS) -fno-optimize-sibling-calls -fpatchable-function-entry=8 -shared -fPIC -o $@ $<

test_decode: test_decode.c libludp.a
	$(CC) $(CFLAGS) -o $@ $< libludp.a $(LDFLAGS)

test_f123: test_f123.c $(ABI_TEST) libtest.so libludp.a
	$(CC) $(CFLAGS) -rdynamic -o $@ test_f123.c $(ABI_TEST) libludp.a -ldl -L. -ltest -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

test_f45: test_f45.c libludp.a
	$(CC) $(CFLAGS) -fpatchable-function-entry=8 -o $@ $< libludp.a $(LDFLAGS)

test_thread_state: test_thread_state.c libludp.a
	$(CC) $(CFLAGS) -rdynamic -o $@ $< libludp.a -ldl $(LDFLAGS)

demo_f123: demo_f123.c libtest.so libludp.a
	$(CC) $(CFLAGS) -o $@ demo_f123.c libludp.a -ldl -L. -ltest -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

bench_f123: bench_f123.c libludp.a
	$(CC) $(CFLAGS) -fno-builtin -o $@ $< libludp.a -ldl $(LDFLAGS)

%.o: %.c ludp.h ludp_internal.h
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) -c -o $@ $<

test: all
ifneq ($(DECODER_TEST),)
	./$(DECODER_TEST)
endif
	./test_f123
	./test_f45
	./test_thread_state
	./demo_f123

benchmark: all
	python3 benchmark.py --rounds 5 --output reports

clean:
	rm -f $(LIB_OBJS) x86dec.o trampoline.o arm64.o trampoline_arm64.o \
		libludp.a libtest.so test_decode $(TEST_BINS)
