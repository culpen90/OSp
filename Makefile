PYTHON ?= python3
QEMU ?= qemu-system-i386
NET ?= user,model=rtl8139

.PHONY: all run test test-network test-network-live clean debug

all:
	$(PYTHON) tools/build.py

run: all
	$(QEMU) -name OSp -machine pc -accel tcg -m 32M -drive file=build/osp.img,format=raw,if=floppy,readonly=on -boot order=a -nic $(NET)

test: all
	$(PYTHON) tools/test.py --qemu $(QEMU)
	$(PYTHON) tools/test_network.py --qemu $(QEMU)

test-network: all
	$(PYTHON) tools/test_network.py --qemu $(QEMU)

test-network-live: all
	$(PYTHON) tools/test_network.py --qemu $(QEMU) --live

debug: all
	$(QEMU) -name OSp -machine pc -accel tcg -m 32M -drive file=build/osp.img,format=raw,if=floppy,readonly=on -boot order=a -nic $(NET) -s -S

clean:
	rm -rf build
