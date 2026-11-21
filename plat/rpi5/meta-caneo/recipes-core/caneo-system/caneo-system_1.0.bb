SUMMARY = "caneo Pi system setup: static eth address, CAN channels (systemd-networkd), sudo for user can"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://10-eth.network \
    file://80-can0.network \
    file://80-can1.network \
    file://80-can2.network \
    file://80-can3.network \
    file://sudoers-can \
"
S = "${WORKDIR}"

do_install() {
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${WORKDIR}/*.network ${D}${sysconfdir}/systemd/network/
    install -d -m 0750 ${D}${sysconfdir}/sudoers.d
    install -m 0440 ${WORKDIR}/sudoers-can ${D}${sysconfdir}/sudoers.d/can
}

FILES:${PN} = "${sysconfdir}/systemd/network ${sysconfdir}/sudoers.d"
RDEPENDS:${PN} = "sudo"
