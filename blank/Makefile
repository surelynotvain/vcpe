# VCPE Blank Engine: the engine and nothing else (see src/main.c). Built by build.sh.
TARGET := vcpe_blank
VCPE ?= ..
VCPE_MODULES := vcpe_sys vcpe_pak vcpe_gfx vcpe_test vcpe_hud vcpe_math vcpe_pad vcpe_mem vcpe_load vcpe_log vcpe_ultra vcpe_jobs vcpe_pack vcpe_adapt vcpe_diag vcpe_prefetch vcpe_ttf vcpe_save vcpe_savedata
OBJS := src/main.o $(addprefix vcpe_obj/,$(addsuffix .o,$(VCPE_MODULES)))
INCDIR := src $(VCPE)/src
CFLAGS := -O2 -G0 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -ffast-math
LIBS := -lpspgu -lpspctrl -lpsppower -lpspaudio -lpsputility -lm
BUILD_PRX := 1
PSP_FW_VERSION := 660
PSP_LARGE_MEMORY := 1
EXTRA_TARGETS := EBOOT.PBP
PSP_EBOOT_TITLE := VCPE Blank Engine
PSP_EBOOT_ICON := ICON0.PNG
PSPSDK ?= $(shell psp-config --pspsdk-path 2>/dev/null)
include $(PSPSDK)/lib/build.mak
vcpe_obj/%.o: $(VCPE)/src/%.c
	@mkdir -p vcpe_obj
	$(CC) $(CFLAGS) -c -o $@ $<
