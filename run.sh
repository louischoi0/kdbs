qemu-system-riscv64 \
  -machine virt \
  -nographic \
  -kernel arch/riscv/boot/Image \
	-smp 4 \
	-drive file=kdb.img,if=virtio \
	-initrd rootfs.img \
	-netdev user,id=net0,hostfwd=tcp:127.0.0.1:15432-10.0.2.15:15432 \
	-device virtio-net-device,netdev=net0 \
	-append "console=ttyS0 root=/dev/ram rdinit=/sbin/init"
