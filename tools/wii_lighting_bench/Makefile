#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

TARGET   := wii_lighting_bench
BUILD    := build
ROOT     := ../..
SOURCES  := $(ROOT)/source $(ROOT)/tests
DATA     :=
INCLUDES := $(ROOT)/source $(ROOT)/third_party

CFLAGS   := -std=c99 -O3 -Wall -Wextra -Wno-unused-parameter -DPLATFORM_WII -DCAVEWII_WII_BENCH_STANDALONE $(MACHDEP) $(INCLUDE)
CXXFLAGS := $(CFLAGS)
LDFLAGS  := -g $(MACHDEP) -Wl,-Map,$(notdir $@).map
LIBS     := -lwiiuse -lbte -lfat -logc -lm
LIBDIRS  :=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH  := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                 $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := lighting.c stack.c bench_lighting_budget.c
CPPFILES :=
sFILES   :=
SFILES   :=
BINFILES :=

ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif

export OFILES := $(addsuffix .o,$(BINFILES)) \
                 $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
                 $(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD) \
                  -I$(LIBOGC_INC)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                   -L$(LIBOGC_LIB)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
