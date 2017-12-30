KVERSION	:= `uname -r`
INSTDIR		:= /lib/modules/$(KVERSION)/kernel/drivers/video

obj-m		:= vfb2.o vfb2_user.o

all:
	$(MAKE) modules -C /lib/modules/$(KVERSION)/build SUBDIRS=`pwd`

.PHONY: clean

clean:
	$(RM) -fr *.o *.ko *.mod.c .*.cmd .tmp_versions *~

install:
	install -m 644 vfb2.ko $(INSTDIR)
	install -m 644 vfb2_user.ko $(INSTDIR)
