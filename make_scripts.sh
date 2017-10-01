#!/bin/sh
#for compiling cartographer sdk
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/media/psf/Home/r16/tinaLinux_resize/out/astar-parrot/staging_dir/target/host/lib
#make clean
make
make install
echo "pushing library and includes to r16 devce"
adb push ../r16_install /mnt/SDCARD/cartographer/

