obj-m := lenovo-sl-laptop.o
KVERSION = $(shell uname -r)

all:
	$(MAKE) -C /lib/modules/$(KVERSION)/build M=$(PWD) modules

clean:
	$(MAKE) -C /lib/modules/$(KVERSION)/build M=$(PWD) clean

module:
	$(MAKE) -C /usr/src/linux M=$(PWD) modules
