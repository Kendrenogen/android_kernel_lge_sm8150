# SPDX-License-Identifier: GPL-2.0
#
# Building a vDSO image for AArch64.
#
# Author: Will Deacon <will.deacon@arm.com>
# Heavily based on the vDSO Makefiles for other archs.
#

obj-vdso-s := note.o sigreturn.o
obj-vdso-c := vgettimeofday.o

# Build rules
targets := $(obj-vdso-s) $(obj-vdso-c) vdso.so vdso.so.dbg
obj-vdso-s := $(addprefix $(obj)/, $(obj-vdso-s))
obj-vdso-c := $(addprefix $(obj)/, $(obj-vdso-c))
obj-vdso   := $(obj-vdso-c) $(obj-vdso-s)

ccflags-y := -fno-stack-protector -ffixed-x18
ccflags-y += -DDISABLE_BRANCH_PROFILING
ccflags-y += $(DISABLE_LTO) $(DISABLE_SCS)
ldflags-y := -shared -soname=linux-vdso.so.1  --hash-style=sysv \
		--build-id=sha1 $(call ld-option,-n) -T

# Force -O2 to avoid libgcc dependencies
CFLAGS_REMOVE_vgettimeofday.o = -pg -Os $(GCC_PLUGINS_CFLAGS)
ifeq ($(cc-name),clang)
CFLAGS_vgettimeofday.o = -O3
else
CFLAGS_vgettimeofday.o = -O2 -mcmodel=tiny
endif
CFLAGS_vgettimeofday.o += -fPIC

# Disable gcov profiling for VDSO code
GCOV_PROFILE := n
KASAN_SANITIZE := n
UBSAN_SANITIZE := n
KCOV_INSTRUMENT := n
obj-y += vdso.o
targets += vdso.lds
CPPFLAGS_vdso.lds += -P -C -U$(ARCH)

# Force dependency (incbin is bad)
$(obj)/vdso.o : $(obj)/vdso.so

# Link rule for the .so file, .lds has to be first
$(obj)/vdso.so.dbg: $(obj)/vdso.lds $(obj-vdso) FORCE
	$(call if_changed,ld)

# Strip rule for the .so file
$(obj)/%.so: OBJCOPYFLAGS := -S
$(obj)/%.so: $(obj)/%.so.dbg FORCE
	$(call if_changed,objcopy)

# Generate VDSO offsets using helper script
gen-vdsosym := $(srctree)/$(src)/gen_vdso_offsets.sh
quiet_cmd_vdsosym = VDSOSYM $@
      cmd_vdsosym = $(NM) $< | $(gen-vdsosym) | LC_ALL=C sort > $@

include/generated/vdso-offsets.h: $(obj)/vdso.so.dbg FORCE
	$(call if_changed,vdsosym)

# Assembly rules for the .S files
$(obj-vdso-s): %.o: %.S FORCE
	$(call if_changed_dep,vdsoas)

$(obj-vdso-c): %.o: %.c FORCE
	$(call if_changed_dep,vdsocc)

# Actual build commands
quiet_cmd_vdsold = VDSOL   $@
      cmd_vdsold = $(CC) $(c_flags) -Wl,-n -Wl,-T $(real-prereqs) -o $@
quiet_cmd_vdsocc = VDSOC   $@
      cmd_vdsocc = ${CC} $(c_flags) -c -o $@ $<
quiet_cmd_vdsoas = VDSOA   $@
      cmd_vdsoas = $(CC) $(a_flags) -c -o $@ $<

# Install commands for the unstripped file
quiet_cmd_vdso_install = INSTALL $@
      cmd_vdso_install = cp $(obj)/$@.dbg $(MODLIB)/vdso/$@

vdso.so: $(obj)/vdso.so.dbg
	@mkdir -p $(MODLIB)/vdso
	$(call cmd,vdso_install)

vdso_install: vdso.so
