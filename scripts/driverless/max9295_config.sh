#!/bin/bash

function write_and_read_back() {
	addr_msb=$(($2 >> 8))
	addr_lsb=$(($2 & 0xff))

	sudo i2ctransfer -y 2 w3@$1 $addr_msb $addr_lsb $3
	result="$(sudo i2ctransfer -y 2 w2@$1 $addr_msb $addr_lsb r1@$1)"
	echo $2 $result
}

# Calling the Serializer (max9295) at its default addr 0x80 (0x40 short addr)
echo -n "Pinging Serializer @0x80: "
sudo i2ctransfer -y 2 w2@0x40 0x00 0x00 r1@0x40 &> /dev/null
if [ $? -eq 0 ]; then
	# Serializer is in the default addr
	# Changing serializer address from the default to 0x84 (0x42 short addr) 
	# by writing 0x84 to reg 0 of the device currently @0x40
	# From now on it'll be @42
	echo "Success"
	echo "Changing Serializer addr to 0x84"
	sudo i2ctransfer -y 2 w3@0x40 0x00 0x00 0x84
else
	echo "No response, perhaps already moved to 0x84"
fi

# Verify Serializer is now at its new addr 0x84 (0x42 short addr)
echo -n "Pinging Serializer @0x84: "
sudo i2ctransfer -y 2 w2@0x42 0x00 0x00 r1@0x42 &> /dev/null
if [ $? -eq 0 ]; then
	echo "Success"
else
	echo "Serializer not accessible"
	return -1
fi

################## Serializer (max9295) configuration ##################
echo "=======Configuring Serializer MAX9295:======="
write_and_read_back 0x42 0x0302 0x10 # Increase CMU regulator voltage
write_and_read_back 0x42 0x0002 0x03 # (VID_TX_EN_X/Y) disabled
write_and_read_back 0x42 0x0005 0x00 # Disable ERRB and LOCK (POC)

# MAX9295A Setup
write_and_read_back 0x42 0x0002 0xF3 # Enable all pipes
write_and_read_back 0x42 0x0331 0x11 # 4 lanes
write_and_read_back 0x42 0x0308 0x6F # Clock from port B
write_and_read_back 0x42 0x0311 0xF0 # Data from port B
write_and_read_back 0x42 0x0314 0x5E # Pipe X Depth DT
write_and_read_back 0x42 0x0315 0x52 # Pipe X EMB8 DT
write_and_read_back 0x42 0x0309 0x01 # Pipe X VC0
write_and_read_back 0x42 0x030A 0x00
write_and_read_back 0x42 0x0312 0x0F # Double 8-bit data
write_and_read_back 0x42 0x031C 0x30 # Pipe X BPP=16
write_and_read_back 0x42 0x0316 0x5E # Pipe Y RGB DT
write_and_read_back 0x42 0x0317 0x52 # Pipe Y EMB8 DT
write_and_read_back 0x42 0x030B 0x02 # Pipe Y VC1
write_and_read_back 0x42 0x030C 0x00
write_and_read_back 0x42 0x031D 0x30 # Pipe Y BPP=16
write_and_read_back 0x42 0x0318 0x6A # Pipe Z Y8 DT
write_and_read_back 0x42 0x0319 0x5E # Pipe Z Y8I DT
write_and_read_back 0x42 0x030D 0x04 # Pipe Z VC2
write_and_read_back 0x42 0x030E 0x00
write_and_read_back 0x42 0x031E 0x30 # Pipe Z BPP=16
write_and_read_back 0x42 0x031A 0x6A # Pipe U IMU DT
write_and_read_back 0x42 0x030F 0x08 # Pipe U VC3
write_and_read_back 0x42 0x0310 0x00
write_and_read_back 0x42 0x031F 0x30 # Pipe U BPP=16
write_and_read_back 0x42 0x0315 0xD2 # Enable independent VCs
write_and_read_back 0x42 0x0102 0x0E # LIM_HEART X disabled
write_and_read_back 0x42 0x010A 0x0E # LIM_HEART Y disabled
write_and_read_back 0x42 0x0112 0x0E # LIM_HEART Z disabled
write_and_read_back 0x42 0x011A 0x0E # LIM_HEART U disabled

# HW-SYNC
write_and_read_back 0x42 0x02C1 0x84 # MFP1
write_and_read_back 0x42 0x02C2 0x20 # OUT_TYPE no pullup
write_and_read_back 0x42 0x02C3 0x1F
write_and_read_back 0x42 0x02BE 0x84 # MFP0
write_and_read_back 0x42 0x02BF 0x20 # OUT_TYPE no pullup
write_and_read_back 0x42 0x02C0 0x1B

# Re-enable pipe outputs
write_and_read_back 0x42 0x0002 0xF3 # (VID_TX_EN_X): Disabled | (Default) (VID_TX_EN_Y): Disabled
