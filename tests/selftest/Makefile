TARGET := selftest
VCPE ?= ../..
VCPE_MODULES := vcpe_sys vcpe_pak vcpe_gfx vcpe_test vcpe_audio vcpe_math vcpe_pad vcpe_save vcpe_log vcpe_hud vcpe_mouse vcpe_mem vcpe_load vcpe_co vcpe_ultra vcpe_jobs vcpe_pack vcpe_3d vcpe_lod
OBJS := src/main.o $(addprefix vcpe_obj/,$(addsuffix .o,$(VCPE_MODULES)))
INCDIR := src build $(VCPE)/src
CFLAGS := -O2 -G0 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -ffast-math
LIBS := -lpspgum -lpspgu -lpspctrl -lpsppower -lpspaudio -lm
BUILD_PRX := 1
PSP_FW_VERSION := 660
PSP_LARGE_MEMORY := 1
EXTRA_TARGETS := EBOOT.PBP
PSP_EBOOT_TITLE := VCPE selftest
PSPSDK ?= $(shell psp-config --pspsdk-path 2>/dev/null)
include $(PSPSDK)/lib/build.mak
vcpe_obj/%.o: $(VCPE)/src/%.c
	@mkdir -p vcpe_obj
	$(CC) $(CFLAGS) -c -o $@ $<
