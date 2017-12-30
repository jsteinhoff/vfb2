KVERSION	:= `uname -r`
KSRC            := /lib/modules/$(KVERSION)/build
INSTDIR		:= /lib/modules/$(KVERSION)/kernel/drivers/video

obj-m		:= vfb2.o vfb2_user.o

all:
	$(MAKE) -C $(KSRC) M=`pwd` CPATH=`pwd` modules

.PHONY: clean

clean:
	$(MAKE) -C $(KSRC) M=`pwd` clean

install: all
	install -m 644 vfb2.ko $(INSTDIR)/vfb2.ko
	install -m 644 vfb2_user.ko $(INSTDIR)/vfb2_user.ko
