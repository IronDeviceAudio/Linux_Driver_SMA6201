obj-m := sma6201.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

install:
	sudo cp sma6201.ko /lib/modules/$(shell uname -r)
	sudo depmod -a

