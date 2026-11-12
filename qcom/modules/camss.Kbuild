# Out-of-tree build of the Qualcomm CAMSS driver (RSDEV-15171).
#
# CAMSS is tristate and self-contained - every object is local to its own
# directory and it includes only public kernel headers - so our fork builds
# out-of-tree and ships through DKMS. DKMS installs to updates/dkms/, which
# depmod prefers over kernel/, so this replaces the in-tree qcom-camss.ko
# without touching the kernel package.
#
# The object list mirrors the kernel's own camss Makefile, minus ope/ (the
# offline processing engine, which we do not use). apply_patches_qcom.sh warns
# if the kernel's list has drifted from this one.

obj-m += qcom-camss.o

qcom-camss-objs := \
		camss.o \
		camss-csid.o \
		camss-csid-4-1.o \
		camss-csid-4-7.o \
		camss-csid-gen2.o \
		camss-csid-gen3.o \
		camss-csiphy-2ph-1-0.o \
		camss-csiphy-3ph-1-0.o \
		camss-csiphy.o \
		camss-ispif.o \
		camss-vfe-4-1.o \
		camss-vfe-4-7.o \
		camss-vfe-4-8.o \
		camss-vfe-17x.o \
		camss-vfe-480.o \
		camss-vfe-690.o \
		camss-vfe-gen1.o \
		camss-vfe.o \
		camss-video.o \
		camss-format.o \
		camss-tpg.o \
		camss-tpg-gen1.o

# The RealSense private media-bus codes its format rows select on. Same header
# d4xx force-includes, so the two agree by construction.
ccflags-y += -include $(src)/compat-include/media/rs-formats.h
