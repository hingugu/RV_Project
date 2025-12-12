#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define CAMERA_DEV "/dev/video0"
#define ENCODER_DEV "/dev/video11" // 라즈베리 파이 HW 인코더
#define WIDTH 640
#define HEIGHT 480
#define FRAME_COUNT 300 // 10초 녹화 (30fps * 10)

using namespace std;

struct Buffer {
    void* start;
    size_t length;
};

// MPLANE 타입인지 확인하는 헬퍼 함수
bool is_mplane(uint32_t type) {
    return (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
        type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
}

// 포맷 설정 함수 (Single & Multi Plane 모두 지원하도록 수정됨)
int set_format(int fd, uint32_t type, uint32_t pixelformat, int w, int h) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;

    if (is_mplane(type)) {
        // [인코더용] Multi-Planar 설정
        fmt.fmt.pix_mp.width = w;
        fmt.fmt.pix_mp.height = h;
        fmt.fmt.pix_mp.pixelformat = pixelformat;
        fmt.fmt.pix_mp.num_planes = 1;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

        // H.264 출력 버퍼 크기 지정 (넉넉하게 512KB)
        if (pixelformat == V4L2_PIX_FMT_H264) {
            fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 512 * 1024;
        }
    }
    else {
        // [카메라용] Single-Planar 설정
        fmt.fmt.pix.width = w;
        fmt.fmt.pix.height = h;
        fmt.fmt.pix.pixelformat = pixelformat;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
    }

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("VIDIOC_S_FMT");
        return -1;
    }
    return 0;
}

// 버퍼 요청 및 맵핑 함수 (MPLANE 대응 추가)
int request_buffers(int fd, uint32_t type, Buffer* buffers) {
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }

    struct v4l2_buffer buf;
    struct v4l2_plane planes[1]; // MPLANE용 추가 구조체
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;

    if (is_mplane(type)) {
        buf.m.planes = planes;
        buf.length = 1; // plane 개수
    }

    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
        perror("VIDIOC_QUERYBUF");
        return -1;
    }

    if (is_mplane(type)) {
        // [인코더용] MPLANE 방식 맵핑
        buffers[0].length = buf.m.planes[0].length;
        buffers[0].start = mmap(NULL, buf.m.planes[0].length,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd,
            buf.m.planes[0].m.mem_offset);
    }
    else {
        // [카메라용] 일반 방식 맵핑
        buffers[0].length = buf.length;
        buffers[0].start = mmap(NULL, buf.length,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd,
            buf.m.offset);
    }

    if (buffers[0].start == MAP_FAILED) {
        perror("mmap");
        return -1;
    }
    return 0;
}

// 큐잉 헬퍼 (QBUF) - 타임스탬프 전달 기능 포함
int queue_buffer(int fd, uint32_t type, int index, int bytesused, struct timeval timestamp) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;

    if (is_mplane(type)) {
        buf.m.planes = planes;
        buf.length = 1;
        planes[0].bytesused = bytesused;
    }
    else {
        buf.bytesused = bytesused;
    }

    buf.timestamp = timestamp; // 싱크를 위해 타임스탬프 전달

    if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
        perror("VIDIOC_QBUF");
        return -1;
    }
    return 0;
}

int main() {
    int cam_fd = open(CAMERA_DEV, O_RDWR);
    int enc_fd = open(ENCODER_DEV, O_RDWR);

    if (cam_fd == -1 || enc_fd == -1) {
        cerr << "장치 열기 실패! (카메라나 인코더 연결 확인)" << endl;
        return 1;
    }

    // 1. 포맷 설정
    // 카메라는 일반(CAPTURE), 인코더는 MPLANE(_MPLANE) 타입을 써야 함 (중요!)
    if (set_format(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_YUV420, WIDTH, HEIGHT) < 0) return 1;

    // 인코더 입구(OUTPUT_MPLANE): YUV420 받기
    if (set_format(enc_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_YUV420, WIDTH, HEIGHT) < 0) return 1;

    // 인코더 출구(CAPTURE_MPLANE): H.264 뱉기
    if (set_format(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_H264, WIDTH, HEIGHT) < 0) return 1;

    // 인코더 프레임레이트 (30fps) 설정
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = 30;
    ioctl(enc_fd, VIDIOC_S_PARM, &parm);

    // 2. 버퍼 요청 및 맵핑
    Buffer cam_buf, enc_in_buf, enc_out_buf;
    if (request_buffers(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &cam_buf) < 0) return 1;
    if (request_buffers(enc_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &enc_in_buf) < 0) return 1;
    if (request_buffers(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &enc_out_buf) < 0) return 1;

    cout << "장치 및 버퍼 설정 완료 (MPLANE 적용됨)" << endl;

    // 3. 스트리밍 시작
    enum v4l2_buf_type type;

    // 카메라 ON
    queue_buffer(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0, 0, { 0,0 });
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam_fd, VIDIOC_STREAMON, &type);

    // 인코더 ON (출구 큐잉 -> 입구 ON -> 출구 ON 순서)
    queue_buffer(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0, 0, { 0,0 }); // 결과 담을 그릇 미리 넣기

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ioctl(enc_fd, VIDIOC_STREAMON, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(enc_fd, VIDIOC_STREAMON, &type);

    cout << "녹화 시작 (H.264 인코딩 중...)" << endl;

    int file_fd = open("output.h264", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1]; // 인코더 결과 받을 때 필요

    for (int i = 0; i < FRAME_COUNT; i++) {
        // [A] 카메라에서 YUV 한 프레임 꺼내기
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cam_fd, VIDIOC_DQBUF, &buf) == -1) {
            perror("Cam DQBUF"); break;
        }

        // [B] 데이터 복사 (Cam -> Encoder Input)
        // 카메라 버퍼 데이터를 인코더 입력 버퍼로 복사
        memcpy(enc_in_buf.start, cam_buf.start, buf.bytesused);

        // [C] 인코더에 넣기 (MPLANE 타입으로 QBUF)
        // 카메라에서 받은 타임스탬프를 그대로 전달
        queue_buffer(enc_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0, buf.bytesused, buf.timestamp);

        // [D] 인코더에서 결과 꺼내기 (DQBUF)
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = 1;

        // 인코더가 압축을 마칠 때까지 대기 (Blocking call)
        if (ioctl(enc_fd, VIDIOC_DQBUF, &buf) == 0) {
            // [E] 파일 저장 (압축된 데이터 크기는 planes[0].bytesused에 있음)
            int size = buf.m.planes[0].bytesused;
            write(file_fd, enc_out_buf.start, size);
            cout << "\r>> 인코딩: " << i << "/" << FRAME_COUNT << " (" << size << " bytes)   " << flush;

            // [F] 인코더 출구 반납 (다음 프레임을 위해)
            queue_buffer(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0, 0, { 0,0 });
        }

        // [G] 인코더 입구 반납 확인 (다 쓴 원본 그릇 돌려받기)
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = 1;
        ioctl(enc_fd, VIDIOC_DQBUF, &buf);

        // [H] 카메라 버퍼 반납 (카메라가 다음 장면 찍도록)
        queue_buffer(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0, 0, { 0,0 });
    }

    cout << "\n완료! output.h264 저장됨." << endl;

    close(file_fd);
    close(cam_fd);
    close(enc_fd);
    return 0;
}