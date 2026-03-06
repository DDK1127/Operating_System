# Makefile for kfetch kernel module

# Replace with your student ID
STUDENT_ID ?= 314553040

# Kernel source directory
KDIR ?= $(PWD)/linux-v6.8-devkit

# Architecture settings
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-

# Module name
obj-m += kfetch_mod_$(STUDENT_ID).o

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) clean
	rm -f *.ko *.o *.mod.c *.mod.o *.symvers *.order

.PHONY: all clean
