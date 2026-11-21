SUMMARY = "caneo - CAN bus visualization / send tool (headless gRPC gateway on the Pi)"
LICENSE = "CLOSED"

# files/caneo is built with the Yocto SDK by plat/rpi5/build.sh (step "caneo")
SRC_URI = " \
    file://caneo \
    file://caneo.yaml \
    file://caneo.service \
    file://caneo-wait-can \
"
S = "${WORKDIR}"

inherit systemd useradd

USERADD_PACKAGES = "${PN}"
USERADD_PARAM:${PN} = "-m -d /home/can -s /bin/sh -G dialout,sudo -U can"
GROUPADD_PARAM:${PN} = "-r sudo"

SYSTEMD_SERVICE:${PN} = "caneo.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/caneo ${D}${bindir}/caneo
    install -m 0755 ${WORKDIR}/caneo-wait-can ${D}${bindir}/caneo-wait-can
    install -d ${D}${sysconfdir}/caneo
    install -m 0644 ${WORKDIR}/caneo.yaml ${D}${sysconfdir}/caneo/caneo.yaml
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/caneo.service ${D}${systemd_system_unitdir}/caneo.service
    install -d -o can -g can ${D}/home/can/logs
}

FILES:${PN} += "/home/can"
CONFFILES:${PN} = "${sysconfdir}/caneo/caneo.yaml"
# prebuilt binary from the SDK build (already linked with the SDK's flags)
INSANE_SKIP:${PN} = "already-stripped ldflags file-rdeps"
RDEPENDS:${PN} = "libstdc++ caneo-system"

# The binary only exists after "./build.sh caneo"; until then the recipe is
# skipped so that meta-toolchain can be built first.
python () {
    import os
    # re-parse when files/ changes (binary added/updated), not only when this .bb changes
    bb.parse.mark_dependency(d, os.path.join(d.getVar('THISDIR'), 'files'))
    if not os.path.exists(os.path.join(d.getVar('THISDIR'), 'files', 'caneo')):
        raise bb.parse.SkipRecipe("files/caneo missing - run plat/rpi5/build.sh caneo")
}
