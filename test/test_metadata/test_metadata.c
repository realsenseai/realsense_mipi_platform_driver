#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>

extern void process_frame_data(void);

#define SIZE_METADATA_BUFFERS 8
#define NUMBER_OF_IN_ARGUMENTS 9

//Input arguments Default configuration : Depth enable, stream frames for 5 seconds, 640x480 resolution, 30 fps, Number of times stream repeat
volatile static uint16_t inputArgs[8] = {1,0,0,5,640,480,30,1};

volatile static uint16_t Depth_en;              //Depth enable
volatile static uint16_t Color_en;              //Color enable
volatile static uint16_t Ir_en;                //IR enable
volatile static uint16_t stream_frames_time;    //Stream time in seconds
volatile static uint16_t width;                //Width
volatile static uint16_t height;               //Height   
volatile static uint16_t fps;                  //Frames per second
volatile static uint16_t stream_repeat;        // Number of times strem repeat (start and stop)

struct timespec current_time,stream_start, first_frame;
static uint32_t Number_of_frames_collected = 0;

////////////////////////
//Intel Configuration //
////////////////////////

typedef union STTriggerMode
{
    uint16_t value;
    struct
    {
        uint16_t  inSync      :1;
        uint16_t  extTrigger  :1;
        uint16_t  reserved   :14;
    }fields;
}STTriggerMode;


////////////////////////////
//       SUB preset       //
////////////////////////////
typedef union STSubPresetInfo
{
    uint32_t value;
    struct
    {
	uint32_t  id		:4;	// -
	uint32_t  numOfItems	:8;	// - according to SubPresetTool ver 2.0.0
	uint32_t  itemIndex	:8;	// -
	uint32_t  iteration	:6;	// - for debug purposes
	uint32_t  itemIteration	:6;	// - for debug purposes
    };
}STSubPresetInfo;


////////////////////////////
//    NEW DEPTH STRUCT    //
////////////////////////////
typedef struct
{
    uint32_t    res[3];
    uint32_t    Frame_counter;
    uint32_t    metaDataID;
    uint32_t    size;
    uint8_t     version;
    uint16_t    calibInfo;          // OCC/TAR calibration info: frame counter, 0xffff is default/inactive value
    uint8_t     reserved[1];
    uint32_t    flags;
    uint32_t    hwTimestamp;        // Register: RegVdfStcCount
    uint32_t    opticalTimestamp;   // ExposureTime/2 in millisecond unit
    uint32_t    exposureTime;
    uint32_t    manualExposure;     //Manual exposure
    uint16_t    laserPower ;        //Laser power value
    STTriggerMode   trigger;         /* Byte <0> ? 0 free-running
                                                 1 in sync
                                                 2 external trigger (depth only)
                                      Byte <1> ? configured delay (depth only) */
    uint8_t     projectorMode;
    uint8_t     preset;
    uint8_t     manualGain;         //Manual gain value
    uint8_t     autoExposureMode;   //AE mode
    uint16_t    inputWidth;
    uint16_t    inputHeight;
    STSubPresetInfo subpresetInfo;
    uint32_t    crc32;
} __attribute__((packed))STMetaDataExtMipiDepthIR;


static int streamFrames(int videoNode_fd, int md_fd, void* metaDataBuffers[SIZE_METADATA_BUFFERS], int lastStream, char *stream_type)
{
    uint8_t FirstFrameArr = 1;
    uint8_t first_write = 1;
    Number_of_frames_collected = 0;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    struct timespec now;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - current_time.tv_sec) + (now.tv_nsec - current_time.tv_nsec) / 1e9;
        if (elapsed >= stream_frames_time)
            break;
        struct v4l2_buffer realsenseV4l2Buffer = { 0 };
        realsenseV4l2Buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        realsenseV4l2Buffer.memory = V4L2_MEMORY_MMAP;
        struct v4l2_buffer mdV4l2Buffer = { 0 };
        mdV4l2Buffer.type = V4L2_BUF_TYPE_META_CAPTURE;
        mdV4l2Buffer.memory = V4L2_MEMORY_MMAP;

        int ret = ioctl(videoNode_fd, VIDIOC_DQBUF, &realsenseV4l2Buffer);
        if (ret < 0) {
            fprintf(stderr, "Error dequeuing depth buffer: %s\n", strerror(errno));
            return -1;
        }
        ret = ioctl(md_fd, VIDIOC_DQBUF, &mdV4l2Buffer);
        if (ret < 0) {
            fprintf(stderr, "Error dequeuing metadata buffer: %s\n", strerror(errno));
            return -1;
        }

        FILE *f;
        if (first_write) {
            f = fopen("frames.txt", "w"); // Truncate on first write
            first_write = 0;
        } else {
            f = fopen("frames.txt", "a"); // Append for subsequent writes
        }

        if (FirstFrameArr)
        {
            FirstFrameArr = 0;
            clock_gettime(CLOCK_MONOTONIC, &first_frame);
            if (f) {
            fprintf(f, "\nrealsense Stream start time: %lluns\n", (unsigned long long)stream_start.tv_nsec);
            fprintf(f, "realsense First frame time : %lluns\n", (unsigned long long)first_frame.tv_nsec);
            fprintf(f, "realsense Elapsed_Time: %lluns (%lluus)\n\n", (unsigned long long)(first_frame.tv_nsec - stream_start.tv_nsec), (unsigned long long)(first_frame.tv_nsec - stream_start.tv_nsec)/1000);
            } else {
            fprintf(stderr, "Error opening frames.txt for writing\n");
        }  
        }

        STMetaDataExtMipiDepthIR *ptr = (STMetaDataExtMipiDepthIR*)metaDataBuffers[mdV4l2Buffer.index];
        //printf("BUFFER_IDX %d, Depth, %u, %llu, %u\n", mdV4l2Buffer.index, ptr->Frame_counter, (unsigned long long)ptr->hwTimestamp, ptr->crc32);
        if (f) {
            fprintf(f, "BUFFER_IDX %d, %s, %u, %llu, %u\n", mdV4l2Buffer.index, stream_type, ptr->Frame_counter, (unsigned long long)ptr->hwTimestamp, ptr->crc32);
        } else {
            fprintf(stderr, "Error opening frames.txt for writing\n");
        }

        Number_of_frames_collected++;

        if (ioctl(videoNode_fd, VIDIOC_QBUF, &realsenseV4l2Buffer) < 0) {
            fprintf(stderr, "Error queuing depth buffer: %s\n", strerror(errno));
            return -1;
        }
        if (ioctl(md_fd, VIDIOC_QBUF, &mdV4l2Buffer) < 0) {
            fprintf(stderr, "Error queuing metadata buffer: %s\n", strerror(errno));
            return -1;
        }
        if (lastStream) {
            break;
        }
        fclose(f);
    }
    printf("Total Number of frames collected: %u\n",Number_of_frames_collected);
    return 0;
}

void setFmt(int fd, uint32_t pxlFmt, uint32_t w, uint32_t h)
{
    struct v4l2_format v4l2Format;
    memset(&v4l2Format, 0, sizeof(v4l2Format));
    v4l2Format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2Format.fmt.pix.pixelformat = pxlFmt;
    v4l2Format.fmt.pix.width = w;
    v4l2Format.fmt.pix.height = h;
    int ret = ioctl(fd, VIDIOC_S_FMT, &v4l2Format);
    if (ret < 0) {
        fprintf(stderr, "Error setting format: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void setFPS(int fd, uint32_t fps)
{
    struct v4l2_streamparm setFps;
    memset(&setFps, 0, sizeof(setFps));
    setFps.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    setFps.parm.capture.timeperframe.numerator = 1;
    setFps.parm.capture.timeperframe.denominator = fps;
    int ret = ioctl(fd, VIDIOC_S_PARM, &setFps);
    if (ret < 0) {
        fprintf(stderr, "Error setting FPS: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void requestBuffers(int fd, uint32_t type, uint32_t memory, uint32_t count)
{
    struct v4l2_requestbuffers v4L2ReqBufferrs;
    memset(&v4L2ReqBufferrs, 0, sizeof(v4L2ReqBufferrs));
    v4L2ReqBufferrs.type = type;
    v4L2ReqBufferrs.memory = memory;
    v4L2ReqBufferrs.count = count;
    int ret = ioctl(fd, VIDIOC_REQBUFS, &v4L2ReqBufferrs);
    if (ret < 0) {
        fprintf(stderr, "Error requesting buffers: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void* queryMapQueueBuf(int fd, uint32_t type, uint32_t memory, uint8_t index, uint32_t size)
{
    struct v4l2_buffer v4l2Buffer;
    memset(&v4l2Buffer, 0, sizeof(v4l2Buffer));
    v4l2Buffer.type = type;
    v4l2Buffer.memory = memory;
    v4l2Buffer.index = index;
    int ret = ioctl(fd, VIDIOC_QUERYBUF, &v4l2Buffer);
    if (ret)
        return NULL;
    void* buffer = mmap(NULL,
        size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        v4l2Buffer.m.offset);
    if (buffer == MAP_FAILED)
        return NULL;
    ret = ioctl(fd, VIDIOC_QBUF, &v4l2Buffer);
    if (ret)
        return NULL;
    return buffer;
}

int main(int argc, char** argv) {
    static int video_fd, md_fd;
    static const char* mdVideoNode;
    enum v4l2_buf_type vType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    enum v4l2_buf_type mdType = V4L2_BUF_TYPE_META_CAPTURE;
    uint16_t temp_strem_inputvalue = 0;

    if (argc < NUMBER_OF_IN_ARGUMENTS) {
        fprintf(stderr, "Usage: %s <arg1> <arg2> <arg3> <stream_time_seconds> <width> <height> <fps>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    for (int i = 1; i < NUMBER_OF_IN_ARGUMENTS; ++i) {
        if (argv[i] == NULL || argv[i][0] == '\0') {
            fprintf(stderr, "Warning: Input Argument %d is NULL or empty. Hence assuming default configuration %d\n", i, inputArgs[i-1]);
        }
        else inputArgs[i-1] = atoi(argv[i]);
    }

    Depth_en           = inputArgs[0];
    Color_en           = inputArgs[1];
    Ir_en              = inputArgs[2];
    stream_frames_time = inputArgs[3];
    width              = inputArgs[4];
    height             = inputArgs[5];
    fps                = inputArgs[6];
    stream_repeat      = inputArgs[7];

    printf("\nTest started: Number of iterations of stream start and stop is %u",stream_repeat);
    printf("\nrealsense:    RESOLUTION: %u*%u,    FPS: %u\n", width,height,fps);
    temp_strem_inputvalue = stream_repeat;

    while (stream_repeat) {
        if(Depth_en) {
            printf("\n/**************Streaming depth frames start: iteration:%u **************/\n",temp_strem_inputvalue-stream_repeat);
            mdVideoNode = "/dev/video0";
            video_fd = open(mdVideoNode, O_RDWR);
            mdVideoNode = "/dev/video1";
            md_fd = open(mdVideoNode, O_RDWR);
            if (video_fd < 0 || md_fd < 0) {
                fprintf(stderr, "Error opening depth video devices\n");
            return 1;
            }
            setFmt(video_fd, V4L2_PIX_FMT_Z16, width, height);
            setFPS(video_fd, fps);
            requestBuffers(video_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, SIZE_METADATA_BUFFERS);
            requestBuffers(md_fd, V4L2_BUF_TYPE_META_CAPTURE, V4L2_MEMORY_MMAP, SIZE_METADATA_BUFFERS);

            void* depthBuffers[SIZE_METADATA_BUFFERS] = {0};
            void* metaDataBuffers[SIZE_METADATA_BUFFERS] = {0};
            for (int i = 0; i < SIZE_METADATA_BUFFERS; ++i) {
                depthBuffers[i] = queryMapQueueBuf(video_fd,
                    V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    V4L2_MEMORY_MMAP,
                    i,
                    2 * width * height);
                metaDataBuffers[i] = queryMapQueueBuf(md_fd,
                    V4L2_BUF_TYPE_META_CAPTURE,
                    V4L2_MEMORY_MMAP,
                    i,
                    4096);
            }

            int ret = ioctl(md_fd, VIDIOC_STREAMON, &mdType);
            if (ret < 0) {
                fprintf(stderr, "Error starting metadata stream: %s\n", strerror(errno));
                return 1;
            }
            ret = ioctl(video_fd, VIDIOC_STREAMON, &vType);
            if (ret < 0) {
                fprintf(stderr, "Error starting depth stream: %s\n", strerror(errno));
                return 1;
            }
            
            clock_gettime(CLOCK_MONOTONIC, &stream_start);
            streamFrames(video_fd, md_fd, metaDataBuffers, 0, "depth");
            process_frame_data();      
            for (int i = 0; i < SIZE_METADATA_BUFFERS; i++) {
                munmap(depthBuffers[i], 2 * width * height);
                munmap(metaDataBuffers[i], 4096);
            }
            ret = ioctl(md_fd, VIDIOC_STREAMOFF, &mdType);
            if (ret < 0) {
                fprintf(stderr, "Error stopping metadata stream: %s\n", strerror(errno));
            }
            ret = ioctl(video_fd, VIDIOC_STREAMOFF, &vType);
            if (ret < 0) {
                fprintf(stderr, "Error stopping depth stream: %s\n", strerror(errno));
            }
            close(md_fd);
            close(video_fd);
            printf("/**************Streaming depth frames End:   iteration:%u **************/\n",temp_strem_inputvalue-stream_repeat);           
        }

        if(Color_en) {
            printf("\n/**************Streaming color frames start: iteration:%u **************/\n",temp_strem_inputvalue-stream_repeat);
            mdVideoNode = "/dev/video2";
            video_fd = open(mdVideoNode, O_RDWR);
            mdVideoNode = "/dev/video3";
            md_fd = open(mdVideoNode, O_RDWR);
            if (video_fd < 0 || md_fd < 0) {
                fprintf(stderr, "Error opening color video devices\n");
                return 1;
            }
            setFmt(video_fd, V4L2_PIX_FMT_YUYV, width, height);
            setFPS(video_fd, fps);
            requestBuffers(video_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, SIZE_METADATA_BUFFERS);
            requestBuffers(md_fd, V4L2_BUF_TYPE_META_CAPTURE, V4L2_MEMORY_MMAP, SIZE_METADATA_BUFFERS); 

            void* depthBuffers[SIZE_METADATA_BUFFERS] = {0};
            void* metaDataBuffers[SIZE_METADATA_BUFFERS] = {0};
            for (int i = 0; i < SIZE_METADATA_BUFFERS; ++i) {
                depthBuffers[i] = queryMapQueueBuf(video_fd,
                    V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    V4L2_MEMORY_MMAP,
                    i,
                    2 * width * height);
                metaDataBuffers[i] = queryMapQueueBuf(md_fd,
                    V4L2_BUF_TYPE_META_CAPTURE,
                    V4L2_MEMORY_MMAP,
                    i,
                    4096);
            }

            int ret = ioctl(md_fd, VIDIOC_STREAMON, &mdType);
            if (ret < 0) {
                fprintf(stderr, "Error starting color metadata stream: %s\n", strerror(errno));
                return 1;
            }
            ret = ioctl(video_fd, VIDIOC_STREAMON, &vType);
            if (ret < 0) {
                fprintf(stderr, "Error starting color stream: %s\n", strerror(errno));
                return 1;
            }
            
            clock_gettime(CLOCK_MONOTONIC, &stream_start);
            streamFrames(video_fd, md_fd, metaDataBuffers, 0, "color");
            process_frame_data();      
            for (int i = 0; i < SIZE_METADATA_BUFFERS; i++) {
                munmap(depthBuffers[i], 2 * width * height);
                munmap(metaDataBuffers[i], 4096);
            }
            ret = ioctl(md_fd, VIDIOC_STREAMOFF, &mdType);
            if (ret < 0) {
                fprintf(stderr, "Error stopping color metadata stream: %s\n", strerror(errno));
            }
            ret = ioctl(video_fd, VIDIOC_STREAMOFF, &vType);
            if (ret < 0) {
                fprintf(stderr, "Error stopping color stream: %s\n", strerror(errno));
            }
            close(md_fd);
            close(video_fd);    
            printf("/**************Streaming color frames End:   iteration:%u **************/\n",temp_strem_inputvalue-stream_repeat);           
        }

        if(Ir_en) {    
            printf("\n/**************Streaming IR frames start: iteration:%u *****************/\n",temp_strem_inputvalue-stream_repeat);
            mdVideoNode = "/dev/video4";
            video_fd = open(mdVideoNode, O_RDWR);
            mdVideoNode = "/dev/video5";
            md_fd = open(mdVideoNode, O_RDWR);
            if (video_fd < 0 || md_fd < 0) {
                fprintf(stderr, "Error opening Ir video devices\n");
                return 1;
            }
            setFmt(video_fd, V4L2_PIX_FMT_GREY, width, height);
            setFPS(video_fd, fps);
            requestBuffers(video_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, SIZE_METADATA_BUFFERS);
            requestBuffers(md_fd, V4L2_BUF_TYPE_META_CAPTURE, V4L2_MEMORY_MMAP, SIZE_METADATA_BUFFERS);      
            void* depthBuffers[SIZE_METADATA_BUFFERS] = {0};
            void* metaDataBuffers[SIZE_METADATA_BUFFERS] = {0};
            for (int i = 0; i < SIZE_METADATA_BUFFERS; ++i) {
                depthBuffers[i] = queryMapQueueBuf(video_fd,
                    V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    V4L2_MEMORY_MMAP,
                    i,
                    width * height);
                metaDataBuffers[i] = queryMapQueueBuf(md_fd,
                    V4L2_BUF_TYPE_META_CAPTURE,
                    V4L2_MEMORY_MMAP,
                    i,
                    4096);
            }

            int ret = ioctl(md_fd, VIDIOC_STREAMON, &mdType);
            if (ret < 0) {
                fprintf(stderr, "Error starting ir metadata stream: %s\n", strerror(errno));
                return 1;
            }
            ret = ioctl(video_fd, VIDIOC_STREAMON, &vType);
            if (ret < 0) {
                fprintf(stderr, "Error starting ir stream: %s\n", strerror(errno));
                return 1;
            }
            
            clock_gettime(CLOCK_MONOTONIC, &stream_start);
            streamFrames(video_fd, md_fd, metaDataBuffers, 0, "IR");
            process_frame_data();      
            for (int i = 0; i < SIZE_METADATA_BUFFERS; i++) {
                munmap(depthBuffers[i], width * height);
                munmap(metaDataBuffers[i], 4096);
            }
            ret = ioctl(md_fd, VIDIOC_STREAMOFF, &mdType);
            if (ret < 0) {
                fprintf(stderr, "Error stopping ir metadata stream: %s\n", strerror(errno));
            }
            ret = ioctl(video_fd, VIDIOC_STREAMOFF, &vType);
            if (ret < 0) {
                fprintf(stderr, "Error stopping ir stream: %s\n", strerror(errno));
            }
            close(md_fd);
            close(video_fd);      
            printf("/**************Streaming IR frames End:   iteration:%u *****************/\n",temp_strem_inputvalue-stream_repeat);
        }
    stream_repeat--;
    }

    if(!stream_repeat)
        printf("\nNumber of stream start and stop %u iterations completed successfully \n",temp_strem_inputvalue);
    else
        printf("\nstream start and stop abrotpted at %u iteration\n",stream_repeat);
    return 0;
}
