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

# Calling the Deserializer (max96712) at its default addr 0x52 (0x29 short addr)
echo -n "Pinging Deserializer @0x52: "
sudo i2ctransfer -y 2 w2@0x29 0x00 0x00 r1@0x29 &> /dev/null
if [ $? -eq 0 ]; then
	# Deserializer is in the default addr
	# Changing serializer address from the default to 0x52 (0x29 short addr) 
	# by writing 0x90 to reg 0 of the device currently @0x52
	# From now on it'll be @48
	echo "Success"
	echo "Changing Serializer addr to 0x84"
	sudo i2ctransfer -y 2 w3@0x29 0x00 0x00 0x90
else
	echo "No response, perhaps already moved to 0x90"
fi

# Verify Deserializer is now at its new addr 0x84 (0x42 short addr)
echo -n "Pinging Deserializer @0x90: "
sudo i2ctransfer -y 2 w2@0x48 0x00 0x00 r1@0x48 &> /dev/null
if [ $? -eq 0 ]; then
	echo "Success"
else
	echo "Deserializer not accessible"
	return -1
fi

################## Deserializer (max9296) configuration ##################
# echo "=======Configuring Deserializer MAX9296:======="
# write_and_read_back 0x48 0x0302 0x10 # Increase CMU regulator voltage
# write_and_read_back 0x48 0x0313 0x00 # (CSI_OUT_EN): CSI output disabled
# # Phy optimization
# write_and_read_back 0x48 0x1458 0x28 # PHY A Optimization
# write_and_read_back 0x48 0x1459 0x68 # PHY A Optimization
# write_and_read_back 0x48 0x1558 0x28 # PHY B Optimization
# write_and_read_back 0x48 0x1559 0x68 # PHY B Optimization
# write_and_read_back 0x48 0x0050 0x00 # (STR_SELX): 0x0
# write_and_read_back 0x48 0x0051 0x01 # (STR_SELY): 0x1

################## Deserializer (max96712) configuration ##################
write_and_read_back 0x48 0x0005 0x80 # GMSL2 link locked output
write_and_read_back 0x48 0x0006 0xF1 # Enable only Link A
write_and_read_back 0x48 0x1458 0x28 # PHY A Optimization - ErrChVTh0
write_and_read_back 0x48 0x1459 0x68 # PHY A Optimization - ErrChVTh1
write_and_read_back 0x48 0x1558 0x28 # PHY B Optimization - ErrChVTh0
write_and_read_back 0x48 0x1559 0x68 # PHY B Optimization - ErrChVTh1
write_and_read_back 0x48 0x1658 0x28 # PHY C Optimization - ErrChVTh0
write_and_read_back 0x48 0x1659 0x68 # PHY C Optimization - ErrChVTh1
write_and_read_back 0x48 0x1758 0x28 # PHY D Optimization - ErrChVTh0
write_and_read_back 0x48 0x1759 0x68 # PHY D Optimization - ErrChVTh1
write_and_read_back 0x48 0x0018 0x0F # RESET_ONESHOT: Activated
write_and_read_back 0x48 0x0018 0x00 # RESET_ONESHOT: Deactivated
write_and_read_back 0x48 0x0414 0xF0
write_and_read_back 0x48 0x0415 0x2F # 1500 Mbps/lane on port A
write_and_read_back 0x48 0x00F0 0x00 # Video Pipe 0 : PHY_A -> Pipe_X
write_and_read_back 0x48 0x00F4 0x03 # Enabling Video Pipe 0
write_and_read_back 0x48 0x040B 0x82
write_and_read_back 0x48 0x040C 0x00
write_and_read_back 0x48 0x040D 0x00
write_and_read_back 0x48 0x040E 0x9E
write_and_read_back 0x48 0x040F 0x02
write_and_read_back 0x48 0x041A 0xF0
write_and_read_back 0x48 0x0415 0xEF
write_and_read_back 0x48 0x0418 0xEF
write_and_read_back 0x48 0x08A2 0x30 # Enabling 0-1 PHYs
write_and_read_back 0x48 0x090A 0xC0
write_and_read_back 0x48 0x094A 0xC0
write_and_read_back 0x48 0x098A 0xC0
write_and_read_back 0x48 0x09CA 0xC0
write_and_read_back 0x48 0x08A0 0x04 # MIPI output configured as 2 ports with 4 data lanes each
write_and_read_back 0x48 0x08A3 0xE4 # Set D0&D1 to PHY1, D2&D3 to PHY0
write_and_read_back 0x48 0x090B 0x02 # Enable 2 Pipe mapping
write_and_read_back 0x48 0x092D 0x05 # Map to PHY1 (master for port A)
write_and_read_back 0x48 0x090D 0x1E # Map SRC0 Depth VC0 - YUV422-8 (DT:1E)
write_and_read_back 0x48 0x090E 0x5E # Map DST0 Depth VC0 - YUV422-8 (DT:1E)
write_and_read_back 0x48 0x090F 0x12 # Map SRC1 VC0
write_and_read_back 0x48 0x0910 0x52 # Map DST1 VC0
# HW-SYNC
# write_and_read_back 0x48 0x0316 0x82 # MFP7 (HW SYNC Depth trigger path)
# write_and_read_back 0x48 0x0317 0x1F
# write_and_read_back 0x48 0x031C 0x82 # MFP9 (HW SYNC RGB trigger path)
# write_and_read_back 0x48 0x031D 0x1B

echo "One-shot reset..."
# Cannot do write_and_read_back when the write is a reset, using regular transfer
sudo i2ctransfer -y 2 w3@0x48 0x00 0x18 0xF # One-shot reset enable auto-link
sleep 0.1
