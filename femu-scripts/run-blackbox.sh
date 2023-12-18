#!/bin/bash
# Huaicheng Li <huaicheng@cs.uchicago.edu>
# Run FEMU as a black-box SSD (FTL managed by the device)
# 7.37% OP 128GiB physical, 128GB(119.2GiB) logical, 122070mib

# image directory
IMGDIR=/media/tmp_nvme4
# IMGDIR=/media/tmp_sdc/images_remap
# IMGDIR=/media/tmp_sdc/images_docker
# Virtual machine disk image
OSIMGF=$IMGDIR/u20s.qcow2.FAST24AE
sudo rm wa.log.vSSD0
sudo rm ec.log.vSSD0

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit
fi

sudo x86_64-softmmu/qemu-system-x86_64 \
    -name "FEMU-BBSSD-VM" \
    -enable-kvm \
    -cpu host \
    -smp 12 \
    -m 16G \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=native,cache=none,format=qcow2,id=hd0 \
    -device femu,devsz_mb=122070,femu_mode=1 \
    -net user,hostfwd=tcp::8080-:22 \
    -net nic,model=virtio \
    -nographic \
    -virtfs local,path=/media/tmp_nvme4/hostshare,mount_tag=host0,security_model=passthrough,id=host0 \
    -device virtio-9p-pci,fsdev=host0,mount_tag=hostshare \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log
