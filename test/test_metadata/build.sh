#!/bin/bash
set -e
set -x

rm -rf test_metadata test_metadata.o

gcc test_metadata.c framesextract.c -o test_metadata

./test_metadata "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8"
