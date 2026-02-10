/*
 * StreamView.cpp
 *
 * Copyright © 2019 Intel Corporation. All rights reserved.
 *
 * The source code contained or described herein and all documents related to the source code
 * ("Material") are owned by Intel Corporation or its suppliers or licensors. Title to the
 * Material remains with Intel Corporation or its suppliers and licensors. The Material may
 * contain trade secrets and proprietary and confidential information of Intel Corporation
 * and its suppliers and licensors, and is protected by worldwide copyright and trade secret
 * laws and treaty provisions. No part of the Material may be used, copied, reproduced,
 * modified, published, uploaded, posted, transmitted, distributed, or disclosed in any way
 * without Intel’s prior express written permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual property right
 * is granted to or conferred upon you by disclosure or delivery of the Materials, either
 * expressly, by implication, inducement, estoppel or otherwise. Any license under such
 * intellectual property rights must be express and approved by Intel in writing.
 *
 */

#include <vector>
#include <string>

#include <opencv2/opencv.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "StreamView.h"
#include "Stream.h"
#include "RealsenseLog.h"

using namespace std;
using namespace cv;
using namespace realsense::camera_sub_system;
using namespace realsense::utils;

namespace realsense {
namespace camera_viewer {

StreamView::StreamView(uint8_t nodeNumber,
                       realsense::camera_sub_system::Stream &stream,
                       V4L2Utils::StreamUtils::StreamType st) :
        mNodeNumber {nodeNumber},
        mStreamType {st},
        mStream {stream},
        mFormat {V4L2_PIX_FMT_Z16, 1280, 720, 30}
{
    RS_AUTOLOG();
    mNodeStr = "/dev/video";
    mNodeStr += to_string(mNodeNumber);

    switch (mStreamType) {
    case V4L2Utils::StreamUtils::StreamType::RS_RGB_STREAM:
        mFormat.v4l2Format = V4L2_PIX_FMT_YUYV;
        break;
    case V4L2Utils::StreamUtils::StreamType::RS_DEPTH_STREAM:
        mFormat.v4l2Format = V4L2_PIX_FMT_Z16;
        break;
    case V4L2Utils::StreamUtils::StreamType::RS_Y8_STREAM:
        mFormat.v4l2Format = V4L2_PIX_FMT_GREY;
        break;
    case V4L2Utils::StreamUtils::StreamType::RS_Y12I_STREAM:
        mFormat.v4l2Format = V4L2_PIX_FMT_Y12I;
        break;
    case V4L2Utils::StreamUtils::StreamType::RS_Y8I_STREAM:
        mFormat.v4l2Format = V4L2_PIX_FMT_Y8I;
        break;
    default:
        break;
    }
}

int StreamView::draw()
{
    RS_AUTOLOG();

    namedWindow(mNodeStr.c_str(), WINDOW_NORMAL | WINDOW_KEEPRATIO | WINDOW_GUI_EXPANDED);

    createTrackbar("stream on/off", mNodeStr.c_str(), &mStartStopTBValue, 1, StreamView::startStopTrackbarCB, static_cast<void*>(this));
    switch(mStreamType) {
    case V4L2Utils::StreamUtils::StreamType::RS_Y8_STREAM:
    case V4L2Utils::StreamUtils::StreamType::RS_Y8I_STREAM:
    case V4L2Utils::StreamUtils::StreamType::RS_Y12I_STREAM:
    case V4L2Utils::StreamUtils::StreamType::RS_DEPTH_STREAM:
        {
            // Get current values of ctrls
            bool laserMode;
            int laserValue;
            mStream.getLaserMode(&laserMode);
            mStream.getLaserValue(&laserValue);

            bool ae;
            mStream.getAE(&ae);

            int exp;
            mStream.getExposure(&exp);
            RS_LOGI("exposure %d ++++++++++++++++++++++++++++++", exp);

            int val;
            val = static_cast<int>(ae);
            createTrackbar("ae", mNodeStr.c_str(), &val, 1, StreamView::aETrackbarCB, static_cast<void*>(this));
            createTrackbar("exposure", mNodeStr.c_str(), &exp, 2000, StreamView::exposureTrackbarCB, static_cast<void*>(this));
            val = static_cast<int>(laserMode);
            createTrackbar("laser power mode", mNodeStr.c_str(), &val, 1, StreamView::laserModeTrackbarCB, static_cast<void*>(this));
            laserValue /= 30;
            createTrackbar("laser power value", mNodeStr.c_str(), &laserValue, 12, StreamView::laserValueTrackbarCB, static_cast<void*>(this));
        }
        break;
    case V4L2Utils::StreamUtils::StreamType::RS_RGB_STREAM:
        {
            // Get current values of ctrls
            bool ae;
            mStream.getAE(&ae);

            int exp;
            mStream.getExposure(&exp);
            RS_LOGI("exposure %d ++++++++++++++++++++++++++++++", exp);

            int val;
            val = static_cast<int>(ae);
            createTrackbar("ae", mNodeStr.c_str(), &val, 1, StreamView::aETrackbarCB, static_cast<void*>(this));
            createTrackbar("exposure", mNodeStr.c_str(), &exp, 10000, StreamView::exposureTrackbarCB, static_cast<void*>(this));
        }
        break;
    }

    if (start(V4L2_MEMORY_MMAP) < 0)
        return -1;

    while (!stop_flag) {
        auto index = mStream.getBufferIndex();
        if (index != UINT32_MAX) {
            processCaptureResult(index);
            mStream.returnBufferIndex();
        }
        char key = (char)waitKey(1);
        if (key == 'q' || key == 27)
            break;
    }

    return 0;
}

StreamView::~StreamView()
{
    stop();
}

void StreamView::startStopTrackbarCB (int pos, void *userData) {
    RS_LOGI("pos %d, userdata %p", pos, userData);
    StreamView* sv = static_cast<StreamView*>(userData);
    switch (pos) {
    case 0:
        sv->stop();
        break;
    case 1:
        sv->start(V4L2_MEMORY_MMAP);
        break;
    default:
        RS_LOGE("Invalid value %d", pos);
    }
}

void StreamView::aETrackbarCB (int pos, void *userData) {
    RS_LOGI("pos %d, userdata %p", pos, userData);
    StreamView* sv = static_cast<StreamView*>(userData);
    sv->setAE(pos);
    if (!pos)
        sv->setExposure(getTrackbarPos("exposure", sv->mNodeStr.c_str()));
}

void StreamView::exposureTrackbarCB (int pos, void *userData) {
    RS_LOGI("pos %d, userdata %p", pos, userData);
    StreamView* sv = static_cast<StreamView*>(userData);
    if (getTrackbarPos("ae", sv->mNodeStr.c_str()))
        setTrackbarPos("ae", sv->mNodeStr.c_str(), 0);
    else
        sv->setExposure(pos);
}

void StreamView::laserModeTrackbarCB (int pos, void *userData) {
    RS_LOGI("pos %d, userdata %p", pos, userData);
    StreamView* sv = static_cast<StreamView*>(userData);
    sv->setLaserMode(pos);
    if (pos)
        sv->setLaserValue(30 * getTrackbarPos("laser power value", sv->mNodeStr.c_str()));
}

void StreamView::laserValueTrackbarCB (int pos, void *userData) {
    RS_LOGI("pos %d, userdata %p", pos, userData);
    StreamView* sv = static_cast<StreamView*>(userData);
    if (!getTrackbarPos("laser power mode", sv->mNodeStr.c_str()))
        setTrackbarPos("laser power mode", sv->mNodeStr.c_str(), 1);
    else
        sv->setLaserValue(pos * 30);
}

void StreamView::fpsTrackbarCB (int pos, void *userData) {
    RS_LOGI("pos %d, userdata %p", pos, userData);
    StreamView* sv = static_cast<StreamView*>(userData);
    sv->setFPS(pos);
}

void StreamView::slaveModeTrackbarCB (int pos, void *userData) {
    RS_LOGI("pos %d, userdata %p", pos, userData);
    StreamView* sv = static_cast<StreamView*>(userData);
    sv->setSlaveMode(pos);
}

int StreamView::start(uint32_t memoryType) {
    RS_AUTOLOG();

    mMemoryType = memoryType;

    // Allocate Buffers
    uint32_t bufferLength = mFormat.calcBytesPerFrame();

    mStream.setSlaveMode(mSlaveMode);

    if (mStream.configure(mFormat) < 0) {
        RS_LOGE("Configure failed");
        return -1;
    }

    switch (memoryType) {
    case V4L2_MEMORY_MMAP:
        for (int i = 0; i < mBuffersCount; i++) {
            mRsBuffers.emplace_back(nullptr, bufferLength, i);
        }
        break;
    case V4L2_MEMORY_USERPTR:
        for (int i = 0; i < mBuffersCount; i++) {
            void* ptr = malloc(bufferLength);
            if (ptr == nullptr) {
                RS_LOGE("Failed to allocate buffer %d of size %u", i, bufferLength);
                return -1;
            }
            mRsBuffers.emplace_back(ptr, bufferLength, i);
        }
        break;
    default:
        RS_LOGE("Non supported memory type %d", memoryType);
    }

    for (int i = 0; i < mBuffersCount; i++) {
        RS_LOGI("pushing ptr to buffer %d", mRsBuffers[i].index);
        mRsBuffersPtrs.push_back(&mRsBuffers[i]);
    }

    return mStream.start(mRsBuffersPtrs, memoryType);
}

int StreamView::stop() {
    RS_AUTOLOG();
    stop_flag = true;
    mStream.stop();
    if (V4L2_MEMORY_USERPTR == mMemoryType)
        for (RsBuffer rsBuf : mRsBuffers)
            if (nullptr != rsBuf.buffer)
                free(rsBuf.buffer);
    mRsBuffers.clear();
    mRsBuffersPtrs.clear();
    return 0;
}

void StreamView::processCaptureResult(uint32_t index)
{
    int i = -1;
    for (i = 0; i < mBuffersCount; i++)
        if (mRsBuffersPtrs[i]->index == index)
            break;

    if (i == mBuffersCount)
        return;

    double min;
    double max;
    cv::Mat map;
    cv::Mat rgbMap;
    cv::Mat colorMap;
    cv::Mat histogramOptimizedMap;
    float scale;
    char* ptr;
    uint32_t cnt = 0;
    char* left;
    char* right;
    // Use heap allocation instead of VLA to prevent stack overflow
    uint32_t frameSize = mFormat.calcBytesPerFrame();
    constexpr uint32_t MAX_FRAME_SIZE = 64 * 1024 * 1024; // 64MB max
    if (frameSize == 0 || frameSize > MAX_FRAME_SIZE) {
        RS_LOGE("Invalid frame size: %u", frameSize);
        return;
    }
    std::vector<char> imageVec(frameSize);
    char* image = imageVec.data();
    //lock_guard<mutex> lock(mMutex);
    switch(mStreamType) {
    case V4L2Utils::StreamUtils::StreamType::RS_DEPTH_STREAM:
        map = cv::Mat(mFormat.height, mFormat.width, CV_16UC1, (void*)mRsBuffersPtrs[i]->buffer, mFormat.calc64BytesAlignedStride());
        cv::minMaxIdx(map, &min, &max);
        scale = 255 / (max-min);
        map.convertTo(histogramOptimizedMap, CV_8UC1, scale, -min*scale);
        cv::applyColorMap(histogramOptimizedMap, colorMap, cv::COLORMAP_JET);
        cv::imshow(mNodeStr.c_str(), colorMap);
        break;
    case V4L2Utils::StreamUtils::StreamType::RS_Y8_STREAM:
        map = cv::Mat(mFormat.height, mFormat.width, CV_8UC1, (void*)mRsBuffersPtrs[i]->buffer, mFormat.calc64BytesAlignedStride());
        cv::imshow(mNodeStr.c_str(), map);
        break;
    case V4L2Utils::StreamUtils::StreamType::RS_Y8I_STREAM:
        ptr = (char*)(mRsBuffersPtrs[i]->buffer);
        left = image;
        right = image + mFormat.width;
        for(; ptr < (char*)(mRsBuffersPtrs[i]->buffer) + mRsBuffersPtrs[i]->length - 2; ptr += 2, cnt += 2) {
            *left++ = *ptr;
            *right++ = *(ptr+1);
            if (cnt == mFormat.width * 2) {
                cnt = 0;
                left += mFormat.width;
                right += mFormat.width;
            }
        }

        map = cv::Mat(mFormat.height, mFormat.width*2, CV_8UC1, image, mFormat.calc64BytesAlignedStride());
        cv::imshow(mNodeStr.c_str(), map);
        break;
    case V4L2Utils::StreamUtils::StreamType::RS_Y12I_STREAM:
        ptr = (char*)(mRsBuffersPtrs[i]->buffer);
        left = image;
        right = image + mFormat.width;
        for(; ptr < (char*)(mRsBuffersPtrs[i]->buffer) + mRsBuffersPtrs[i]->length - 4; ptr += 4, cnt += 4) {
            *left++ = (uint8_t)((0xFFF & *(uint16_t *)ptr) / 4096.0 * 255);
            *right++ = (uint8_t)((*(uint16_t *)(ptr+1) >> 4) / 4096.0 * 255);
            if (cnt == mFormat.width * 4) {
                cnt = 0;
                left += mFormat.width;
                right += mFormat.width;
            }
        }

        map = cv::Mat(mFormat.height, mFormat.width * 2, CV_8UC1, image, mFormat.calc64BytesAlignedStride() / 2);
        cv::imshow(mNodeStr.c_str(), map);
        break;
    case V4L2Utils::StreamUtils::StreamType::RS_RGB_STREAM:
        map = cv::Mat(mFormat.height, mFormat.width, CV_8UC2, (void*)mRsBuffersPtrs[i]->buffer, mFormat.calc64BytesAlignedStride());
        cv::cvtColor(map, rgbMap, COLOR_YUV2BGR_UYVY);
        cv::imshow(mNodeStr.c_str(), rgbMap);
        break;
    default:
        break;
    }
}

int StreamView::setAE(bool value) {
    RS_AUTOLOG();
    return mStream.setAE(value);
}

int StreamView::setExposure(int value) {
    RS_AUTOLOG();
    if (value < 1)
        value = 1;
    return mStream.setExposure(value);
}

int StreamView::setLaserMode(bool value) {
    RS_AUTOLOG();
    return mStream.setLaserMode(value);
}

int StreamView::setLaserValue(int value) {
    RS_AUTOLOG();
    return mStream.setLaserValue(value);
}

int StreamView::setFPS(int value) {
    RS_AUTOLOG();
    int ret;
    if ((ret = mStream.setFPS((uint32_t)value)) == 0)
        mFormat.fps = (uint32_t)value;

    return ret;
}

int StreamView::setResolution(uint32_t width, uint32_t height)
{
    mFormat.width = width;
    mFormat.height = height;
    return 0;
}

int StreamView::setSlaveMode(bool value){
    // New slave mode will take effect in the next stream start
    mSlaveMode = value;
    return 0;
}


} // namespace camera_viewer
} // namespace realsense
