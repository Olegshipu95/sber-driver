# Makefile for sber_driver module

obj-m += sber_driver.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Compile and insert the module into the kernel
insmod: all
	sudo insmod sber_driver.ko || (echo "Module already loaded"; exit 1)
	dmesg | tail

# Remove the module from the kernel
rmmod:
	sudo rmmod sber_driver || (echo "Module not loaded"; exit 1)
	dmesg | tail

# Reload the module
reload: rmmod insmod

# Compile the module
all:
	make -C $(KDIR) M=$(PWD) modules

# Clean the build files
clean:
	make -C $(KDIR) M=$(PWD) clean

# View the last kernel messages
dmesg:
	dmesg | tail
