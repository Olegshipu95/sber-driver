obj-m += sber_driver.o

all:
	@echo "Targets: clean, build, install, dmesg, test"

build:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean

install: build
	sudo insmod sber_driver.ko

dmesg: install
	sudo dmesg | tail


remove: clean
	sudo rmmod sber_driver