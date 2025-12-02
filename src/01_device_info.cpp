#include <iostream>
#include <cstring>
#include <fcntl.h>      // open()
#include <unistd.h>     // close()
#include <sys/ioctl.h>  // ioctl()
#include <linux/videodev2.h> // V4L2 핵심 헤더

#define CAMERA_DEV "/dev/video0" // 첫 번째 카메라 장치

using namespace std;

int main() {
    // 1. 카메라 장치 파일 열기 (읽기/쓰기 모드) 
    int fd = open(CAMERA_DEV, O_RDWR);  // 카메라 전원을 키고 읽기/쓰기가 가능한 모드(O_RDWR)로 설정
    if (fd == -1) {     // 카메라 켜기 실패
        cout << "카메라 열기 실패! (" << CAMERA_DEV << ")" << endl;
        return 1;
    }
    cout << "카메라 열기 성공! (FD: " << fd << ")" << endl;

    // 2. 카메라 기능(Capability) 확인
    struct v4l2_capability cap; 
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {       // input/output control, 드라이버에게 장치 정보 요청
        cout << "장치 정보 조회 실패!" << endl;
        close(fd);
        return 1;
    }

    cout << "---------------------------------" << endl;
    cout << "driver : " << cap.driver << endl;
    cout << "card   : " << cap.card << endl;
    cout << "bus_info : " << cap.bus_info << endl;
    cout << "version : " << cap.version << endl;
    cout << "capabilities : " << cap.capabilities << endl;
    cout << "device_caps : " << cap.device_caps << endl;
    cout << "reversed : " << cap.reserved << endl;

    // 이 장치가 비디오 캡처(촬영) 기능을 지원하는지 확인
    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
        cout << "비디오 캡처 지원함" << endl;
    }
    else {
        cout << "비디오 캡처 지원 안함" << endl;
    }

    // 3. 현재 설정된 포맷 확인
    struct v4l2_format fmt;
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) == -1) { 
        cerr << "포맷 정보 조회 실패!" << endl;
    }
    else {
        cout << "가로 해상도: " << fmt.fmt.pix.width << endl;
        cout << "세로 해상도: " << fmt.fmt.pix.height << endl;
        cout << "픽셀 포맷  : " << (char*)&fmt.fmt.pix.pixelformat << endl;
    }
    cout << "---------------------------------" << endl;

    close(fd);
    return 0;
}