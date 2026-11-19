#
# Circle kernel Makefile: umac + Musashi + the Circle platform layer.
# Normally run from the top-level Makefile, which first builds
# circle-stdlib and generates the Musashi opcode tables into build/gen.
#

CIRCLESTDLIB = ../libs/circle-stdlib
include $(CIRCLESTDLIB)/Config.mk

UMAC    = ../external/umac
MUSASHI = $(UMAC)/external/Musashi
GEN     = ../build/gen

# Mac RAM in KB: 4096 = 4 MB Mac Plus (System 7 needs 2048 or more)
MEMSIZE ?= 4096

OBJS = main.o kernel.o emu_glue.o keymap_usb.o \
       $(UMAC)/src/main.o $(UMAC)/src/disc.o $(UMAC)/src/rom.o \
       $(UMAC)/src/scc.o $(UMAC)/src/via.o \
       $(MUSASHI)/m68kcpu.o $(MUSASHI)/m68kdasm.o \
       $(MUSASHI)/softfloat/softfloat.o $(GEN)/m68kops.o

include $(CIRCLEHOME)/Rules.mk

# Generated m68kops.h must win over anything else; umac's m68kconf.h is the CPU config
INCLUDE := -I$(GEN) -I. -I$(UMAC)/include -I$(MUSASHI) $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)
DEFINE  += -DMUSASHI_CNF=\"$(abspath $(UMAC))/include/m68kconf.h\" -DUMAC_MEMSIZE=$(MEMSIZE) \
           -DDISP_WIDTH=512 -DDISP_HEIGHT=342
OPTIMIZE = -O3

LIBS := $(CIRCLE_STDLIB_LIBS)

-include $(DEPS)
