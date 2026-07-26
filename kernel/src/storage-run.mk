DISK_IMAGE := latteros-test-disk.img
IMAGE_NAME := template-x86_64.iso

.PHONY: run rebuild

$(DISK_IMAGE):
	dd if=/dev/zero of=$(DISK_IMAGE) bs=1M count=16

run: $(DISK_IMAGE)
	$(MAKE) -f GNUmakefile TOOLCHAIN=llvm
	qemu-system-x86_64 \
		-M pc \
		-cpu qemu64,+x2apic \
		-m 2G \
		-cdrom $(IMAGE_NAME) \
		-boot d \
		-drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk

rebuild: $(DISK_IMAGE)
	$(MAKE) -f GNUmakefile clean
	$(MAKE) -f storage-run.mk run
