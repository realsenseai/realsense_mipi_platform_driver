#!/bin/bash

function write_and_read_back() {
	addr_msb=$(($2 >> 8))
	addr_lsb=$(($2 & 0xff))

	sudo i2ctransfer -y 2 w3@$1 $addr_msb $addr_lsb $3
	#sleep 0.1
	result="$(sudo i2ctransfer -y 2 w2@$1 $addr_msb $addr_lsb r1@$1)"
	#sleep 0.1
	echo $2 $result
}

################## Deserializer (max96712) configuration ##################
echo "=======Configuring Deserializer MAX6712:======="
write_and_read_back 0x29 0x017 0x14
# Cannot do write_and_read_back when the write is a reset, using regular transfer
sudo i2ctransfer -y 2 w3@0x29 0x00 0x18 0xF0 # Reset All Linkes
sudo i2ctransfer -y 2 w3@0x29 0x00 0x18 0x00 # Reset release
sleep 0.1

write_and_read_back 0x29 0x040B 0x00 # (CSI_OUT_EN): CSI output disabled
write_and_read_back 0x29 0x0005 0x80 # GMSL2 link locked output
write_and_read_back 0x29 0x0006 0xF1 # Enable only Link A
write_and_read_back 0x29 0x08A0 0x21 # MIPI output configured as 4 ports with 2 data lanes each
write_and_read_back 0x29 0x08A2 0xF4 # Enabling all PHYs
write_and_read_back 0x29 0x08A3 0x44 # Set D0&D1 to PHY1, D0&D1 to PHY0
write_and_read_back 0x29 0x08A5 0x00 # All lane poarity Non-Inverse
write_and_read_back 0x29 0x00F0 0x10 # PHY_A -> Pipe_Z ; PHY_A -> Pipe_U
write_and_read_back 0x29 0x00F1 0x32 # PHY_A -> Pipe_X ; PHY_A -> Pipe_Y
write_and_read_back 0x29 0x00F4 0x0F # Enabling Pipe 0 to 3
write_and_read_back 0x29 0x090B 0x0F # Enable 2 Pipe mapping

write_and_read_back 0x29 0x090B 0x0F # Enable 4 mappings for Pipe X
write_and_read_back 0x29 0x090D 0x1E # Map SRC0 Depth VC0 - YUV422-8 (DT:1E)
write_and_read_back 0x29 0x090E 0x1E # Map DST0 Depth VC0 - YUV422-8 (DT:1E)
write_and_read_back 0x29 0x090F 0x00 # Map SRC1 VC0
write_and_read_back 0x29 0x0910 0x00 # Map DST1 VC0
write_and_read_back 0x29 0x0911 0x01 # Map SRC2 VC0
write_and_read_back 0x29 0x0912 0x01 # Map DST2 VC0
write_and_read_back 0x29 0x0913 0x12 # Map SRC3 EMB8 VC0 (DT:12)
write_and_read_back 0x29 0x0914 0x12 # Map DST3 EMB8 VC0 (DT:12)
write_and_read_back 0x29 0x092D 0x00 # Map to PHY1 (master for port A)
write_and_read_back 0x29 0x0100 0x33 # SEQ_MISS_EN Pipe X: Disabled / DIS_PKT_DET Pipe X: Disabled
write_and_read_back 0x29 0x1458 0x28 # PHY A Optimization - ErrChVTh0
write_and_read_back 0x29 0x1459 0x68 # PHY A Optimization - ErrChVTh1
write_and_read_back 0x29 0x1558 0x28 # PHY B Optimization - ErrChVTh0
write_and_read_back 0x29 0x1559 0x68 # PHY B Optimization - ErrChVTh1
write_and_read_back 0x29 0x090A 0x40 # CSI 2 data lanes
write_and_read_back 0x29 0x0418 0x2F # PHY1 CSI 1500 Mbps/lane
write_and_read_back 0x29 0x040B 0x02 # (CSI_OUT_EN): CSI output enabled
write_and_read_back 0x29 0x0018 0x0F # RESET_ONESHOT: Activated
write_and_read_back 0x29 0x0018 0x00 # RESET_ONESHOT: Deactivated
