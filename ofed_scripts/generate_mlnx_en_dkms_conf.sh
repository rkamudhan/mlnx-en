#!/bin/bash
#
# Copyright (c) 2015 Mellanox Technologies. All rights reserved.
#
# This Software is licensed under one of the following licenses:
#
# 1) under the terms of the "Common Public License 1.0" a copy of which is
#    available from the Open Source Initiative, see
#    http://www.opensource.org/licenses/cpl.php.
#
# 2) under the terms of the "The BSD License" a copy of which is
#    available from the Open Source Initiative, see
#    http://www.opensource.org/licenses/bsd-license.php.
#
# 3) under the terms of the "GNU General Public License (GPL) Version 2" a
#    copy of which is available from the Open Source Initiative, see
#    http://www.opensource.org/licenses/gpl-license.php.
#
# Licensee has the right to choose one of the above licenses.
#
# Redistributions of source code must retain the above copyright
# notice and one of the license notices.
#
# Redistributions in binary form must reproduce both the above copyright
# notice, one of the license notices in the documentation
# and/or other materials provided with the distribution.
#
#

cd ${0%*/*}

with_mlx4=${with_mlx4:-1}
with_mlx5=${with_mlx5:-1}
MLNX_EN_PATCH_PARAMS=

kernelver=${kernelver:-`uname -r`}
kernel_source_dir=${kernel_source_dir:-"/lib/modules/$kernelver/build"}
PACKAGE_NAME=${PACKAGE_NAME:-"mlnx-en"}
PACKAGE_VERSION=${PACKAGE_VERSION:-"3.0"}

echo STRIP_MODS=\${STRIP_MODS:-"yes"}
echo kernelver=\${kernelver:-\$\(uname -r\)}
echo kernel_source_dir=\${kernel_source_dir:-"/lib/modules/\$kernelver/build"}

modules="compat/mlx_compat"
if [ $with_mlx4 -eq 1 ]; then
	modules="$modules drivers/net/ethernet/mellanox/mlx4/mlx4_core drivers/infiniband/hw/mlx4/mlx4_ib drivers/net/ethernet/mellanox/mlx4/mlx4_en"
else
	MLNX_EN_PATCH_PARAMS="$MLNX_EN_PATCH_PARAMS --without-mlx4"
fi
if [ $with_mlx5 -eq 1 ]; then
	modules="$modules drivers/net/ethernet/mellanox/mlx5/core/mlx5_core drivers/infiniband/hw/mlx5/mlx5_ib"
else
	MLNX_EN_PATCH_PARAMS="$MLNX_EN_PATCH_PARAMS --without-mlx5"
fi

i=0

for module in $modules
do
	name=`echo ${module##*/} | sed -e "s/.ko//"`
	echo BUILT_MODULE_NAME[$i]=$name
	echo BUILT_MODULE_LOCATION[$i]=${module%*/*}
	echo DEST_MODULE_NAME[$i]=$name
	echo DEST_MODULE_LOCATION[$i]=/kernel/${module%*/*}
	echo STRIP[$i]="\$STRIP_MODS"
	let i++
done

echo "MAKE=\"./scripts/mlnx_en_patch.sh ${MLNX_EN_PATCH_PARAMS} -j\$(grep ^processor /proc/cpuinfo | wc -l) && make -j\$(grep ^processor /proc/cpuinfo | wc -l)\""
echo CLEAN=\"make clean\"
echo PACKAGE_NAME=$PACKAGE_NAME
echo PACKAGE_VERSION=$PACKAGE_VERSION
echo REMAKE_INITRD=yes
echo AUTOINSTALL=yes


