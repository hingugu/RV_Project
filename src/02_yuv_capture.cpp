#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define CAMERA_DEV "/dev/video0"
#define WIDTH 640
#define HEIGHT 480

using namespace std;

// 버퍼 정보를 담을 구조체
struct Buffer {
    void* start;
    size_t length;
};

int main() {
    // 1. 카메라 열기
    int fd = open(CAMERA_DEV, O_RDWR);
    if (fd == -1) {
        cerr << "카메라 열기 실패" << endl;
        return 1;
    }

    // 2. 포맷 설정 (640x480 YUV420)
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = WIDTH;
    fmt.fmt.pix.height = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420; // YUV 포맷 지정
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        cerr << "포맷 설정 실패" << endl;
        close(fd);
        return 1;
    }
    cout << "포맷 설정 완료: " << WIDTH << "x" << HEIGHT << " YUV420" << endl;

    // 3. 버퍼 요청 (메모리 맵핑용)
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 1; // 버퍼 1개만 사용
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        cerr << "버퍼 요청 실패" << endl;
        close(fd);
        return 1;
    }

    // 4. 메모리 맵핑 (커널 공간 -> 유저 공간)
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;

    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
        cerr << "버퍼 정보 조회 실패" << endl;
        close(fd);
        return 1;
    }

    Buffer my_buffer;
    my_buffer.length = buf.length;
    my_buffer.start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);

    if (my_buffer.start == MAP_FAILED) {
        cerr << "메모리 맵핑 실패" << endl;
        close(fd);
        return 1;
    }
    cout << "메모리 맵핑 성공" << endl;

    // 5. 초기 버퍼 큐잉
    if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
        cerr << "초기 버퍼 큐잉 실패" << endl;
        return 1;
    }

    // 6. 스트리밍 시작 (카메라 ON)
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) == -1) {
        cerr << "스트리밍 시작 실패" << endl;
        return 1;
    }

    // 30프레임 예열
    cout << "카메라 예열 중... (약 1초 소요)" << endl;

    // 100프레임 연속 저장 (약 3초 분량)
    cout << "녹화 시작! (100프레임 저장 중...)" << endl;

    int file_fd = open("../yuv/output_video.yuv", O_WRONLY | O_CREAT | O_TRUNC, 0666);

    for (int i = 0; i < 100; i++) {
        // 1. 꺼내오기
        if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
            cerr << "프레임 에러" << endl;
            break;
        }

        // 2. 저장
        write(file_fd, my_buffer.start, buf.bytesused);
        cout << "\r>> 녹화 중: " << i << "/100 프레임" << flush;

        // 3. 반납하기
        if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            break;
        }
    }

    close(file_fd);
    cout << "\n녹화 완료! (../yuv/output_video.yuv)" << endl;

    // 7. 끄기
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    munmap(my_buffer.start, my_buffer.length);
    close(fd);

    return 0;
}