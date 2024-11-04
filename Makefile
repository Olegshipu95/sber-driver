obj-m += sber_driver.o

all:
	@echo "Targets: clean, build, install, test"

build:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

install: build
	sudo insmod sber_driver.ko

test: install
	dmesg | tail

remove: clean
	sudo rmmod lab2