docker exec -it riscv-kernel-build riscv64-linux-gnu-gcc -g -O0 -static -O2 -o kds_init kds_init.c
chmod +x kds_init
mv kds_init rootfs/sbin/init
cd rootfs
find . | cpio -o -H newc | gzip > ../rootfs.img
