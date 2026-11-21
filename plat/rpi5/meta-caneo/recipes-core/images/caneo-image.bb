SUMMARY = "Raspberry Pi 5 + PCAN-M.2: headless caneo gRPC gateway (192.168.10.75)"
LICENSE = "MIT"

inherit core-image extrausers

IMAGE_FEATURES += "ssh-server-openssh"

IMAGE_INSTALL = " \
    packagegroup-core-boot \
    ${CORE_IMAGE_EXTRA_INSTALL} \
    kernel-modules \
    caneo \
    caneo-system \
    can-utils \
    iproute2 \
    iproute2-ip \
    sudo \
    htop \
    nano \
    tzdata \
"

# user "can", password "can" (sha512 crypt, '$' escaped for the shell)
EXTRA_USERS_PARAMS = "usermod -p '\$6\$caneopi5\$Xjmaljv5qP3UAx3Bs3EUsVXOUzhuJJoOMS/9gxTQ6c.6Up0nmkG0n9sFRZ74i2yEtZGUXyczfxyo5KVu/VkD41' can;"

# room for logs on the SD card
IMAGE_ROOTFS_EXTRA_SPACE = "1048576"
