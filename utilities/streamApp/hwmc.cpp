// this program gets HMC (HW Monitor Command) parameters from command line and sends
// the info to the camera i2c driver to encapsulate and send to device/FW.
// At this point, we support only non-data commands i.e. only op_code and param 
// note, values can be decimal or hex e.g. 0x80 is supported 
#include <iostream>
#include <vector>
#include <cstring>
#include <iomanip>
#include <algorithm>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "ScopedFileDescriptor.h"

using namespace std;
using namespace realsense::utils;

#define DS5_STREAM_CONFIG_0	0x4000
#define DS5_CAMERA_CID_BASE	(V4L2_CTRL_CLASS_CAMERA | DS5_STREAM_CONFIG_0)
#define DS5_CAMERA_CID_HWMC	(DS5_CAMERA_CID_BASE+15)

#pragma pack(push, 1)
struct HWMC {
	HWMC(const vector<int32_t> &inParams):header(0x14), magic_word(0xCDAB), opcode(0) {
		if (inParams.empty()) {
			cerr << "Error: HWMC requires at least an opcode" << endl;
			return;
		}
		opcode = inParams[0];
		memset(params, 0, sizeof(params));
		size_t paramCount = std::min(inParams.size() - 1, sizeof(params)/sizeof(params[0]));
		for (size_t i = 0; i < paramCount; i++)
			params[i] = inParams[i + 1];
	}
	uint16_t header = 0x14;
	uint16_t magic_word = 0xCDAB;
	uint32_t opcode;
	uint32_t params[4];
};
#pragma pack(pop)

vector<int32_t> getParams(char *input[], int numOfInputs) {
	vector<int32_t> params;
	for (auto i = 0; i < numOfInputs; i++) {
		string val(input[i]);
		// we support input of hex and decimal
		if (val.find("0x") == 0)
			params.push_back(strtol(val.substr(2).c_str(), nullptr, 16));
		else
			params.push_back(strtol(val.c_str(), nullptr, 10));
	}

	return params;
}

int main(int argc, char *argv[]) {
	if (argc < 2 || argc > 6) {
		cout << "usage: ./hmc_utility <opcode> [param1] [param2] [param3] [param4]\n";
		return -1;
	}

	vector<int32_t> args = getParams(&argv[1], argc - 1);

	// now issue ioctl on /dev/video0 with relevant params...
	struct HWMC hmc(args);

	ScopedFileDescriptor fd{0};
	if (!fd)
		return -1;

	uint8_t hwmcBuff[1024 + sizeof(struct HWMC)] {0};
	memset(hwmcBuff, 0, sizeof(hwmcBuff));

	struct v4l2_ext_control ctrl {0};
	ctrl.id = DS5_CAMERA_CID_HWMC;
	ctrl.size = sizeof(hwmcBuff);
	ctrl.p_u8 = hwmcBuff;

	struct v4l2_ext_controls ext {0};
	ext.ctrl_class = V4L2_CTRL_CLASS_CAMERA;
	ext.controls = &ctrl;
	ext.count = 1;

	memcpy(hwmcBuff, &hmc, sizeof(struct HWMC));
	int ret = ioctl(fd.get(), VIDIOC_S_EXT_CTRLS, &ext);
        if (ret) {
            cerr << "ioctl VIDIOC_S_EXT_CTRLS failed" << std::endl;
            return -1;
        }
	if (hmc.opcode == *(hwmcBuff + sizeof(struct HWMC))) {
		uint16_t outLen = hwmcBuff[1001 + sizeof(struct HWMC)] << 8;
		outLen |= hwmcBuff[1000 + sizeof(struct HWMC)];
		cout << "output length: "<< outLen << endl;;
		// Bounds check: ensure we don't read past buffer
		uint16_t maxLen = sizeof(hwmcBuff) - sizeof(struct HWMC) - 4;
		if (outLen > maxLen)
			outLen = maxLen;
		for (int i = 0; i < outLen; ++i) {
			if (i != 0 && 0 == (i % 16))
				cout << endl;
			cout << hex << std::setfill('0') << std::setw(2) << static_cast<uint16_t>(hwmcBuff[i + sizeof(struct HWMC) + 4]) << " ";
		}
	} else {
		cout << "failed" << endl;;
	}

	cout << endl;

	return 0;
}
