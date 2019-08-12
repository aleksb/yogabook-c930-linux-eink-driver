KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD

test: default
	-rmmod wacom
	-rmmod usbhid
	-rmmod eink
	insmod eink.ko
	-dmesg
	-sleep 1
	rmmod eink
	-dmesg

imagescript: default
	-rmmod wacom
	-rmmod usbhid
	-rmmod eink
	-rm /dev/eink*
	# clear the kernel message buffer for easier debugging
	-dmesg -C
	insmod eink.ko
	-sleep 0.1
	# check whether device file appeared
	ls /dev/eink*
	bash -c 'echo "draw" >> /dev/eink*'
	bash -c '../image.py >> /dev/eink*'

mandelbrot: run
	../mandelbrot.py

run: default
	-rmmod wacom
	-rmmod usbhid
	-rmmod eink
	insmod eink.ko
	-modprobe usbhid
	-modprobe wacom

install:
	-rmmod wacom
	-rmmod usbhid
	-rmmod eink
	$(MAKE) -C $(KDIR) modules_install M=$$PWD
	-depmod
	-modprobe eink
	-modprobe usbhid
	-modprobe wacom

clean:
	-rm *.o *.o.d *o.ur-safe *.ko *.mod *.mod.c modules.order
