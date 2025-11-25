#!/bin/bash
set -e
set -x

rm -rf test_metadata test_metadata.o

gcc -g -O0 test_metadata.c framesextract.c -o test_metadata
