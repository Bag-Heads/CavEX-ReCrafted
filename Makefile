#---------------------------------------------------------------------------------
# Clear the implicit built in rules
#---------------------------------------------------------------------------------
.SUFFIXES:
.SECONDARY:
#---------------------------------------------------------------------------------
# prevent deletion of implicit targets
#---------------------------------------------------------------------------------
.SECONDARY:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

#---------------------------------------------------------------------------------
# Sidecar libogc gate (maintainer standing rule; docs/wii_libogc_kernel_heap_sidecar.md,
# third_party/libogc-kernel-heap/README.md). Every real-Wii .dol MUST link the patched
# 256KB-KERNEL_HEAP libogc sidecar -- the stock devkitPro libogc's 64KB lwp KERNEL_HEAP
# silently starves wiiuse's Wiimote attach-time allocation (18,432B needed, ~4,932B free
# on the stock pool), so the remote pages/links but never delivers a single input report.
#
# wii_rules (just included, immediately above) sets LIBOGC_LIB via a plain ':=' assignment
# (`export LIBOGC_LIB := $(DEVKITPRO)/libogc/lib/wii`). Per GNU make's variable-precedence
# rules, a COMMAND-LINE assignment (`make LIBOGC_LIB=<stage-lib>`) always overrides a plain
# in-makefile assignment like that one -- including through a recursive $(MAKE) re-invocation
# (verified empirically: origin stays "command line" in the build/ sub-make) -- but a plain
# `export LIBOGC_LIB=...` in the calling shell does NOT survive it (make's default "makefile
# wins over environment" rule re-clobbers it back to the stock path). So $(origin LIBOGC_LIB)
# reporting "file" at this point means nobody pointed this link at the sidecar via the
# documented `make ... LIBOGC_LIB=<stage-lib>` command-line form -- it is about to silently
# link the stock, Wiimote-starving libogc. Fail loudly instead, unless the caller opts in.
#
# Scoped out of `make clean`: cleaning never links anything, so it never needed
# LIBOGC_LIB in the first place -- gating it too would just be an unrelated
# annoyance (e.g. every wrapper script's own pre-build `make clean` step).
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(origin LIBOGC_LIB),file)
ifneq ($(CW_ALLOW_STOCK_LIBOGC),1)
$(error LIBOGC_LIB was not set on the command line -- refusing to link stock libogc for a \
real-Wii build. Stock libogc's 64KB lwp KERNEL_HEAP silently starves the Wiimote's attach- \
time allocation (see third_party/libogc-kernel-heap/README.md). Build the patched 256KB \
sidecar with tools/build_libogc_sidecar.sh, then pass LIBOGC_LIB=<stage-lib> on the make \
command line -- see docs/wii_libogc_kernel_heap_sidecar.md for the exact command. Hosted \
CI's stock-libogc artifact build may set CW_ALLOW_STOCK_LIBOGC=1 to bypass this gate on \
purpose; that artifact is NOT valid for hardware.)
endif
$(warning ****************************************************************)
$(warning * STOCK LIBOGC - NOT FOR HARDWARE                              *)
$(warning * CW_ALLOW_STOCK_LIBOGC=1 set: linking the stock 64KB-KERNEL_HEAP *)
$(warning * libogc. The Wiimote will NOT work on real Wii hardware with  *)
$(warning * this artifact (third_party/libogc-kernel-heap/README.md).    *)
$(warning * The output .dol is renamed with a "-stock" suffix so it can  *)
$(warning * never be mistaken for the hardware-certified build.          *)
$(warning ****************************************************************)
# Marks the artifact (TARGET gets a "-stock" suffix below) so a CI/PC-smoke-test-only
# stock-libogc build can never be staged/mistaken for the hardware-certified release.
CW_STOCK_LIBOGC := 1
endif
ifeq ($(wildcard $(LIBOGC_LIB)/libogc.a),)
$(error LIBOGC_LIB=$(LIBOGC_LIB) has no libogc.a -- not a valid libogc lib directory. Check \
the path (see docs/wii_libogc_kernel_heap_sidecar.md / tools/build_libogc_sidecar.sh if this \
was meant to be the sidecar stage-lib).)
endif
endif # MAKECMDGOALS != clean

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
#---------------------------------------------------------------------------------
# The shipped artifact is always CaveWii_v2.dol (the build-wii skill, SD staging as boot.dol,
# and the CI upload-artifact step all assume that exact name). Pin it here rather than
# $(notdir $(CURDIR)) so a checkout dir named anything else -- e.g. the GitHub repo dir
# "CaveWii" -- still builds CaveWii_v2.dol instead of CaveWii.dol (which broke CI's upload).
TARGET		:=	CaveWii_v2
BUILD		:=	build

# `make dev` (CW_DEV=1): fast-iteration variant — no LTO (the ~30-partition
# LTRANS link dominates rebuild time), its own object dir (LTO objects are
# GIMPLE, not sharable), and a separately named artifact + "-dev" build-ID
# suffix so a dev .dol can never be mistaken for the release CaveWii_v2.dol.
# The release path is the ONLY hardware-proof artifact; with CW_DEV unset
# every variable below expands byte-identically to the pre-dev Makefile, so
# the release md5/build-ID/TLS/staging gates are untouched.
ifeq ($(CW_DEV),1)
TARGET		:=	CaveWii_v2-dev
BUILD		:=	build-dev
CW_LTO_CFLAG	=
CW_LTO_LDFLAG	=
CW_DEV_SUFFIX	:=	-dev
else
CW_LTO_CFLAG	=	-flto=auto
CW_LTO_LDFLAG	=	-flto=$(LTO_JOBS)
CW_DEV_SUFFIX	:=
endif
# CW_ALLOW_STOCK_LIBOGC=1 (sidecar gate above): rename the artifact so a stock-libogc
# build (CI-artifact/PC-smoke-test only, not hardware-proof) can never collide with, or
# be mistaken for, the release/dev sidecar-linked .dol of the same nominal name.
ifeq ($(CW_STOCK_LIBOGC),1)
TARGET		:=	$(TARGET)-stock
endif
SOURCES		:=	source source/block source/entity source/graphics source/network source/game source/game/gui source/game/achievements source/game/stats source/platform source/item source/item/items source/worldgen source/cNBT source/lodepng source/parson
DATA		:=
TEXTURES	:=	textures
INCLUDES	:=	source third_party

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------

# Build stamp for the F3 overlay (see CMakeLists for the PC twin). Evaluated at
# make time; embeds into screen_ingame.o when that file recompiles -- the stamp
# identifies the build the OVERLAY came from; the md5-verify discipline for the
# .dol artifact stands. Single token (no spaces) to keep make->sh->gcc quoting
# trivial.
# The source lists below are wildcard-driven, so a dirty identity includes a
# deterministic digest of tracked changes and every non-ignored untracked file.
# Two different same-day dirty hardware candidates must never share an F3 ID.
CW_GIT_HASH	:=	$(shell sh tools/worktree-stamp.sh 2>/dev/null || echo nogit)
CW_BUILD_DATE	:=	$(shell date +%m.%d)

# FCAVEX_PROFILE_WII_LOW: the Broadway-sized budgets in profile.h -- 2 explosions/tick,
# 20 random ticks/chunk, and (the most behavior-visible knob) the
# scheduled-tick FIRE budget 1000->128, whose redstone-timing deviation window is
# documented at profile.h. client_display_tick.c separately keys its cosmetic sample
# count (1000->250) on the same define. The 2026-06-12 audit found this had NEVER been
# defined in any Wii build -- hardware silently ran the fast-core defaults profile.h
# documents as unsustainable. NOTE: BASELINE.md section 8.4's hardware numbers predate
# this flip (measured at 80 random ticks / budget-1000-via-config); section 8 carries
# the basis-change annotation. The hang report's "prof=" field (wii/watchdog.c) stamps
# which profile a .dol actually compiled.
# CW_PREFIX_MAP: strips the absolute build-root prefix from every path GCC embeds
# via __FILE__/__BASE_FILE__ (assert strings, m-lib's, diagnostics) and from macro
# expansions of those, replacing it with "." -- makes the .dol byte-reproducible
# across build directories (CLAUDE.md "The .dol md5 is PATH-SENSITIVE"). No -g in
# this release CFLAGS today (Wii release ships no debug info), so -fdebug-prefix-map
# is a no-op belt-and-suspenders now and only matters if -g is ever added later;
# -ffile-prefix-map alone already implies both -fmacro-prefix-map and
# -fdebug-prefix-map on this GCC (15.2.0), all three are listed for explicitness.
# CW_BUILD_ROOT is $(CURDIR) captured in PASS 1 (see above) -- NOT a $(CURDIR)
# reference here, which would wrongly re-evaluate to the pass-2 build/ subdir.
CW_PREFIX_MAP	=	-ffile-prefix-map=$(CW_BUILD_ROOT)=. -fmacro-prefix-map=$(CW_BUILD_ROOT)=. -fdebug-prefix-map=$(CW_BUILD_ROOT)=.
CFLAGS		=	-std=c11 -pedantic -Wextra -Wno-unused-parameter $(CW_LTO_CFLAG) -O3 -ffp-contract=off -Wall -DNDEBUG -DPLATFORM_WII -DFCAVEX_PROFILE_WII_LOW -DCW_BUILD_ID=\"$(CW_GIT_HASH)_$(CW_BUILD_DATE)$(CW_DEV_SUFFIX)\" $(EXTRA_DEFS) $(MACHDEP) $(INCLUDE) $(CW_PREFIX_MAP)
CXXFLAGS	=	$(CFLAGS)

# The objects are GIMPLE (CFLAGS -flto=auto), so LTO codegen happens at LINK
# time and LTRANS parallelism is decided by the LINK invocation -- without a
# -flto here every build pays a serial ~30-partition link ("lto-wrapper:
# warning: using serial compilation of N LTRANS jobs"). An EXPLICIT job count
# is required: base_rules' link recipe is not '+'-marked, so under -j the
# jobserver make advertises is unreachable and -flto=auto degrades to the same
# serial link. Same partitions either way: .dol proven md5-identical vs serial.
# --undefined=cw_layout_probe_pad (BREAKER-#2 LAYOUT GUARD, source/main.c):
# LTO drops an unreferenced `static` object even under `__attribute__((used))`
# once whole-program analysis sees no call site keeping it alive (found
# 2026-09-03: the guard pad was absent from both the release .elf's nm output
# and its link map). Forcing the linker to see cw_layout_probe_pad as an
# undefined symbol it must resolve is a second, independent anchor alongside
# the external linkage + real runtime store in main.c -- belt-and-suspenders
# against the same class of drop recurring. Applies to BOTH the release and
# dev variants (this LDFLAGS line is shared; only CW_LTO_LDFLAG differs
# between them) since the dev build, though LTO-off today, should not
# silently regress this guard if that ever changes.
LTO_JOBS	?=	$(shell nproc 2>/dev/null || echo 4)
# P04: gate native pathname resolution and reuse the selected FatFs world cwd.
# Keep public rmdir wrapped too: its SDK-local _rmdir_r branch bypasses --wrap.
CW_SAVE_PATH_WRAP = -Wl,--wrap=fatInitDefault,--wrap=_open_r,--wrap=_stat_r,--wrap=_unlink_r,--wrap=_rename_r,--wrap=_rmdir_r,--wrap=rmdir,--wrap=mkdir,--wrap=opendir,--wrap=chdir
LDFLAGS	=	$(MACHDEP) $(CW_LTO_LDFLAG) -Wl,-Map,$(notdir $@).map -Wl,--undefined=cw_layout_probe_pad $(CW_SAVE_PATH_WRAP)

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
#---------------------------------------------------------------------------------
# -laesnd: the S0 audio backend (AESND owns the DSP; never also link -lasnd —
# the two libs are mutually exclusive DSP owners).
LIBS	:=	-laesnd -lwiiuse -lfat -lbte -logc -lm -lz

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
					$(foreach dir,$(TEXTURES),$(CURDIR)/$(dir))


export DEPSDIR	:=	$(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# automatically build a list of object files for our project
#---------------------------------------------------------------------------------
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))
SCFFILES	:=	$(foreach dir,$(TEXTURES),$(notdir $(wildcard $(dir)/*.scf)))
TPLFILES	:=	$(SCFFILES:.scf=.tpl)

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
					$(addsuffix .o,$(TPLFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o)

#---------------------------------------------------------------------------------
# build a list of include paths
#---------------------------------------------------------------------------------
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD) \
					-I$(LIBOGC_INC)

#---------------------------------------------------------------------------------
# Reproducible-.dol build root (CLAUDE.md "The .dol md5 is PATH-SENSITIVE").
# Captured HERE (pass 1, before the recursive $(MAKE) -C $(BUILD) re-enters this
# file with CURDIR pointing at build/ instead of the repo root) and exported so
# CFLAGS -- expanded lazily, on demand, inside the pass-2 sub-make -- resolves it
# to this frozen absolute string rather than the wrong (pass-2) $(CURDIR).
export CW_BUILD_ROOT	:=	$(CURDIR)

#---------------------------------------------------------------------------------
# build a list of library paths
#---------------------------------------------------------------------------------
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					-L$(LIBOGC_LIB)

export OUTPUT	:=	$(CURDIR)/$(TARGET)
.PHONY: $(BUILD) clean dev

#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# Fast-iteration Wii build: re-enter this Makefile with CW_DEV=1 (see the
# variant block at the top). Produces CaveWii_v2-dev.dol from build-dev/.
dev:
	@$(MAKE) --no-print-directory CW_DEV=1 build-dev

#---------------------------------------------------------------------------------
# Removes BOTH variants regardless of which one this invocation selected.
clean:
	@echo clean ...
	@rm -fr build build-dev $(CURDIR)/CaveWii_v2.elf $(CURDIR)/CaveWii_v2.dol \
		$(CURDIR)/CaveWii_v2-dev.elf $(CURDIR)/CaveWii_v2-dev.dol
#---------------------------------------------------------------------------------
run:
	wiiload $(OUTPUT).dol

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
# Explicit .dol recipe (overrides wii_rules' %.dol pattern) so the freshly linked
# ELF is asserted TLS-free before elf2dol. devkitPPC compiles __thread /
# _Thread_local / C++ thread_local with the local-exec TLS model (r2-relative),
# but libogc's linker script defines no .tdata/.tbss output sections and r2
# actually holds _SDA2_BASE_: the link emits no PT_TLS, TLS data orphan-places at
# address 0 aliasing live .eh_frame_hdr, and the build corrupts silently instead
# of failing. First-party source has zero TLS (verified 2026-08-13); this gate
# catches a vendored dep or future addition sneaking it in. Checks section names
# AND TLS-typed symbols; advisory skip when readelf is absent, FATAL when TLS is
# found. Every build path (build-verify.sh, local_ci's devkitPPC Docker job,
# bare make) routes through this rule.
$(OUTPUT).dol: $(OUTPUT).elf
	@READELF='$(DEVKITPPC)/bin/powerpc-eabi-readelf'; \
	if [ ! -x "$$READELF" ]; then \
		echo "TLS check: SKIPPED -- $$READELF not found (advisory only)"; \
	else \
		"$$READELF" -h "$<" >/dev/null || { echo "TLS check: FAIL -- readelf cannot read $<"; exit 1; }; \
		TLS=$$( { "$$READELF" -SW "$<" | grep -E '^ *\[ *[0-9]+\] +\.t(data|bss)([. ]|$$)'; \
		          "$$READELF" -sW "$<" | awk '$$4 == "TLS"'; } ); \
		if [ -n "$$TLS" ]; then \
			echo "TLS check: FAIL -- thread-local storage linked into $(notdir $<):"; \
			printf '%s\n' "$$TLS"; \
			echo "libogc emits no PT_TLS and r2 is _SDA2_BASE_; this data would sit at 0x0 over .eh_frame_hdr."; \
			echo "Remove the __thread/_Thread_local/thread_local qualifier from the offending code."; \
			exit 1; \
		fi; \
		echo "TLS check: OK -- no .tdata/.tbss sections, no TLS-typed symbols"; \
		if "$$READELF" -sW "$<" | grep -q cavewii_test_engine_marker; then \
			echo "test-marker gate: FAIL -- test-instrumented engine objects linked into $(notdir $<)"; \
			echo "(cavewii_test_engine_marker present; see tests/support/test_engine_marker.c and docs/build_architecture.md step 4)"; \
			exit 1; \
		fi; \
		echo "test-marker gate: OK -- no test-instrumented objects"; \
	fi
# bootdeath-0917 / K12 SD DMA buffer gate (Astra ruling 2 D1, 2026-09-17;
# tmp/jobs/bootdeath-0917/EXPERIMENT-3.md; docs/wii_boot_death_gate.md).
# Refuses a link in which libogc's __sd0_functions -- the 64-byte buffer handed
# RAW to the SD host controller as a DMA target during SD init -- lies across a
# 4 KB line. A hardware double dissociation isolated that to the cause of the
# pre-FAT zero-trace boot death; the __bss_end page-offset window this rule
# replaces was only a proxy for it. Deliberately NOT nested inside the readelf
# conditional above: that TLS check is advisory when its tool is missing, this
# one FAILS CLOSED (the script exits 2 when nm/the .elf/the symbol is missing,
# ambiguous or unparsable), because an unverified layout is the exact case it
# exists to stop. nm is resolved from the MAKE variable $(DEVKITPPC) and passed
# explicitly, the same way the READELF above does it -- the script can also fall
# back to the DEVKITPPC environment variable, but a recipe's environment is not
# guaranteed to carry a variable that reached make on its command line.
# $(CW_BUILD_ROOT) is the pass-1 repo root frozen for this pass-2 sub-make (see
# its export above); plain $(CURDIR) here would be build/ or build-dev/.
#
# On refusal, any PRE-EXISTING .dol at the target path is a STALE artifact from an
# earlier build (elf2dol below never runs on this pass, so make would otherwise
# leave yesterday's .dol sitting at the exact path staging copies from). Rename it
# out of the way so a refused build cannot leave anything that looks stageable.
# $(OUTPUT) already carries the -dev/-stock variant suffix, so the release, dev,
# and stock paths are all covered by this one rule.
	@sh $(CW_BUILD_ROOT)/tools/check_sd0_dma_buffer.sh "$<" '$(DEVKITPPC)/bin/powerpc-eabi-nm' || { \
		RC=$$?; \
		if [ -f "$(OUTPUT).dol" ]; then \
			mv -f "$(OUTPUT).dol" "$(OUTPUT)-REFUSED-sd0dma.dol" && \
			echo "[bootdeath-0917] stale $(notdir $(OUTPUT)).dol renamed to $(notdir $(OUTPUT))-REFUSED-sd0dma.dol -- do NOT stage it"; \
		fi; \
		echo "[bootdeath-0917] build FAILED: no stageable $(notdir $(OUTPUT)).dol was produced (gate rc=$$RC)"; \
		exit 1; \
	}
	$(SILENTMSG) output ... $(notdir $@)
	$(SILENTCMD)elf2dol $< $@
$(OUTPUT).elf: $(OFILES)

#---------------------------------------------------------------------------------
# This rule links in binary data with the .bin extension
#---------------------------------------------------------------------------------
%.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	$(bin2o)

#---------------------------------------------------------------------------------
%.tpl.o	:	%.tpl
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)


-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
