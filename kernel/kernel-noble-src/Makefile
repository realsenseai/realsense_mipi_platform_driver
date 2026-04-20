# SPDX-FileCopyrightText: Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause

KERNEL_SRC_DIR ?= kernel-noble-src
KERNEL_DEF_CONFIG ?= defconfig

MAKEFILE_DIR := $(abspath $(shell dirname $(lastword $(MAKEFILE_LIST))))
kernel_source_dir := $(MAKEFILE_DIR)/$(KERNEL_SRC_DIR)

ifdef KERNEL_OUTPUT
O_OPT := O=$(KERNEL_OUTPUT)
$(mkdir -p $(KERNEL_OUTPUT))
endif

NPROC ?= $(shell nproc)

# LOCALVERSION : -tegra or -rt-tegra
LOCALVERSION = $(shell grep -q "CONFIG_PREEMPT_RT=y" \
    ${kernel_source_dir}/arch/arm64/configs/${KERNEL_DEF_CONFIG} && echo "-rt-tegra" || echo "-tegra")
export LOCALVERSION

.PHONY : kernel install clean help

kernel:
	@echo   "================================================================================"
	@echo   "Building $(KERNEL_SRC_DIR) sources"
	@echo   "================================================================================"
	$(MAKE) -C $(kernel_source_dir) $(O_OPT) \
		$(KERNEL_DEF_CONFIG)

	$(MAKE) -j $(NPROC) \
		-C $(kernel_source_dir) $(O_OPT) \
		--output-sync=target Image dtbs modules

	@echo   "================================================================================"
	@echo   "Kernel sources compiled successfully."
	@echo   "================================================================================"

install:
	@echo   "================================================================================"
	@echo   "Installing $(KERNEL_SRC_DIR) sources"
	@echo   "================================================================================"
	$(MAKE) -C $(kernel_source_dir) $(O_OPT) \
		install
	$(MAKE) \
		-C $(kernel_source_dir) $(O_OPT) \
		modules_install
	@echo   "================================================================================"
	@echo   "Kernel and in-tree modules installed successfully."
	@echo   "================================================================================"


clean:
	@echo   "================================================================================"
	@echo   "Cleaning $(KERNEL_SRC_DIR) sources"
	@echo   "================================================================================"
	$(MAKE) \
		-C $(kernel_source_dir) $(O_OPT) \
		mrproper
	@echo   "================================================================================"
	@echo   "Kernel and in-tree modules installed successfully."
	@echo   "================================================================================"

# make help
help:
	@echo   "================================================================================"
	@echo   "Usage:"
	@echo   "   make or make kernel   # to build kernel"
	@echo   "   make install          # to install kernel image and in-tree modules"
	@echo   "   make clean            # to make clean kernel source"
	@echo   "================================================================================"
