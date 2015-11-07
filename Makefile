TARGET = parrot_driver

ifneq ($(KERNELRELEASE),)
# call from kernel build system

obj-m	:= $(TARGET).o

else

KERNELDIR ?= /usr/src/linux
PWD       := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) SUBDIRS=$(PWD) modules

endif

clean:
	rm -rf *.o *.ko *~ core .depend *.mod.c .*.cmd .tmp_versions .*.o.d

depend .depend dep:
	$(CC) $(CFLAGS) -M *.c > .depend

ins: default rem
	insmod $(TARGET).ko debug=1

rem:
	@if [ -n "`lsmod | grep -s $(TARGET)`" ]; then rmmod $(TARGET); echo "rmmod $(TARGET)"; fi

ifeq (.depend,$(wildcard .depend))
include .depend
endif
