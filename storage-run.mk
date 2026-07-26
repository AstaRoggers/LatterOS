DISK_IMAGE := latteros-test-disk.img
IMAGE_NAME := template-x86_64.iso

.PHONY: run rebuild

$(DISK_IMAGE):
	dd if=/dev/zero of=$(DISK_IMAGE) bs=1M count=16

run: $(DISK_IMAGE)
	$(MAKE) -f GNUmakefile TOOLCHAIN=llvm
	qemu-system-x86_64 \
		-M pc,pcspk-audiodev=audio0 \
		-cpu qemu64,+x2apic \
		-m 2G \
		-audiodev dsound,id=audio0 \
		-usb \
		-device usb-tablet \
		-netdev user,id=net0 \
		-device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
		-cdrom $(IMAGE_NAME) \
		-boot d \
		-drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk

rebuild: $(DISK_IMAGE)
	$(MAKE) -f GNUmakefile clean
	$(MAKE) -f storage-run.mk run