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

################## Deserializer (max9296) configuration ##################
echo "=======Configuring Deserializer MAX9296:======="
write_and_read_back 0x48 0x0302 0x10 # Increase CMU regulator voltage
write_and_read_back 0x48 0x0313 0x00 # (CSI_OUT_EN): CSI output disabled
# Phy optimization
write_and_read_back 0x48 0x1458 0x28 # PHY A Optimization
write_and_read_back 0x48 0x1459 0x68 # PHY A Optimization
write_and_read_back 0x48 0x1558 0x28 # PHY B Optimization
write_and_read_back 0x48 0x1559 0x68 # PHY B Optimization
write_and_read_back 0x48 0x0050 0x00 # (STR_SELX): 0x0
write_and_read_back 0x48 0x0051 0x01 # (STR_SELY): 0x1

# MAX9296A Setup
write_and_read_back 0x48 0x044A 0x50 # 4 lanes on port A // Write 0x50 for 2 lanes
write_and_read_back 0x48 0x0320 0x2F # 1500 Mbps/lane on port A
write_and_read_back 0x48 0x0473 0x10 # Un-double 8-bit data, Enable ALT2_MEM_MAP8
write_and_read_back 0x48 0x040B 0x0F # Enable 4 mappings for Pipe X
write_and_read_back 0x48 0x040D 0x1E # Map Depth VC0
write_and_read_back 0x48 0x040E 0x1E # —
write_and_read_back 0x48 0x040F 0x00 # Map frame start VC0
write_and_read_back 0x48 0x0410 0x00 # —
write_and_read_back 0x48 0x0411 0x01 # Map frame end VC0
write_and_read_back 0x48 0x0412 0x01 # —
write_and_read_back 0x48 0x0413 0x12 # Map EMB8 VC0
write_and_read_back 0x48 0x0414 0x12 # —
write_and_read_back 0x48 0x042D 0x55 # All mappings to PHY1 (master for port A)
write_and_read_back 0x48 0x044B 0x0F # Enable 4 mappings for Pipe Y
write_and_read_back 0x48 0x044D 0x5E # Map RGB VC1
write_and_read_back 0x48 0x044E 0x5E # —
write_and_read_back 0x48 0x044F 0x40 # Map frame start VC1
write_and_read_back 0x48 0x0450 0x40 # —
write_and_read_back 0x48 0x0451 0x41 # Map frame end VC1
write_and_read_back 0x48 0x0452 0x41 # —
write_and_read_back 0x48 0x0453 0x52 # Map EMB8 VC1
write_and_read_back 0x48 0x0454 0x52 # —
write_and_read_back 0x48 0x046D 0x55 # All mappings to PHY1 (master for port A)
write_and_read_back 0x48 0x048B 0x0F # Enable 4 mappings for Pipe Z
write_and_read_back 0x48 0x048D 0xAA # Map Y8 VC2
write_and_read_back 0x48 0x048E 0xAA # —
write_and_read_back 0x48 0x048F 0x80 # Map frame start VC2
write_and_read_back 0x48 0x0490 0x80 # —
write_and_read_back 0x48 0x0491 0x81 # Map frame end VC2
write_and_read_back 0x48 0x0492 0x81 # —
write_and_read_back 0x48 0x0493 0x9E # Map Y8I VC2
write_and_read_back 0x48 0x0494 0x9E # —
write_and_read_back 0x48 0x04AD 0x55 # Map to PHY1 (master for port A)
write_and_read_back 0x48 0x04CB 0x07 # Enable 3 mappings for Pipe U
write_and_read_back 0x48 0x04CD 0xEA # Map IMU VC3
write_and_read_back 0x48 0x04CE 0xEA # —
write_and_read_back 0x48 0x04CF 0xC0 # Map frame start VC3
write_and_read_back 0x48 0x04D0 0xC0 # —
write_and_read_back 0x48 0x04D1 0xC1 # Map frame end VC3
write_and_read_back 0x48 0x04D2 0xC1 # —
write_and_read_back 0x48 0x04ED 0x15 # Map to PHY1 (master for port A)

# VID RX configuration
write_and_read_back 0x48 0x0100 0x23 # SEQ_MISS_EN Pipe X: Disabled / DIS_PKT_DET Pipe X: Disabled
write_and_read_back 0x48 0x0112 0x23 # SEQ_MISS_EN Pipe Y: Disabled / DIS_PKT_DET Pipe Y: Disabled
write_and_read_back 0x48 0x0124 0x23 # SEQ_MISS_EN Pipe Z: Disabled / DIS_PKT_DET Pipe Z: Disabled
write_and_read_back 0x48 0x0136 0x23 # SEQ_MISS_EN Pipe U: Disabled / DIS_PKT_DET Pipe U: Disabled

# HW-SYNC routing
write_and_read_back 0x48 0x02C5 0x82 # MFP7 (HW SYNC Depth trigger path)
write_and_read_back 0x48 0x02C6 0x1F # —
write_and_read_back 0x48 0x02CB 0x82 # MFP9 (HW SYNC RGB trigger path)
write_and_read_back 0x48 0x02CC 0x1B # —

# Re-enable CSI output
write_and_read_back 0x48 0x0313 0x02 # (CSI_OUT_EN): CSI output enabled
echo "One-shot reset..."
# Cannot do write_and_read_back when the write is a reset, using regular transfer
sudo i2ctransfer -y 2 w3@0x48 0x00 0x10 0x31 # One-shot reset enable auto-link
sleep 0.1

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
