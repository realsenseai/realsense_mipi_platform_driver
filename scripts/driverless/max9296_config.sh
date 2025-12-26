#!/bin/bash

function write_and_read_back() {
	addr_msb=$(($2 >> 8))
	addr_lsb=$(($2 & 0xff))

	sudo i2ctransfer -y 2 w3@$1 $addr_msb $addr_lsb $3
	result="$(sudo i2ctransfer -y 2 w2@$1 $addr_msb $addr_lsb r1@$1)"
	echo $2 $result
}

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
