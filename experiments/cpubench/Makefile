# cpubench: make TARGET_STDLIB=<path to a configured circle-stdlib>
CIRCLESTDLIB ?= $(TARGET_STDLIB)
include $(CIRCLESTDLIB)/Config.mk

BII  = $(BASILISK)
UMAC = $(CURDIR)/../../external/umac
MUS  = $(UMAC)/external/Musashi

UAE_OBJS = uae/newcpu.o uae/readcpu.o uae/memory.o uae/cpudefs.o uae/cpuemu.o uae/cpustbl.o uae/cpufunctbl.o uae/fpu_ieee.o uae/stubs.o
MUS_OBJS = mus/m68kcpu.o mus/m68kdasm.o mus/m68kops.o mus/softfloat/softfloat.o

OBJS = main.o kernel.o bench_uae.o bench_musashi.o $(UAE_OBJS) $(MUS_OBJS)

CHECK_DEPS = 0
include $(CIRCLEHOME)/Rules.mk

INCLUDE := $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)
OPTIMIZE = -O2
LIBS := $(CIRCLE_STDLIB_LIBS)

# Each CPU core with its own headers (both have an m68k.h)
UAE_INC = -I. -Iuae -I$(BII)/Unix -I$(BII)/include -I$(BII)/uae_cpu_2021 -I$(BII)/CrossPlatform
bench_uae.o $(UAE_OBJS): INCLUDE := $(UAE_INC) $(INCLUDE)
bench_uae.o $(UAE_OBJS): STANDARD := -std=gnu++17
MUS_INC = -Imus -I. -I$(UMAC)/include -I$(MUS)
bench_musashi.o $(MUS_OBJS): INCLUDE := $(MUS_INC) $(INCLUDE)
bench_musashi.o $(MUS_OBJS): DEFINE += -DMUSASHI_CNF=\"$(UMAC)/include/m68kconf.h\" -DUMAC_MEMSIZE=8192

-include $(DEPS)
