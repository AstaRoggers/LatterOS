AUTOTEST_DISK := latteros-autotest-disk.img
AUTOTEST_LOG := latteros-autotest.log
IMAGE_NAME := template-x86_64.iso

.PHONY: test clean-test

test:
	rm -f $(AUTOTEST_DISK) $(AUTOTEST_LOG)
	dd if=/dev/zero of=$(AUTOTEST_DISK) bs=1M count=16
	$(MAKE) -f GNUmakefile clean
	$(MAKE) -f GNUmakefile \
		TOOLCHAIN=llvm \
		CFLAGS="-g -O2 -pipe -fno-omit-frame-pointer" \
		CPPFLAGS="-DLATTEROS_AUTOTEST=1"
	@set +e; \
		timeout 120s qemu-system-x86_64 \
			-M pc \
			-cpu qemu64,+x2apic \
			-m 2G \
			-display none \
			-monitor none \
			-serial file:$(AUTOTEST_LOG) \
			-usb \
			-device usb-tablet \
			-nic none \
			-netdev user,id=net0 \
			-device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
			-cdrom $(IMAGE_NAME) \
			-boot d \
			-drive file=$(AUTOTEST_DISK),format=raw,if=ide,index=0,media=disk \
			-no-reboot; \
		qemu_status=$$?; \
		if grep -q "AUTOTEST: PASS" $(AUTOTEST_LOG); then \
			echo "LatterOS automated QEMU tests: PASSED"; \
			result=0; \
		else \
			echo "LatterOS automated QEMU tests: FAILED"; \
			echo "QEMU exit status: $$qemu_status"; \
			echo "--- $(AUTOTEST_LOG) ---"; \
			cat $(AUTOTEST_LOG); \
			result=1; \
		fi; \
		$(MAKE) -f GNUmakefile clean >/dev/null; \
		exit $$result

clean-test:
	rm -f $(AUTOTEST_DISK) $(AUTOTEST_LOG)
	$(MAKE) -f GNUmakefile clean
