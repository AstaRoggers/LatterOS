DISK_IMAGE := latteros-test-disk.img
USB_DISK_IMAGE := latteros-usb-rw.img
USB_DISK_BUILDER := make-usb-rw.py
IMAGE_NAME := template-x86_64.iso
PYTHON ?= python

.PHONY: run rebuild usb-image

$(DISK_IMAGE):
	dd if=/dev/zero of=$(DISK_IMAGE) bs=1M count=16

$(USB_DISK_IMAGE): $(USB_DISK_BUILDER)
	$(PYTHON) $(USB_DISK_BUILDER) $(USB_DISK_IMAGE)

usb-image:
	$(PYTHON) $(USB_DISK_BUILDER) $(USB_DISK_IMAGE)

run: $(DISK_IMAGE) $(USB_DISK_IMAGE)
	$(MAKE) -f GNUmakefile TOOLCHAIN=llvm CFLAGS="-g -O2 -pipe -fno-omit-frame-pointer"
	rm -f latteros-serial.log
	qemu-system-x86_64 \
		-M pc,pcspk-audiodev=audio0 \
		-cpu qemu64,+x2apic \
		-smp cpus=4,sockets=1,cores=4,threads=1,maxcpus=4 \
		-m 2G \
		-audiodev dsound,id=audio0 \
		-serial file:latteros-serial.log \
		-usb \
		-device usb-kbd,id=latteros-kbd \
		-device usb-mouse,id=latteros-mouse \
		-drive if=none,id=usbdisk,file=$(USB_DISK_IMAGE),format=raw \
		-device usb-storage,drive=usbdisk,removable=true \
		-nic none \
		-netdev user,id=net0 \
		-device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
		-cdrom $(IMAGE_NAME) \
		-boot d \
		-drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk

rebuild: $(DISK_IMAGE) $(USB_DISK_IMAGE)
	$(MAKE) -f GNUmakefile clean
	$(MAKE) -f storage-run.mk run
