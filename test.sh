#!/bin/bash

ulimit -c unlimited

export LD_LIBRARY_PATH=/usr/local/lib

#./h264encoder -W 720 -H 480 -i 192.168.0.67 -p 9998 -b 3000000 -M 1 -T 80 -o stream.nals
#./h264encoder -W 1280 -H 768 -i 192.168.0.80 -p 9998 -b 3000000 -M 1 -f 30
#./h264encoder -W 1920 -H 1080 -i 192.168.0.67 -p 9998 -b 3000000 -M 1 -T 80 -o stream.nals

valgrind --tool=memcheck ./h264encoder -W 1280 -H 768 -i 192.168.0.80 -p 9998 -b 3000000 -M 1 -f 30
