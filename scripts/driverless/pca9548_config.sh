#!/bin/bash

function write_and_read_back() {
	addr_msb=$(($2 >> 8))
	addr_lsb=$(($2 & 0xff))

	sudo i2ctransfer -y 2 w3@$1 $addr_msb $addr_lsb $3
	result="$(sudo i2ctransfer -y 2 w2@$1 $addr_msb $addr_lsb r1@$1)"
	echo $2 $result
}

echo -n "Pinging PCA9548 @0x72: "
sudo i2ctransfer -y 2 r1@0x72 &> /dev/null
if [ $? -eq 0 ]; then
	# Mux is available, setting its channels to 0xff
	echo "Success"
	echo "Enabling mux channels"
	sudo i2ctransfer -y 2 w1@0x72 0xff
else
	echo "Mux unresponsive"
	return -1
fi
