#!/bin/bash

cd $PWD/pintos/src/threads
make clean
make
cd build
pintos -v -k -T 60 --bochs  -- -q   run alarm-multiple < /dev/null
