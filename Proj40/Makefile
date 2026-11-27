# 轻量级用户态动态探针：双架构核心库、验证、演示和性能程序
CC       ?= gcc
AR       ?= ar
CFLAGS   ?= -O2 -g -Wall -Wextra -fno-strict-aliasing -pthread
LDFLAGS  ?= -pthread

TARGET_MACHINE := $(shell $(CC) -dumpmachine)
ifneq (,$(findstring aarch64,$(TARGET_MACHINE)))
ARCH_OBJS = aarch64.o aarch64_trampoline.o
ABI_SENTINEL = abi_state_aarch64.S
VECTOR_SENTINEL = simd_state_aarch64.S
else ifneq (,$(findstring x86_64,$(TARGET_MACHINE)))
ARCH_OBJS = x86dec.o trampoline.o
ABI_SENTINEL = abi_state_sentinel.S
VECTOR_SENTINEL = avx_state_sentinel.S
else
$(error Unsupported compiler target: $(TARGET_MACHINE); expected x86_64 or aarch64)
endif

LIB_OBJS = ludp.o probes.o elf.o $(ARCH_OBJS)
VALIDATION_BINS = instruction_validation probe_abi_validation probe_lifecycle_validation multiprobe_validation probe_extensions_validation callback_validation thread_signal_validation api_contract_validation aslr_pie_validation
DEMO_BINS = probe_demo event_stream_demo
BENCHMARK_BINS = getpid_benchmark
PROGRAMS = $(VALIDATION_BINS) $(DEMO_BINS) $(BENCHMARK_BINS)

.PHONY: all check test demo demo-full benchmark clean
all: libludp.a libprobe_targets.so $(PROGRAMS)

libludp.a: $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

libprobe_targets.so: probe_targets.c
	$(CC) $(CFLAGS) -fno-optimize-sibling-calls -fpatchable-function-entry=8 -shared -fPIC -o $@ $<

instruction_validation: instruction_validation.c libludp.a
	$(CC) $(CFLAGS) -o $@ $< libludp.a $(LDFLAGS)

probe_abi_validation: probe_abi_validation.c $(ABI_SENTINEL) libprobe_targets.so libludp.a
	$(CC) $(CFLAGS) -rdynamic -o $@ probe_abi_validation.c $(ABI_SENTINEL) libludp.a -ldl -L. -lprobe_targets -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

probe_lifecycle_validation: probe_lifecycle_validation.c libludp.a
	$(CC) $(CFLAGS) -fpatchable-function-entry=8 -o $@ $< libludp.a $(LDFLAGS)

multiprobe_validation: multiprobe_validation.c libludp.a
	$(CC) $(CFLAGS) -fpatchable-function-entry=8 -fno-ipa-icf -o $@ $< libludp.a $(LDFLAGS)

probe_extensions_validation: probe_extensions_validation.c libludp.a
	$(CC) $(CFLAGS) -fpatchable-function-entry=8 -fno-ipa-icf -o $@ $< libludp.a $(LDFLAGS)

callback_validation: callback_validation.c $(VECTOR_SENTINEL) libludp.a
	$(CC) $(CFLAGS) -fpatchable-function-entry=8 -fno-ipa-icf -o $@ callback_validation.c $(VECTOR_SENTINEL) libludp.a $(LDFLAGS)

thread_signal_validation: thread_signal_validation.c libludp.a
	$(CC) $(CFLAGS) -rdynamic -o $@ $< libludp.a -ldl $(LDFLAGS)

api_contract_validation: api_contract_validation.c libludp.a
	$(CC) $(CFLAGS) -rdynamic -fpatchable-function-entry=8 -o $@ $< libludp.a $(LDFLAGS)

aslr_pie_validation: aslr_pie_validation.c libprobe_targets.so libludp.a
	$(CC) $(CFLAGS) -fPIE -pie -rdynamic -fpatchable-function-entry=8 -o $@ $< libludp.a -L. -lprobe_targets -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

probe_demo: probe_demo.c libprobe_targets.so libludp.a
	$(CC) $(CFLAGS) -o $@ probe_demo.c libludp.a -ldl -L. -lprobe_targets -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

getpid_benchmark: getpid_benchmark.c libludp.a
	$(CC) $(CFLAGS) -fno-builtin -o $@ $< libludp.a -ldl $(LDFLAGS)

event_stream_demo: event_stream_demo.c libludp.a
	$(CC) $(CFLAGS) -fno-builtin -o $@ $< libludp.a -ldl $(LDFLAGS)

%.o: %.c ludp.h ludp_internal.h
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) -c -o $@ $<

$(PROGRAMS): validation_arch.h

check: all
	./instruction_validation
	./probe_abi_validation
	./probe_lifecycle_validation
	./multiprobe_validation
	./probe_extensions_validation
	./callback_validation
	./thread_signal_validation
	./api_contract_validation
	./aslr_pie_validation
	./probe_demo
	./event_stream_demo --standalone

# Keep the conventional target as an alias for existing automation.
test: check

benchmark: all
	python3 benchmark.py --rounds 5

demo:
	python3 ../scripts/run_demo.py

demo-full:
	python3 ../scripts/run_demo.py --full

clean:
	rm -f ludp.o probes.o elf.o x86dec.o trampoline.o aarch64.o aarch64_trampoline.o libludp.a libprobe_targets.so $(PROGRAMS)
