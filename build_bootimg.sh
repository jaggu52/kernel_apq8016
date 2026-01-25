make -j4 O=build Image.gz modules dtbs PERL=/usr/bin/perl
sync
cat build/arch/$ARCH/boot/Image.gz build/arch/$ARCH/boot/dts/qcom/apq8016-sbc.dtb > build/Image.gz+dtb
sync
echo "not a ramdisk" > build/ramdisk.img
#abootimg --create build/boot-db410c.img -k build/Image.gz+dtb -r build/ramdisk.img -c pagesize=2048 -c kerneladdr=0x80008000 -c ramdiskaddr=0x81000000 -c cmdline="root=/dev/mmcblk0p14 rw rootwait rootdelay=8 console=tty0 console=ttyMSM0,115200n8"
abootimg --create build/boot-db410c.img   -k build/Image.gz+dtb   -r build/ramdisk.img   -c pagesize=2048   -c kerneladdr=0x80008000   -c ramdiskaddr=0x81000000   -c cmdline="root=PARTUUID=1be9b461-71ed-43f0-a07c-35d72366b655 rw rootwait console=tty0 console=ttyMSM0,115200n8"
sync
md5sum build/boot-db410c.img
