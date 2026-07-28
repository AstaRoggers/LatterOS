DISK_IMAGE := latteros-test-disk.img
USB_DISK_IMAGE := latteros-usb-complete.img
USB_DISK_BUILDER := make-usb-fat32-unicode.py
SATA_DISK_IMAGE := latteros-sata-ahci.img
SATA_DISK_BUILDER := make-sata-ahci.py
NVME_DISK_IMAGE := latteros-nvme-gpt.img
NVME_DISK_BUILDER := make-nvme-gpt.py
INSTALL_TARGET_IMAGE := latteros-install-target.img
INSTALL_TARGET_BUILDER := make-install-target.py
INSTALL_TARGET_VERIFIER := verify-installed-target.py
PACKAGE_BUILDER := make-lpkg.py
DEMO_PACKAGE_MANIFEST := packages/hello/manifest.ini
DEMO_PACKAGE_SOURCE := packages/hello
DEMO_PACKAGE := dist/packages/hello-1.0.0.lpkg
RELEASE_BUILDER := make-release.py
RELEASE_VERSION_FILE := release-version.txt
RELEASE_VERSION := $(shell cat $(RELEASE_VERSION_FILE))
RELEASE_NAME := LatterOS-$(RELEASE_VERSION)-x86_64
RELEASE_DIRECTORY := dist/$(RELEASE_NAME)
RECOVERY_ISO := $(RELEASE_DIRECTORY)/$(RELEASE_NAME)-recovery.iso
IMAGE_NAME := template-x86_64.iso
OVMF_DIRECTORY := edk2-ovmf-bins
OVMF_CODE := $(OVMF_DIRECTORY)/ovmf-code-x86_64.fd
PYTHON ?= python

.PHONY: run rebuild usb-image sata-image nvme-image install-target \
	reset-install-target verify-installed run-installed package-demo \
	verify-package release release-clean run-recovery

$(DISK_IMAGE):
	dd if=/dev/zero of=$(DISK_IMAGE) bs=1M count=16

$(DEMO_PACKAGE): $(PACKAGE_BUILDER) $(DEMO_PACKAGE_MANIFEST) $(DEMO_PACKAGE_SOURCE)/README.txt
	mkdir -p $(dir $(DEMO_PACKAGE))
	$(PYTHON) $(PACKAGE_BUILDER) $(DEMO_PACKAGE_MANIFEST) $(DEMO_PACKAGE_SOURCE) $(DEMO_PACKAGE)

$(USB_DISK_IMAGE): $(USB_DISK_BUILDER) $(DEMO_PACKAGE)
	$(PYTHON) $(USB_DISK_BUILDER) $(USB_DISK_IMAGE) $(DEMO_PACKAGE)

$(SATA_DISK_IMAGE): $(SATA_DISK_BUILDER)
	$(PYTHON) $(SATA_DISK_BUILDER) $(SATA_DISK_IMAGE)

$(NVME_DISK_IMAGE): $(NVME_DISK_BUILDER)
	$(PYTHON) $(NVME_DISK_BUILDER) $(NVME_DISK_IMAGE)

$(INSTALL_TARGET_IMAGE): $(INSTALL_TARGET_BUILDER)
	$(PYTHON) $(INSTALL_TARGET_BUILDER) $(INSTALL_TARGET_IMAGE)

$(OVMF_CODE):
	$(MAKE) -f GNUmakefile $(OVMF_DIRECTORY)

package-demo: $(DEMO_PACKAGE)

verify-package: $(DEMO_PACKAGE)
	$(PYTHON) $(PACKAGE_BUILDER) --verify $(DEMO_PACKAGE)

usb-image: $(DEMO_PACKAGE)
	$(PYTHON) $(USB_DISK_BUILDER) $(USB_DISK_IMAGE) $(DEMO_PACKAGE)

sata-image:
	$(PYTHON) $(SATA_DISK_BUILDER) $(SATA_DISK_IMAGE)

nvme-image:
	$(PYTHON) $(NVME_DISK_BUILDER) $(NVME_DISK_IMAGE)

install-target:
	$(PYTHON) $(INSTALL_TARGET_BUILDER) $(INSTALL_TARGET_IMAGE)

reset-install-target:
	$(PYTHON) $(INSTALL_TARGET_BUILDER) --reset $(INSTALL_TARGET_IMAGE)

verify-installed: $(INSTALL_TARGET_IMAGE) $(INSTALL_TARGET_VERIFIER)
	$(PYTHON) $(INSTALL_TARGET_VERIFIER) $(INSTALL_TARGET_IMAGE)

run: $(DISK_IMAGE) $(USB_DISK_IMAGE) $(SATA_DISK_IMAGE) $(NVME_DISK_IMAGE) $(INSTALL_TARGET_IMAGE)
	$(MAKE) -f GNUmakefile TOOLCHAIN=llvm CFLAGS="-g -O2 -pipe -fno-omit-frame-pointer"
	rm -f latteros-serial.log
	qemu-system-x86_64 \
		-M pc,pcspk-audiodev=audio0 \
		-cpu qemu64,+x2apic \
		-smp cpus=4,sockets=1,cores=4,threads=1,maxcpus=4 \
		-m 2G \
		-audiodev dsound,id=audio0 \
		-vga none \
		-device virtio-vga,id=latteros-gpu,xres=1280,yres=800 \
		-display gtk,zoom-to-fit=off,show-menubar=off \
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
		-drive if=none,id=installtarget,file=$(INSTALL_TARGET_IMAGE),format=raw \
		-device ide-hd,drive=installtarget,bus=ahci.1,unit=0 \
		-drive if=none,id=nvmedisk,file=$(NVME_DISK_IMAGE),format=raw \
		-device nvme,drive=nvmedisk,serial=LATTEROSNVME \
		-nic none \
		-netdev user,id=net0 \
		-device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
		-cdrom $(IMAGE_NAME) \
		-boot d \
		-drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk

run-installed: $(INSTALL_TARGET_IMAGE) $(OVMF_CODE)
	rm -f latteros-installed-serial.log
	qemu-system-x86_64 \
		-M pc,pcspk-audiodev=audio0 \
		-cpu qemu64,+x2apic \
		-smp cpus=4,sockets=1,cores=4,threads=1,maxcpus=4 \
		-m 2G \
		-drive if=pflash,unit=0,format=raw,file=$(OVMF_CODE),readonly=on \
		-audiodev dsound,id=audio0 \
		-vga none \
		-device virtio-vga,id=latteros-gpu,xres=1280,yres=800 \
		-display gtk,zoom-to-fit=off,show-menubar=off \
		-serial file:latteros-installed-serial.log \
		-monitor stdio \
		-usb \
		-device usb-kbd,bus=usb-bus.0 \
		-device usb-mouse,bus=usb-bus.0 \
		-device ich9-ahci,id=installed-ahci \
		-drive if=none,id=installed,file=$(INSTALL_TARGET_IMAGE),format=raw \
		-device ide-hd,drive=installed,bus=installed-ahci.0,unit=0 \
		-nic none \
		-boot c

release: $(DEMO_PACKAGE) $(INSTALL_TARGET_IMAGE) $(INSTALL_TARGET_VERIFIER) \
	$(RELEASE_BUILDER) $(RELEASE_VERSION_FILE) limine-recovery.conf
	$(MAKE) -f GNUmakefile TOOLCHAIN=llvm CFLAGS="-g -O2 -pipe -fno-omit-frame-pointer"
	$(PYTHON) $(INSTALL_TARGET_VERIFIER) $(INSTALL_TARGET_IMAGE)
	$(PYTHON) $(PACKAGE_BUILDER) --verify $(DEMO_PACKAGE)
	$(PYTHON) $(RELEASE_BUILDER)

release-clean:
	rm -rf $(RELEASE_DIRECTORY)

run-recovery: release
	rm -f latteros-recovery-serial.log
	qemu-system-x86_64 \
		-M pc,pcspk-audiodev=audio0 \
		-cpu qemu64,+x2apic \
		-smp cpus=4,sockets=1,cores=4,threads=1,maxcpus=4 \
		-m 2G \
		-audiodev dsound,id=audio0 \
		-vga none \
		-device virtio-vga,id=latteros-gpu,xres=1280,yres=800 \
		-display gtk,zoom-to-fit=off,show-menubar=off \
		-serial file:latteros-recovery-serial.log \
		-monitor stdio \
		-usb \
		-device usb-kbd,bus=usb-bus.0 \
		-device usb-mouse,bus=usb-bus.0 \
		-device ich9-ahci,id=recovery-ahci \
		-drive if=none,id=recoverytarget,file=$(INSTALL_TARGET_IMAGE),format=raw \
		-device ide-hd,drive=recoverytarget,bus=recovery-ahci.0,unit=0 \
		-cdrom $(RECOVERY_ISO) \
		-boot d \
		-nic none

rebuild: $(DISK_IMAGE) $(SATA_DISK_IMAGE) $(NVME_DISK_IMAGE)
	$(MAKE) -f GNUmakefile clean
	rm -f $(USB_DISK_IMAGE)
	$(MAKE) -f storage-run.mk run
