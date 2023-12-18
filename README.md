# FAST ’24 Artifacts Evaluation
Thank you for your time on evaluating our artifact. Here are the steps for setting up the FEMU for CVSSD.

## Title: The Design and Implementation of a Capacity-Variant Storage System
Contact: Ziyang Jiao (zjiao04@syr.edu), Xiangqun Zhang (xzhang84@syr.edu)

## Contents
- [1. Hardware requirements](#1-hardware-requirements)
- [2. Downloading the repository](#2-downloading-the-repository)
- [3. Compilation](#3-compilation)
- [4. Preparing the VM image (skip for AE)](#4-preparing-the-vm-image-skip-for-artifact-evaluation-committe)
- [5. Starting the virtual machine](#5-starting-the-virtual-machine)
- [6. Connecting into the virtual machine](#6-connecting-into-the-virtual-machine)
- [7. Setting up CV-SSD](#7-setting-up-cv-ssd)
- [8. FIO experiments on CV-SSD](#8-fio-experiments-on-cv-ssd-without-logical-capacity-adjustment)
- [9. FIO experiments on CVSS (CV-FS+CV-SSD+CV-Manager)](#9-fio-experiments-on-cvss-cv-fscv-ssdcv-manager)

## 1. Hardware requirements

Please make sure you have at least 160 GiB memory and 150 GiB free space on your disk if testing on your own machine. Our evaluation is based on the following hardware specifications:

| **Hardware** | **Specification**                  |
|---------------|------------------------------------|
| Processor     | Intel(R) Xeon(R) Silver 4208 CPU @ 2.10GHz, 32-Core |
| Architecture  | x86_64                        |
| Memory        | DDR4 2666 MHz, 1 TiB (64 GB x16)  |
| OS            | Ubuntu 20.04.6 LTS (kernel 5.15.0-86-generic)|

## 2. Downloading the repository

To clone the code, please run the following command:
```bash
git clone https://github.com/ZiyangJiao/FAST24_CVSS_FEMU.git
```

## 3. Compilation

To compile the code, please run the following commands after cloning the repository:
```bash
cd FAST24_CVSS_FEMU
mkdir build-femu
cd build-femu
cp ../femu-scripts/femu-copy-scripts.sh ./
./femu-copy-scripts.sh ./
./femu-compile.sh
```

## 4. Preparing the VM image (skip for artifact evaluation committe)

You can either build your own VM image, or use the VM image provided by us

**Option 1**: This is the **recommended** way to get CV-SSD running quickly - Use our VM image file. To obtain the VM image, you can contact Ziyang Jiao, Email: ``zjiao04@syr.edu`` or Xiangqun Zhang, Email: ``xzhang84@syr.edu``. The VM image downloading instructions will be sent to your email address.

**Option 2**: To build your own VM image, please refer to the [FEMU instructions](https://github.com/vtess/FEMU).

## 5. Starting the virtual machine
We first copy the pre-configured disk imgae to the current directory (`FAST24_CVSS_FEMU/build-femu`):
```bash
cp /media/tmp_nvme4/u20s.qcow2.FAST24AE ./u20s.qcow2.FAST24AE
```

To start the virtual machine, please run:
```bash
./run-blackbox.sh
```

This will start the virtual machine (based on QEMU). You can set the path to your VM image via `IMGDIR=/` in the script.

## 6. Connecting into the virtual machine

Although the terminal shows an operable console for the virtual machine, it has some limitations. For example, there are no highlights for the terminal. Using Ctrl-C could terminate the virtual machine directly instead of terminating the process running under the virtual machine. Therefore, we have an extra SSH port to connect to the virtual machine for better usability. 

To connect to the virtual machine, please run:
```bash
ssh femu@localhost -p 8080
```

If there is a prompt for the password, simply use _femu_ as password.

## 7. Setting up CV-SSD

We use fio as an example here to test the functionality of CV-SSD:

- First, we check the status of the emulated deivce (/dev/nvme0n1) by issuing `lsblk` command. We should find the emulated deivce with ~120GiB capacity as below.

    ```bash
    femu@fvm:~$ lsblk 
    NAME    MAJ:MIN RM   SIZE RO TYPE MOUNTPOINT
    fd0       2:0    1     4K  0 disk 
    loop0     7:0    0  55.7M  1 loop /snap/core18/2785
    loop1     7:1    0  63.5M  1 loop /snap/core20/2015
    loop2     7:2    0  40.9M  1 loop /snap/snapd/19993
    loop3     7:3    0  55.7M  1 loop /snap/core18/2790
    loop4     7:4    0  91.9M  1 loop /snap/lxd/24061
    loop6     7:6    0  91.8M  1 loop /snap/lxd/23991
    loop7     7:7    0  40.9M  1 loop /snap/snapd/20290
    loop8     7:8    0  63.9M  1 loop /snap/core20/2105
    sda       8:0    0    80G  0 disk 
    ├─sda1    8:1    0     1M  0 part 
    └─sda2    8:2    0    80G  0 part /
    nvme0n1 259:0    0 119.2G  0 disk 
    ```

- Second, we initiate the capacity-variant mode and disable WL in the deivice.
    ```bash
    sudo nvme admin-passthru /dev/nvme0n1 --opcode=0xef --cdw10=0x0008 --cdw11=0x00 --cdw12=0x02 --cdw13=0x00 -r -b
    ```
This new `nvme-cli` command enbales the CV-SSD mode, where `cdw12` is used to adjust the acceleration factor for aging, and `cdw13` is used to update the threshold to determine retired blocks. A lower value makes the blocks retire earlier and thus causes a shorter device lifetime and higher device reliability. The CV-SSD will use the default threshold and ignore this field if `cdw13=0x00`.

## 8. FIO experiments on CV-SSD (Without logical capacity adjustment)

- To run FIO directly:
    ```bash
    sudo fio --randrepeat=1 --ioengine=libaio --direct=1 --name=test --bs=16k --iodepth=128 --readwrite=randwrite  --size=10G --filename=/dev/nvme0n1
    ```
This example issues 10GiB random write to the CV-SSD (`/dev/nvme0n1`). The detailed usage of FIO can be found on https://fio.readthedocs.io/en/latest/fio_doc.html.

- We can manually set and unset the degraded mode for CV-SSD. This mode updates the block management policies in CV-SSD.
- To set: 
    ```bash
    sudo nvme admin-passthru /dev/nvme0n1 --opcode=0xef --cdw10=0x0A --cdw11=0x01 -r -b
    ```
- To unset: 
    ```bash
    sudo nvme admin-passthru /dev/nvme0n1 --opcode=0xef --cdw10=0x0A --cdw11=0x00 -r -b
    ```

This part tests the funtionality of CV-SSD and our hardware emulation platform. The CV-SSD exports a fixed logical capacity without the capacity variance feature since CV-FS and CV-Manager was not enabled.
To enable capacity variance, please refer to the following section.

## 9. FIO experiments on CVSS (CV-FS+CV-SSD+CV-Manager)

We now test the functionality of CV-FS:

- First, the CV-FS code can be found under the directory `/home/femu/linux-5.15.0-76-generic-f2fs`.
- Second _(**skip for AE**)_, we can complie CV-FS using the run.sh script under this directory. This step can be skipped since the provided image has done it.
    ```bash
    sudo ./run.sh
    ```
    
- Third, we build CV-FS on CV-SSD. This can be done by running:
    ```bash
    inscvfs
    diskcvfs
    ```
This command will build the file system and mount the CV-SSD on the path `/mnt/nvme0n1`. It also create directories for the experiments.
- Fourth, we can check the status of the system using `lsblk`. We should find the device is mounted successfully and ready to use. Below is an example:

    ```bash
    femu@fvm:/mnt/nvme0n1$ lsblk
    NAME    MAJ:MIN RM   SIZE RO TYPE MOUNTPOINT
    fd0       2:0    1     4K  0 disk 
    loop0     7:0    0  55.7M  1 loop /snap/core18/2785
    loop1     7:1    0  63.5M  1 loop /snap/core20/2015
    loop2     7:2    0  40.9M  1 loop /snap/snapd/19993
    loop3     7:3    0  55.7M  1 loop /snap/core18/2790
    loop4     7:4    0  91.9M  1 loop /snap/lxd/24061
    loop6     7:6    0  91.8M  1 loop /snap/lxd/23991
    loop7     7:7    0  40.9M  1 loop /snap/snapd/20290
    loop8     7:8    0  63.9M  1 loop /snap/core20/2105
    sda       8:0    0    80G  0 disk 
    ├─sda1    8:1    0     1M  0 part 
    └─sda2    8:2    0    80G  0 part /
    nvme0n1 259:0    0 119.2G  0 disk /mnt/nvme0n1
    ```

We now test the functionality of CVSS and CV-Manager:

- First, we can find the code of CV-Manager (cv_manager.py) under `/home/femu/f2fs` and the user-level tool under `/home/femu/f2fs-tools`.
- Second, we can issue some workloads to the system. Below is an example:
    ```bash
    sudo fio --randrepeat=1 --ioengine=libaio --direct=1 --name=test --bs=16k --iodepth=128 --readwrite=randwrite  --size=10G --filename=/mnt/nvme0n1/test1.fio
    ```
    Note that the filename atttribute is the mount point of the device, instead of /dev/nvme0n1.
- Third, the logical capacity of CVSS can be adjusted online by issuing:
    ```bash
    sudo /home/femu/f2fs/reduction_with_parameter.out 118
    ```
    The parameter (e.g., 118) is the new logical capacity we set for the system.
    This can also be done by using the file system utility tool.
    ```bash
    sudo cvfs.f2fs /dev/nvme0n1 -f /mnt/nvme0n1/resize.tmp -t 118
    ```
- Fourth, we can validate the size of the logical capacity by issuing:
    ```bash
    df -h /dev/nvme0n1
    ```

    The new logical size should be shown under the Size field as following.
    ```bash
    Filesystem      Size  Used Avail Use% Mounted on
    /dev/nvme0n1    118G  1.9G  117G   2% /mnt/nvme0n1
    ```

- Fifth, we test the functionality of CVSS after the logical capacity is updated. 
    ```bash
    sudo fio --randrepeat=1 --ioengine=libaio --direct=1 --name=test --bs=16k --iodepth=128 --readwrite=randwrite  --size=10G --filename=/mnt/nvme0n1/test2.fio
    ```

    We now should find the file (test2.fio) under the directory `/mnt/nvme0n1/`.
- Sixth, the monitor of bad capacity and the adjustment of the logical capacity can be automated by CV-Manager:
    ```bash
    sudo /home/femu/f2fs/cv_manager.py
    ```

