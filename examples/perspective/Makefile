TARGET := vcpe_perspective_demo
VCPE ?= ../..
VCPE_MODULES := vcpe_sys vcpe_pak vcpe_gfx vcpe_test vcpe_hud vcpe_math vcpe_pad vcpe_mem vcpe_load vcpe_log vcpe_perspective vcpe_sector vcpe_raycast
OBJS := src/main.o $(addprefix vcpe_obj/,$(addsuffix .o,$(VCPE_MODULES)))
INCDIR := src $(VCPE)/src
CFLAGS := -O2 -G0 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -ffast-math -DVCPE_DLIST_KB=128 -MMD -MP
LIBS := -lpspgu -lpspctrl -lpsppower -lpspaudio -lm
BUILD_PRX := 1
PSP_FW_VERSION := 660
PSP_LARGE_MEMORY := 0
EXTRA_TARGETS := EBOOT.PBP
PSP_EBOOT_TITLE := VCPE Perspective Lite 1.02
PSPSDK ?= $(shell psp-config --pspsdk-path 2>/dev/null)
include $(PSPSDK)/lib/build.mak
vcpe_obj/%.o: $(VCPE)/src/%.c
	@mkdir -p vcpe_obj
	$(CC) $(CFLAGS) $(addprefix -I,$(INCDIR)) -c -o $@ $<
-include src/*.d vcpe_obj/*.d
