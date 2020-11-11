#!/bin/bash

# Print script commands.
set -x
# Exit on errors.
set -e

NUM_CORES=`grep -c ^processor /proc/cpuinfo`

COMPILE_TARGET=${1:-all}

# Bmv2
cd behavioral-model

#make clean

if [ "$COMPILE_TARGET" = "all" ]; then
	echo "Compiling all targets.."
	./autogen.sh
	./configure --enable-debugger --with-pi --disable-logging-macros --disable-elogger 'CFLAGS=-O3' 'CXXFLAGS=-O3'
	#./configure --with-pi --disable-logging-macros --disable-elogger 'CFLAGS=-O3' 'CXXFLAGS=-O3'
	make -j${NUM_CORES}
	sudo make install
	sudo ldconfig
fi

# Simple_switch_grpc target
echo "Compiling simple_switch target.."
cd targets/simple_switch_grpc
sudo make clean
./autogen.sh
./configure --with-thrift
make -j${NUM_CORES}
sudo make install
sudo ldconfig
cd ..
cd ..
cd ..
