#!/bin/bash

UBOOT_VER=v2022.07
echo "UBOOT_VER=$UBOOT_VER" >> $GITHUB_ENV

# Prepare compile environment
sudo apt-get update && sudo apt-get install -y bc build-essential bison flex python3 python3-distutils swig python3-dev libpython3-dev device-tree-compiler wget
wget "https://developer.arm.com/-/media/Files/downloads/gnu/11.3.rel1/binrel/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu.tar.xz"
tar xf arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu.tar.xz
export PATH=`pwd`/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH

# Get source code
wget -O uboot.tar.gz "https://github.com/u-boot/u-boot/archive/refs/tags/${UBOOT_VER}.tar.gz"
wget -O rkbin.tar.gz "https://github.com/rockchip-linux/rkbin/archive/refs/heads/master.tar.gz"
wget -O rkbin_old.tar.gz "https://github.com/rockchip-linux/rkbin/archive/96997db3bbd501437165325917114fb13a2e645b.tar.gz"
tar xzf uboot.tar.gz && mv u-boot-* u-boot
tar xzf rkbin.tar.gz && mv rkbin-* rkbin
tar xzf rkbin_old.tar.gz && mv rkbin-96997db3bbd501437165325917114fb13a2e645b rkbin_old
mv rkbin/tools/boot_merger rkbin/tools/boot_merger.new
mv rkbin_old/tools/{boot_merger,lib} rkbin/tools/
rm -r rkbin_old
mkdir miniloader tpl-spl
BL31=`grep bl31 rkbin/RKTRUST/RK3328TRUST.ini | cut -d'/' -f3`
cp rkbin/bin/rk33/${BL31} u-boot/bl31.elf
ROOT="$(pwd)"

# Build U-Boot
echo "Building u-boot"
cd ${ROOT}/u-boot
patch -p1 < ${ROOT}/patches/dtb.patch
cp ${ROOT}/patches/rk3328-beikeyun-u-boot.dtsi ${ROOT}/u-boot/arch/arm/dts/rk3328-beikeyun-u-boot.dtsi
cp ${ROOT}/patches/rk3328-beikeyun.dts ${ROOT}/u-boot/arch/arm/dts/rk3328-beikeyun.dts
cp ${ROOT}/patches/beikeyun-rk3328_defconfig ${ROOT}/u-boot/configs/beikeyun-rk3328_defconfig
#cp ${ROOT}/patches/phy-rockchip-inno-usb3.c ${ROOT}/u-boot/drivers/phy/rockchip/phy-rockchip-inno-usb3.c
#cp ${ROOT}/patches/rockchip_sip.h ${ROOT}/u-boot/include/linux/rockchip/rockchip_sip.h
#cp ${ROOT}/patches/rockchip_sip.c ${ROOT}/u-boot/drivers/firmware/rockchip_sip.c
#cp ${ROOT}/patches/rockchip-ddr.h ${ROOT}/u-boot/include/dt-bindings/clock/rockchip-ddr.h
#cp ${ROOT}/patches/rk3328-dram.h ${ROOT}/u-boot/include/dt-bindings/memory/rk3328-dram.h
CROSS_COMPILE=aarch64-none-linux-gnu- make beikeyun-rk3328_defconfig
CROSS_COMPILE=aarch64-none-linux-gnu- make
mv idbloader.img u-boot.itb ${ROOT}/tpl-spl/
cp u-boot.bin ${ROOT}/rkbin/

# Build miniloader
echo "Generating miniloader"
cd ${ROOT}/rkbin
FD=`grep 'FlashData=' RKBOOT/RK3328MINIALL.ini | cut -f2 -d'='`
FB=`grep 'FlashBoot=' RKBOOT/RK3328MINIALL.ini | cut -f2 -d'='`
LOADER=`grep 'PATH=' RKBOOT/RK3328MINIALL.ini | cut -f2 -d'='`
tools/mkimage -n rk3328 -T rksd -d ${FD} idbloader.img
cat ${FB} >> idbloader.img
sed -i '/\[BL32/,/SEC=1/s/SEC=1/SEC=0/' RKTRUST/RK3328TRUST.ini
tools/trust_merger RKTRUST/RK3328TRUST.ini
tools/loaderimage --pack --uboot u-boot.bin uboot.img 0x00200000
tools/boot_merger pack RKBOOT/RK3328MINIALL.ini
mv idbloader.img uboot.img trust.img ${LOADER} ${ROOT}/miniloader/

# Package built binaries
cd ${ROOT}/miniloader
tar czf "../uboot-${UBOOT_VER}-miniloader.tar.gz" idbloader.img uboot.img trust.img ${LOADER}
cd ${ROOT}/tpl-spl
tar czf "../uboot-${UBOOT_VER}-spl.tar.gz" idbloader.img u-boot.itb
