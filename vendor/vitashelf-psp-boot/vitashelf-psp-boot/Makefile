# VitaShelf Launcher boot plugin for Adrenaline, derived from Pocket Shelf's
# MIT-licensed psp-boot source (see README.md). Built with the pspdev toolchain;
# scripts/build-psp-plugin.sh copies the result to
# assets/bin/psp/vitashelf.prx for the VPK.
TARGET = vitashelf
OBJS = main.o

BUILD_PRX = 1
PRX_EXPORTS = exports.exp
# A kernel module: it sets the UMD file and boots through SystemCtrlForKernel.
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

CFLAGS = -Os -G0 -Wall -Wextra -fno-pic -fno-builtin-printf
LDFLAGS = -nostartfiles
LIBS = -lpspsystemctrl_kernel -lgcc

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
