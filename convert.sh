#!/bin/bash
# Convert YUY2 to a bmp for debug viewing

convert -size 1280x768 pal:1280x768.000047_yuy2.bin new.bmp
