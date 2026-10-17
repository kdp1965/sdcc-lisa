# Regression test specification for the lisa target running on lisa_sim
# (the C++ simulator in lisa-tools/lisa_sim, which models the TT07 silicon).
#
# Point LISA_SIM at the simulator binary if it is not next to this tree:
#   make test-lisa LISA_SIM=/path/to/lisa_sim
# LISA_REV2=1 runs the suite for the second chip: sdcc --rev2 (its stack
# convention, the lisa-rev2 library) on lisa_sim --rev2.  rm -rf gen/lisa
# results/lisa between the two modes: they share the directories.

LISA_SIM ?= $(top_srcdir)/../lisa_sim/lisa_sim
LISA_MODEL = $(if $(LISA_REV2),lisa-rev2,lisa)

# simulation budget in cycles; _exitEmu() executes `brk`, which halts the
# simulator, so a passing test never runs out the budget
SIM_CYCLES = 200000000

EMU = $(LISA_SIM) -b -c $(SIM_CYCLES) $(if $(LISA_REV2),--rev2)
EMU_INPUT =

ifdef SDCC_BIN_PATH
  AS = $(SDCC_BIN_PATH)/sdaslisa$(EXEEXT)
else
  AS = $(WINE) $(top_builddir)/bin/sdaslisa$(EXEEXT)
ifndef CROSSCOMPILING
  SDCCFLAGS += --nostdinc -I$(top_srcdir)
  LINKFLAGS += --nostdlib -L$(top_builddir)/device/lib/build/$(LISA_MODEL)
endif
endif

ifdef CROSSCOMPILING
  SDCCFLAGS += -I$(top_srcdir)
endif

# the simulator has 32K of data; the port's default stack (0x7f) is for the
# 128-byte RAM of the TT07 chip
SDCCFLAGS += -mlisa --less-pedantic --stack-loc 0x7fff $(if $(LISA_REV2),--rev2)
LINKFLAGS += lisa.lib

OBJEXT = .rel
BINEXT = .ihx

# otherwise `make` deletes testfwk.rel and `make -j` will fail
.PRECIOUS: $(PORT_CASES_DIR)/%$(OBJEXT)

# Required extras
EXTRAS = $(PORT_CASES_DIR)/testfwk$(OBJEXT) $(PORT_CASES_DIR)/support$(OBJEXT)
include $(srcdir)/fwk/lib/spec.mk

%$(OBJEXT): %.asm
	$(AS) -plosgff $<

_clean:
