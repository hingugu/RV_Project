#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>

// --------------------------------------------------------------------
// [설정] PC(받는 쪽)의 IP 주소로 변경하세요!
// --------------------------------------------------------------------
#define DEST_IP "192.168.0.30" // PC IP 주소
#define DEST_PORT 5004

#define CAMERA_DEV "/dev/video0"
#define ENCODER_DEV "/dev/video11"
#define WIDTH 640
#define HEIGHT 480
#define MTU_SIZE 1400

using namespace std;

// RTP 헤더 정의
#pragma pack(push, 1)
struct RtpHeader {
    uint8_t csrc_count : 4;
    uint8_t extension : 1;
    uint8_t padding : 1;
    uint8_t version : 2;

    uint8_t payload_type : 7;
    uint8_t marker : 1;

    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
};
#pragma pack(pop)

struct Buffer {
    void* start;
    size_t length;
};

int sock_fd;
struct sockaddr_in dest_addr;
uint16_t g_seq = 0;
uint32_t g_timestamp = 0;

bool is_mplane(uint32_t type) {
    return (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
        type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
}

void send_rtp_packet(uint8_t* data, int len, bool is_last) {
    uint8_t packet[1500];
    RtpHeader* rtp = (RtpHeader*)packet;

    rtp->version = 2;
    rtp->padding = 0;
    rtp->extension = 0;
    rtp->csrc_count = 0;
    rtp->marker = is_last ? 1 : 0;
    rtp->payload_type = 96; // H.264 Dynamic Payload Type
    rtp->sequence = htons(g_seq++);
    rtp->timestamp = htonl(g_timestamp);
    rtp->ssrc = htonl(0x12345678);

    memcpy(packet + 12, data, len);
    sendto(sock_fd, packet, 12 + len, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
}

void send_single_nalu(uint8_t* nalu_data, int nalu_len) {
    if (nalu_len <= 0) return;

    if (nalu_len <= MTU_SIZE) {
        send_rtp_packet(nalu_data, nalu_len, true);
    }
    else {
        // FU-A Fragmentation
        uint8_t nalu_header = nalu_data[0];
        uint8_t type = nalu_header & 0x1F;
        nalu_data++;
        nalu_len--;

        uint8_t fu_indicator = (nalu_header & 0xE0) | 28;
        bool start_bit = true;

        while (nalu_len > 0) {
            int send_len = (nalu_len > MTU_SIZE - 2) ? (MTU_SIZE - 2) : nalu_len;
            bool end_bit = (nalu_len <= MTU_SIZE - 2);

            uint8_t fu_header = type;
            if (start_bit) fu_header |= 0x80;
            if (end_bit)   fu_header |= 0x40;

            uint8_t payload[1500];
            payload[0] = fu_indicator;
            payload[1] = fu_header;
            memcpy(payload + 2, nalu_data, send_len);

            send_rtp_packet(payload, 2 + send_len, end_bit);

            nalu_data += send_len;
            nalu_len -= send_len;
            start_bit = false;
        }
    }
}

void send_h264_frame(uint8_t* frame, int len) {
    uint8_t* p = frame;
    uint8_t* end = frame + len;
    uint8_t* nalu_start = NULL;

    while (p < end - 3) {
        if (p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01) {
            if (nalu_start != NULL) {
                send_single_nalu(nalu_start, p - nalu_start);
            }
            nalu_start = p + 3; // Skip start code
            p += 3;
        }
        else {
            p++;
        }
    }
    // Last NALU
    if (nalu_start != NULL && nalu_start < end) {
        send_single_nalu(nalu_start, end - nalu_start);
    }
    g_timestamp += 3000; // 90000Hz / 30fps = 3000
}

// --------------------------------------------------------------------
// V4L2 Helper Functions
// --------------------------------------------------------------------
int set_format(int fd, uint32_t type, uint32_t pixelformat, int w, int h) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    if (is_mplane(type)) {
        fmt.fmt.pix_mp.width = w;
        fmt.fmt.pix_mp.height = h;
        fmt.fmt.pix_mp.pixelformat = pixelformat;
        fmt.fmt.pix_mp.num_planes = 1;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        // H.264 인코더 버퍼 크기 넉넉하게 잡기
        if (pixelformat == V4L2_PIX_FMT_H264) fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 2 * 1024 * 1024;
    }
    else {
        fmt.fmt.pix.width = w;
        fmt.fmt.pix.height = h;
        fmt.fmt.pix.pixelformat = pixelformat;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
    }
    return ioctl(fd, VIDIOC_S_FMT, &fmt);
}

int set_encoder_settings(int fd) {
    struct v4l2_ext_control ctrls[3]; // 4 -> 3으로 복구 (비트레이트 제거)
    struct v4l2_ext_controls control;
    memset(ctrls, 0, sizeof(ctrls));
    memset(&control, 0, sizeof(control));

    // 1. SPS/PPS 헤더를 매 IDR 프레임마다 삽입 (중요: 중간 접속 시 영상 보임)
    ctrls[0].id = V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER;
    ctrls[0].value = 1;

    // 2. I-Frame 간격 설정 (15 -> 60으로 변경)
    // 60프레임(약 2초)마다 키프레임 전송. 키프레임 빈도를 줄여 네트워크 부하 감소
    ctrls[1].id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD;
    ctrls[1].value = 60;

    // 3. 프로파일 설정 (Baseline이 호환성 좋음)
    ctrls[2].id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
    ctrls[2].value = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;

    // [제거] 비트레이트 설정 제거 (오히려 지연 원인이 됨)

    control.ctrl_class = V4L2_CTRL_CLASS_MPEG;
    control.count = 3;
    control.controls = ctrls;
    return ioctl(fd, VIDIOC_S_EXT_CTRLS, &control);
}

int request_buffers(int fd, uint32_t type, Buffer* buffers) {
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 1; // 단일 버퍼 사용 (가장 단순한 구조)
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        perror("Request Buffers Failed");
        return -1;
    }

    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (is_mplane(type)) { buf.m.planes = planes; buf.length = 1; }

    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
        perror("Query Buffer Failed");
        return -1;
    }

    if (is_mplane(type)) {
        buffers[0].length = buf.m.planes[0].length;
        buffers[0].start = mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.planes[0].m.mem_offset);
    }
    else {
        buffers[0].length = buf.length;
        buffers[0].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    }

    if (buffers[0].start == MAP_FAILED) {
        perror("MMAP Failed");
        return -1;
    }
    return 0;
}

int queue_buffer(int fd, uint32_t type, int index, int bytesused) {
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
    return ioctl(fd, VIDIOC_QBUF, &buf);
}

int main() {
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(DEST_PORT);
    inet_pton(AF_INET, DEST_IP, &dest_addr.sin_addr);

    cout << "RTP 스트리밍 준비 완료 (" << DEST_IP << ":" << DEST_PORT << ")" << endl;

    int cam_fd = open(CAMERA_DEV, O_RDWR);
    int enc_fd = open(ENCODER_DEV, O_RDWR);
    if (cam_fd < 0 || enc_fd < 0) {
        perror("Device Open Failed");
        return 1;
    }

    // 1. 포맷 설정
    if (set_format(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_YUV420, WIDTH, HEIGHT) < 0) perror("Cam Format Fail");
    if (set_format(enc_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_YUV420, WIDTH, HEIGHT) < 0) perror("Enc Out Format Fail");
    if (set_format(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_H264, WIDTH, HEIGHT) < 0) perror("Enc Cap Format Fail");

    // 2. 프레임 레이트 설정
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = 30;
    ioctl(enc_fd, VIDIOC_S_PARM, &parm);

    set_encoder_settings(enc_fd);

    // 3. 버퍼 요청 및 매핑
    Buffer cam_buf, enc_in, enc_out;
    request_buffers(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &cam_buf);
    request_buffers(enc_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &enc_in);
    request_buffers(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &enc_out);

    // 4. 스트리밍 시작 (Stream On)
    enum v4l2_buf_type type;

    // 카메라: 큐에 넣고 시작
    queue_buffer(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0, 0);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(cam_fd, VIDIOC_STREAMON, &type);

    // 인코더: 입력(Output Plane)은 아직 데이터가 없으므로 큐에 넣지 않고 StreamOn만 함
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; ioctl(enc_fd, VIDIOC_STREAMON, &type);

    // 인코더: 출력(Capture Plane)은 큐에 넣고 시작 (결과 받을 준비)
    queue_buffer(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0, 0);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; ioctl(enc_fd, VIDIOC_STREAMON, &type);

    cout << ">>> 실시간 스트리밍 시작! <<<" << endl;

    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    struct v4l2_buffer enc_in_reclaim; // 인코더 입력 버퍼 회수용
    struct v4l2_plane enc_in_planes[1];

    while (true) {
        // -------------------------------------------------------
        // A. 카메라에서 프레임 가져오기 (DQBUF)
        // -------------------------------------------------------
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cam_fd, VIDIOC_DQBUF, &buf) < 0) {
            perror("Camera DQBUF Fail");
            usleep(1000); continue;
        }

        // -------------------------------------------------------
        // B. 카메라 데이터 -> 인코더 입력 버퍼로 복사
        // -------------------------------------------------------
        // 주의: 여기서 enc_in.start에 바로 씁니다. 
        // (버퍼가 1개라 이전 인코딩이 끝났다고 가정. 
        // 엄밀히는 아래의 'E단계'에서 회수한 버퍼를 써야 함)
        memcpy(enc_in.start, cam_buf.start, buf.bytesused);

        // 카메라 버퍼 반납 (QBUF)
        queue_buffer(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0, 0);

        // -------------------------------------------------------
        // C. 인코더에 데이터 넣기 (QBUF - Output Plane)
        // -------------------------------------------------------
        // 큐에 넣어서 "인코딩 해줘"라고 요청
        if (queue_buffer(enc_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0, buf.bytesused) < 0) {
            perror("Encoder Input QBUF Fail");
        }

        // -------------------------------------------------------
        // D. 인코더 결과물 기다리기 (DQBUF - Capture Plane) - Blocking
        // -------------------------------------------------------
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = 1;

        if (ioctl(enc_fd, VIDIOC_DQBUF, &buf) == 0) {
            // [성공] H.264 데이터 전송
            int h264_size = buf.m.planes[0].bytesused;
            uint8_t* h264_data = (uint8_t*)enc_out.start;

            send_h264_frame(h264_data, h264_size);

            // [성능 개선] 매 프레임 출력하면 느려지므로 30프레임마다(약 1초마다) 출력
            static int frame_cnt = 0;
            if (frame_cnt % 30 == 0) {
                cout << ">> [Frame " << frame_cnt << "] " << h264_size << " bytes sent." << endl;
            }
            frame_cnt++;

            // 인코더 출력 버퍼 반납 (QBUF - Capture Plane) - 다시 쓸 수 있게
            queue_buffer(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0, 0);
        }
        else {
            perror("Encoder Output DQBUF Fail");
        }

        // -------------------------------------------------------
        // [중요 수정] E. 인코더 입력 버퍼 회수하기 (DQBUF - Output Plane)
        // -------------------------------------------------------
        // 이 부분이 없으면 2번째 루프에서 인코더 입력 큐가 꽉 차서 죽습니다.
        // 인코더가 "나 원본 데이터 다 읽었어"라고 반환하는 과정입니다.
        memset(&enc_in_reclaim, 0, sizeof(enc_in_reclaim));
        memset(enc_in_planes, 0, sizeof(enc_in_planes));
        enc_in_reclaim.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        enc_in_reclaim.memory = V4L2_MEMORY_MMAP;
        enc_in_reclaim.m.planes = enc_in_planes;
        enc_in_reclaim.length = 1;

        if (ioctl(enc_fd, VIDIOC_DQBUF, &enc_in_reclaim) < 0) {
            perror("Encoder Input Reclaim Failed");
        }
    }

    close(cam_fd);
    close(enc_fd);
    close(sock_fd);
    return 0;
}