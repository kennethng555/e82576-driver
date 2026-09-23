obj-m += e82576.o

e82576-objs := 	e82576_main.o \
								e82576_phy.o \
								e82576_hw.o \
								e82576_irq.o \
								e82576_nvm.o \
								e82576_ring.o \
								e82576_rx.o \
								e82576_tx.o \
								e82576_napi.o

KDIR := /lib/modules/6.12.101+deb13-amd64/build

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean