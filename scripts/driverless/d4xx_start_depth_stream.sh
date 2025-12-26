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

echo "=======Configuring DS5:======="
ds5_reg_write_16 0x10 0x400 0x0001 # DS5_MIPI_LANE_NUMS
ds5_reg_write_16 0x10 0x402 0x03e8 # DS5_MIPI_LANE_DATARATE

ds5_reg_write_16 0x10 0x4000 0x31 # DS5_DEPTH_STREAM_DT
ds5_reg_write_16 0x10 0x4002 0x12 # DS5_DEPTH_STREAM_MD
ds5_reg_write_16 0x10 0x401C 0x1E # DS5_DEPTH_OVERRIDE
ds5_reg_write_16 0x10 0x400C 30 # DS5_DEPTH_FPS
ds5_reg_write_16 0x10 0x4004 1280 # DS5_DEPTH_RES_WIDTH
ds5_reg_write_16 0x10 0x4008 720 # DS5_DEPTH_RES_HEIGHT
echo "=======Starting stream:======="

ds5_reg_write_16 0x10 0x1000 0x200 # DS5_STREAM_START | stream_id(0)

sleep 1

depth_stream_status="$(ds5_reg_read_16 0x10 0x1004)"
depth_config_status="$(ds5_reg_read_16 0x10 0x4800)"

depth_stream_value=$((${depth_config_status:9:6}))
depth_config_value=$((${depth_stream_status:9:6}))

if [[ $depth_stream_value -ne 0x0001 ]]; then
	echo "depth_stream_status != 0x1 got $depth_stream_value"
elif [[ $depth_config_value -ne 0x0002 ]]; then
	echo "depth_stream_status != 0x2 got $depth_config_value"
else
	echo "SUCCESS"
fi