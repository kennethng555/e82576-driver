obj-m += e82576.o

e82576-objs := e82576_main.o e82576_phy.o

KDIR := /lib/modules/6.12.101+deb13-amd64/build

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean