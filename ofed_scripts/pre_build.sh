#!/bin/bash
kernelver=$1
kernel_source_dir=$2
PACKAGE_NAME=$3
PACKAGE_VERSION=$4

config_flag=`/var/lib/dkms/${PACKAGE_NAME}/${PACKAGE_VERSION}/source/ofed_scripts/dkms_ofed $kernelver get-config`

make distclean

NJOBS=`grep ^processor /proc/cpuinfo | wc -l`

find compat -type f -exec touch -t 200012201010 '{}' \; || true
./configure --kernel-version=$kernelver --kernel-sources=$kernel_source_dir ${config_flag} --with-njobs=${NJOBS:-1}

make -j${NJOBS:-1}

./ofed_scripts/install_helper
