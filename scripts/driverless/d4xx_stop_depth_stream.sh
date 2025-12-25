#!/bin/bash

function ds5_reg_write_16() {
	addr_msb=$(($2 >> 8))
	addr_lsb=$(($2 & 0xff))

	val_msb=$(($3 >> 8))
	val_lsb=$(($3 & 0xff))

	sudo i2ctransfer -y 2 w4@$1 $addr_lsb $addr_msb $val_lsb $val_msb
	echo w $2 $3
}

function ds5_reg_read_16() {
	addr_msb=$(($2 >> 8))
	addr_lsb=$(($2 & 0xff))

	result="$(sudo i2ctransfer -y 2 w2@$1 $addr_lsb $addr_msb r2@$1)"
	echo r $2 "${result:5:4}${result:2:2}"
}

echo "=======Stopping stream:======="

ds5_reg_write_16 0x10 0x1000 0x100 # DS5_STREAM_START | stream_id(0)

sleep 1

depth_stream_status="$(ds5_reg_read_16 0x10 0x1004)"
depth_config_status="$(ds5_reg_read_16 0x10 0x4800)"

depth_stream_value=$((${depth_config_status:9:6}))
depth_config_value=$((${depth_stream_status:9:6}))

if [[ $depth_stream_value -ne 0x0000 ]]; then
	echo "depth_stream_status != 0x0 got $depth_stream_value"
elif [[ $depth_config_value -ne 0x0001 ]]; then
	echo "depth_stream_status != 0x1 got $depth_config_value"
else
	echo "SUCCESS"
fi
