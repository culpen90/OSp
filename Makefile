PYTHON ?= python3
QEMU ?= qemu-system-i386

.PHONY: all run test clean debug

all:
	$(PYTHON) tools/build.py

run: all
	$(QEMU) -name OSp -machine pc -accel tcg -m 32M -drive file=build/osp.img,format=raw,if=floppy,readonly=on -boot order=a -nic none

test: all
	$(PYTHON) tools/test.py --qemu $(QEMU)

debug: all
	$(QEMU) -name OSp -machine pc -accel tcg -m 32M -drive file=build/osp.img,format=raw,if=floppy,readonly=on -boot order=a -nic none -s -S

clean:
	rm -rf build
