DISK_IMAGE := latteros-test-disk.img
USB_DISK_IMAGE := latteros-usb-complete.img
USB_DISK_BUILDER := make-usb-fat32-unicode.py
SATA_DISK_IMAGE := latteros-sata-ahci.img
SATA_DISK_BUILDER := make-sata-ahci.py
NVME_DISK_IMAGE := latteros-nvme-gpt.img
NVME_DISK_BUILDER := make-nvme-gpt.py
IMAGE_NAME := template-x86_64.iso
PYTHON ?= python

.PHONY: run rebuild usb-image sata-image nvme-image

$(DISK_IMAGE):
	dd if=/dev/zero of=$(DISK_IMAGE) bs=1M count=16

$(USB_DISK_IMAGE): $(USB_DISK_BUILDER)
	$(PYTHON) $(USB_DISK_BUILDER) $(USB_DISK_IMAGE)

$(SATA_DISK_IMAGE): $(SATA_DISK_BUILDER)
	$(PYTHON) $(SATA_DISK_BUILDER) $(SATA_DISK_IMAGE)

$(NVME_DISK_IMAGE): $(NVME_DISK_BUILDER)
	$(PYTHON) $(NVME_DISK_BUILDER) $(NVME_DISK_IMAGE)

usb-image:
	$(PYTHON) $(USB_DISK_BUILDER) $(USB_DISK_IMAGE)

sata-image:
	$(PYTHON) $(SATA_DISK_BUILDER) $(SATA_DISK_IMAGE)

nvme-image:
	$(PYTHON) $(NVME_DISK_BUILDER) $(NVME_DISK_IMAGE)

run: $(DISK_IMAGE) $(USB_DISK_IMAGE) $(SATA_DISK_IMAGE) $(NVME_DISK_IMAGE)
	$(MAKE) -f GNUmakefile TOOLCHAIN=llvm CFLAGS="-g -O2 -pipe -fno-omit-frame-pointer"
	rm -f latteros-serial.log
	qemu-system-x86_64 \
		-M pc,pcspk-audiodev=audio0 \
		-cpu qemu64,+x2apic \
		-smp cpus=4,sockets=1,cores=4,threads=1,maxcpus=4 \
		-m 2G \
		-audiodev dsound,id=audio0 \
		-serial file:latteros-serial.log \
		-monitor stdio \
		-usb \
		-device usb-hub,id=latteros-usb-hub,bus=usb-bus.0,port=1 \
		-device usb-kbd,id=latteros-kbd,bus=usb-bus.0,port=1.1 \
		-device usb-mouse,id=latteros-mouse,bus=usb-bus.0,port=1.2 \
		-drive if=none,id=usbdisk,file=$(USB_DISK_IMAGE),format=raw \
		-device usb-storage,id=latteros-usb-storage,drive=usbdisk,bus=usb-bus.0,port=1.3,removable=on \
		-device ich9-ahci,id=ahci \
		-drive if=none,id=satadisk,file=$(SATA_DISK_IMAGE),format=raw \
		-device ide-hd,drive=satadisk,bus=ahci.0,unit=0 \
		-drive if=none,id=nvmedisk,file=$(NVME_DISK_IMAGE),format=raw \
		-device nvme,drive=nvmedisk,serial=LATTEROSNVME \
		-nic none \
		-netdev user,id=net0 \
		-device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
		-cdrom $(IMAGE_NAME) \
		-boot d \
		-drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk

rebuild: $(DISK_IMAGE) $(SATA_DISK_IMAGE) $(NVME_DISK_IMAGE)
	$(MAKE) -f GNUmakefile clean
	rm -f $(USB_DISK_IMAGE)
	$(MAKE) -f storage-run.mk run
