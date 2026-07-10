#!/usr/bin/env bash

set -o nounset
set -o pipefail
set -o errexit
#set -o xtrace

executed_file="${BASH_SOURCE[0]}"
script_dir=$(dirname $(realpath "${executed_file}"))
root_dir=$(dirname "${script_dir}")

target=${1:-}
debugging_args=""
#debugging_args="V=1"

if [[ ${ARCH:-} == "arm" ]]; then
	target=coltrane
	defconfig=coltrane_defconfig
elif [[ ${ARCH:-} == "arm64" ]]; then
	target=davis
	defconfig=davis_blade_1_defconfig
else
	echo "You should run this script in an SDK sourced env"
	exit 1
fi

build_dir=/vortex/${target}
if [[ -d $(dirname ${build_dir}) ]]; then
	mkdir -p ${build_dir}
else
	build_dir=${root_dir}
fi

if [[ "${target}" == "coltrane" ]]; then
	sdma_firmware_path=${root_dir}/firmware/imx/sdma/sdma-imx6q.bin
	if [[ ! -f ${sdma_firmware_path} ]]; then
		echo "You are gonna wanna grab sdma-imx6q from: https://github.com/ShaperTools/coltrane-firmware/blob/develop/in-kernel/firmware/imx/sdma/sdma-imx6q.bin"
		echo "and deposit it here: ${sdma_firmware_path}"
		exit 1
	fi
fi

make_args="O=${build_dir} ${debugging_args}"

mod_install_path=${build_dir}/working/modules
fitimage_working_path=${build_dir}/fitimage
deploy_path=${build_dir}/deploy
boot=${deploy_path}/boot

branch=feature-linux-6.6
#branch=develop

if [[ -z ${debugging_args} ]]; then
	rm -Rf ${mod_install_path} ${deploy_path} ${fitimage_working_path}
fi

echo "INSTALL_MOD_PATH=${mod_install_path} make ${make_args} menuconfig"
INSTALL_MOD_PATH=${mod_install_path} make ${make_args} ${defconfig}
INSTALL_MOD_PATH=${mod_install_path} make ${make_args} all
INSTALL_MOD_PATH=${mod_install_path} make ${make_args} modules_install
INSTALL_MOD_PATH=${mod_install_path} make ${make_args} dtbs
INSTALL_MOD_PATH=${mod_install_path} make ${make_args} INSTALL_HDR_PATH=exported_headers headers_install

mkdir -p ${deploy_path}/usr ${boot} ${fitimage_working_path}
cp -r ${mod_install_path}/lib/ ${deploy_path}/usr

mcu_firmware_path=${root_dir}/mcu-firmware.elf

if [[ "${target}" == "davis" ]]; then
	#cp ${build_dir}/arch/arm64/boot/dts/freescale/imx8mm-ddr4-evk.dtb ${fitimage_working_path}/linux.dtb
	#cp ${build_dir}/arch/arm64/boot/Image ${fitimage_working_path}/linux.bin
	
	zstd -T0 --force ${build_dir}/arch/arm64/boot/dts/freescale/davis-blade-1.dtb -o ${fitimage_working_path}/linux.dtb
	zstd -T0 --force ${build_dir}/arch/arm64/boot/Image -o ${fitimage_working_path}/linux.bin

	cp ${script_dir}/fitimage/* ${fitimage_working_path}

	cd ${fitimage_working_path}
	mkimage -f origin-boot.its origin-boot.itb
	cp ${fitimage_working_path}/origin-boot.itb ${boot}

	mkimage -f net-boot.its net-boot.itb
	cp ${fitimage_working_path}/net-boot.itb ${boot}

	mkimage -f net-boot-legacy.its net-boot-legacy.itb
	cp ${fitimage_working_path}/net-boot-legacy.itb ${boot}

    if [[ -f ${mcu_firmware_path} ]]; then
	    zstd -T0 --force ${mcu_firmware_path} -o ${fitimage_working_path}/mcu-firmware.elf
	    mkimage -f net-boot-mcu.its net-boot-mcu.itb
	    cp ${fitimage_working_path}/net-boot-mcu.itb ${boot}
    else
        echo "no mcu-firmware found at ${mcu_firmware_path}, net-boot-mcu.itb will be skipped"
    fi
else
	cp ${build_dir}/arch/arm/boot/dts/nxp/imx/coltrane.dtb ${boot}
	cp ${build_dir}/arch/arm/boot/zImage ${boot}
fi
