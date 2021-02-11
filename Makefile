obj-m += exosensepi.o
 
exosensepi-objs := module.o
exosensepi-objs += sensirion/common/sensirion_common.o
exosensepi-objs += sensirion/sht4x/sht4x.o
exosensepi-objs += sensirion/sht4x/sht_git_version.o
exosensepi-objs += sensirion/sgp40/sgp40.o
exosensepi-objs += sensirion/sgp40/sgp_git_version.o
exosensepi-objs += sensirion/sgp40_voc_index/sensirion_voc_algorithm.o

ccflags-y := -std=gnu99 -Wno-declaration-after-statement

all:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) clean

install:
	sudo install -m 644 -c exosensepi.ko /lib/modules/$(shell uname -r)
	sudo depmod
