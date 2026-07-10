SHELL := /bin/bash

ifeq ($(findstring ${BUILD_ROOT},${CURDIR}),)
include ${PROJECT_ROOT}/tools/makefiles/target.mk
else

include ${PROJECT_ROOT}/tools/makefiles/common.mk

.NOTPARALLEL:

#all: | ${INSTALL_ROOT}/boot/linux.bin ${INSTALL_ROOT}/boot/linux.dtb ${INSTALL_ROOT}/boot/linux-initramfs.bin
all: | ${INSTALL_ROOT}/boot/linux.bin ${INSTALL_ROOT}/boot/linux.dtb
	@:
.PHONY: all

clean:
	@echo "CLEANING kernel"
	$(Q) -$(MAKE) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} INSTALL_MOD_PATH=${INSTALL_ROOT} clean
.PHONY: clean

menuconfig: ${CURDIR}/.config
	$(Q) $(MAKE) -C ${SOURCE_DIR} O=${CURDIR} ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} menuconfig
	$(Q) install -m 0644 ${CURDIR}/.config ${SOURCE_DIR}/arch/${ARCH}/configs/${KERNEL_CONFIG}
.PHONY: menuconfig

${CURDIR}/.config:
	@echo "CONFIGURING $@"
	$(Q) $(MAKE) -C ${SOURCE_DIR} O=${CURDIR} ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} ${KERNEL_CONFIG}

${CURDIR}/vmlinux: ${CURDIR}/.config
	@echo "BUILDING $@"
	$(Q) $(MAKE) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} vmlinux modules
	$(Q) $(MAKE) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} INSTALL_MOD_PATH=${INSTALL_ROOT} modules_install
	$(Q) $(MAKE) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} INSTALL_HDR_PATH=${INSTALL_ROOT}/usr headers_install
.PHONY: ${CURDIR}/vmlinux

${INSTALL_ROOT}/boot/linux.bin: ${CURDIR}/vmlinux
	@echo "GENERATING ${@}"
	$(Q) $(MAKE) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} Image
	$(Q) install -m 0644 -D ${CURDIR}/arch/${ARCH}/boot/Image ${@}
.PHONY: ${INSTALL_ROOT}/boot/linux.bin

${INSTALL_ROOT}/boot/linux.dtb: ${CURDIR}/.config ${SOURCE_DIR}/arch/${ARCH}/boot/dts/freescale/${KERNEL_DTB}.dts
	@echo "GENERATING ${CURDIR}/arch/${ARCH}/boot/dts/freescale/${KERNEL_DTB}.dtb"
	$(Q) $(MAKE) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
	@echo "INSTALLING ${@}"
	$(Q) install -m 0644 -D ${CURDIR}/arch/${ARCH}/boot/dts/freescale/${KERNEL_DTB}.dtb ${@}
.PHONY: ${INSTALL_ROOT}/boot/linux.dtb

${CURDIR}/initramfs.data:
	@echo "GENERATING ${@}"
	$(Q) -${RM} -rf ${CURDIR}/initramfs
	$(Q) PATH=${PATH}:${CROSS_PATH} ${CROSS_COMPILE}populate -f -l nss_compat-2.31:nss_dns:nss_files-2.31 -s ${INSTALL_ROOT} -d ${CURDIR}/initramfs
	$(Q) find ${CURDIR}/initramfs -type f -name "*.so.[0-9]" -exec $(STRIP) --strip-unneeded "{}" \;
	$(Q) ${SOURCE_DIR}/usr/gen_initramfs_list.sh -u squash -g squash -d ${CURDIR}/initramfs > ${CURDIR}/initramfs.data
	$(Q) sed -i "/file \/boot/d" ${CURDIR}/initramfs.data
	$(Q) sed -i "/file \/usr\/include\//d" ${CURDIR}/initramfs.data
.PHONY: ${CURDIR}/initramfs.data

${INSTALL_ROOT}/boot/linux-initramfs.bin: ${CURDIR}/initramfs.data ${CURDIR}/vmlinux
	@echo "GENERATING ${@}"
	$(Q) $(MAKE) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_INITRAMFS_SOURCE="${CURDIR}/initramfs.data" Image
	$(Q) install -m 0644 -D ${CURDIR}/arch/${ARCH}/boot/Image ${@}
.PHONY: ${INSTALL_ROOT}/boot/linux-initramfs.bin

${INSTALL_ROOT}/boot/linux-initramfs-dtb.bin: ${CURDIR}/initramfs.data ${CURDIR}/vmlinux
	@echo "GENERATING ${@}"
	$(Q) $(MAKE) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_INITRAMFS_SOURCE="${CURDIR}/initramfs.data" CONFIG_ARM_APPENDED_DTB=y Image
	$(Q) cat ${CURDIR}/arch/${ARCH}/boot/Image ${CURDIR}/arch/${ARCH}/boot/dts/freescale/${KERNEL_DTB}.dtb > ${@}
.PHONY: ${INSTALL_ROOT}/boot/linux-initramfs-dtb.bin


endif
