#
# Circle kernel Makefile: umac + Musashi + the Circle platform layer.
# Run by the top-level Makefile from a per-target copy, build/<target>/src,
# with CIRCLESTDLIB, UMAC and GEN pointing at that target's own copies.
#

CIRCLESTDLIB ?= ../circle-stdlib
include $(CIRCLESTDLIB)/Config.mk

UMAC    ?= ../umac
MUSASHI  = $(UMAC)/external/Musashi
GEN     ?= ../gen

# Mac RAM in KB: 4096 = 4 MB Mac Plus (System 7 needs 2048 or more)
MEMSIZE ?= 4096

OBJS = main.o kernel.o emu_glue.o keymap_usb.o mac_rtc.o mac_sound.o webserver.o \
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

# Wi-Fi (time from NTP): Circle's WLAN driver + WPA supplicant
LIBS := $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
	$(CIRCLEHOME)/addon/wlan/libwlan.a $(CIRCLEHOME)/lib/sound/libsound.a $(CIRCLE_STDLIB_LIBS)

-include $(DEPS)
