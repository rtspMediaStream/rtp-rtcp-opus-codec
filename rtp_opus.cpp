// RTP, RTCP, 오디오 캡처 및 MP3 인코딩을 처리하는 C++ 클래스 기반 프로그램 (싱글톤 패턴 적용)

#include <iostream>
#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <alsa/asoundlib.h>
#include <opus/opus.h>

#define DEST_IP "127.0.0.1"
#define RTP_PORT 5004
#define RTCP_PORT 5005
#define PCM_FRAME_SIZE 1152
#define OPUS_SAMPLE_RATE 48000
#define OPUS_FRAME_SIZE 480  // 10ms at 48kHz
#define CHANNELS 2
#define APPLICATION OPUS_APPLICATION_AUDIO
#define MAX_PACKET_SIZE 1500

class RTPHeader {
public:
    struct rtp_header {
#if __BYTE_ORDER == __LITTLE_ENDIAN
        uint8_t cc:4;
        uint8_t x:1;
        uint8_t p:1;
        uint8_t version:2;
        uint8_t pt:7;
        uint8_t m:1;
#elif __BYTE_ORDER == __BIG_ENDIAN
        uint8_t version:2;
        uint8_t p:1;
        uint8_t x:1;
        uint8_t cc:4;
        uint8_t m:1;
        uint8_t pt:7;
#else
#error "Please fix <bits/endian.h>"
#endif
        uint16_t seq_num;
        uint32_t timestamp;
        uint32_t ssrc;
    } __attribute__((packed));

    static void create(rtp_header &header, unsigned short seq_num, unsigned int timestamp, unsigned int ssrc) {
        header.version = 2;
        header.p = 0;
        header.x = 0;
        header.cc = 0;
        header.m = 0;
        header.pt = 111;  
        header.seq_num = htons(seq_num);
        header.timestamp = htonl(timestamp);
        header.ssrc = htonl(ssrc);
    }
};

class RTCPSenderReport {
public:
    struct rtcp_sr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
        uint8_t rc:5;
        uint8_t p:1;
        uint8_t version:2;
#elif __BYTE_ORDER == __BIG_ENDIAN
        uint8_t version:2;
        uint8_t p:1;
        uint8_t rc:5;
#else
#error "Please fix <bits/endian.h>"
#endif
        uint8_t pt;          // 패킷 타입 (200 for SR)
        uint16_t length;     // RTCP 패킷 길이
        uint32_t ssrc;       // 송신자의 SSRC
        uint32_t ntp_timestamp_msw;  // NTP 타임스탬프 상위 32비트
        uint32_t ntp_timestamp_lsw;  // NTP 타임스탬프 하위 32비트
        uint32_t rtp_timestamp;      // RTP 타임스탬프
        uint32_t sender_packet_count; // 송신한 패킷 수
        uint32_t sender_octet_count;  // 송신한 옥텟(바이트) 수
    } __attribute__((packed));

    static void create(rtcp_sr &sr, uint32_t ssrc, uint32_t rtp_timestamp, uint32_t packet_count, uint32_t octet_count) {
        memset(&sr, 0, sizeof(sr));
        sr.version = 2;
        sr.p = 0;
        sr.rc = 0;
        sr.pt = 200;
        sr.length = htons(6);
        sr.ssrc = htonl(ssrc);

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        uint64_t ntp_time = ((uint64_t)(now.tv_sec + 2208988800ULL) << 32) |
                            ((uint64_t)now.tv_nsec * 0x100000000ULL / 1000000000ULL);

        sr.ntp_timestamp_msw = htonl((uint32_t)(ntp_time >> 32));
        sr.ntp_timestamp_lsw = htonl((uint32_t)(ntp_time & 0xFFFFFFFF));
        sr.rtp_timestamp = htonl(rtp_timestamp);
        sr.sender_packet_count = htonl(packet_count);
        sr.sender_octet_count = htonl(octet_count);
    }
};

class AudioCapture {
private:
    snd_pcm_t *pcm_handle;
    snd_pcm_hw_params_t *params;
    unsigned int sample_rate;
    int dir;

public:
    AudioCapture(unsigned int rate = 48000) : sample_rate(rate) {
        int rc = snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_CAPTURE, 0);
        if (rc < 0) {
            throw std::runtime_error("PCM 디바이스를 열 수 없습니다: " + std::string(snd_strerror(rc)));
        }

        snd_pcm_hw_params_alloca(&params);
        snd_pcm_hw_params_any(pcm_handle, params);
        snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm_handle, params, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(pcm_handle, params, 2);
        snd_pcm_hw_params_set_rate_near(pcm_handle, params, &sample_rate, &dir);
        rc = snd_pcm_hw_params(pcm_handle, params);
        if (rc < 0) {
            throw std::runtime_error("하드웨어 파라미터를 설정할 수 없습니다: " + std::string(snd_strerror(rc)));
        }
    }

    int read(short *buffer, int frames) {
        int rc = snd_pcm_readi(pcm_handle, buffer, frames);
        if (rc == -EPIPE) {
            std::cerr << "오버런 발생" << std::endl;
            snd_pcm_prepare(pcm_handle);
        } else if (rc < 0) {
            std::cerr << "PCM 디바이스에서 읽기 오류: " << snd_strerror(rc) << std::endl;
        }
        return rc;
    }

    ~AudioCapture() {
        snd_pcm_close(pcm_handle);
    }
};

class OpusEncoder {
private:
    OpusEncoder* encoder;
    unsigned char encoded_buffer[MAX_PACKET_SIZE];
    int error;

public:
    OpusEncoder() {
        encoder = opus_encoder_create(OPUS_SAMPLE_RATE, CHANNELS, APPLICATION, &error);
        if (error < 0) {
            throw std::runtime_error("Opus 인코더 생성 실패: " + std::string(opus_strerror(error)));
        }

        // 비트레이트 설정 (128kbps)
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(128000));
    }

    int encode(const short* pcm_buffer, int frame_size, unsigned char* encoded_data) {
        int encoded_bytes = opus_encode(encoder, pcm_buffer, frame_size, 
                                      encoded_data, MAX_PACKET_SIZE);
        if (encoded_bytes < 0) {
            throw std::runtime_error("Opus 인코딩 실패: " + std::string(opus_strerror(encoded_bytes)));
        }
        return encoded_bytes;
    }

    ~OpusEncoder() {
        if (encoder) {
            opus_encoder_destroy(encoder);
        }
    }
};

class RTPStreamer {
private:
    int rtp_sockfd, rtcp_sockfd;
    sockaddr_in rtp_dest_addr, rtcp_dest_addr;
    unsigned short seq_num;
    unsigned int timestamp;
    unsigned int ssrc;
    unsigned int packet_count;
    unsigned int octet_count;

public:
    RTPStreamer(const std::string &ip = DEST_IP, int rtp_port = RTP_PORT, int rtcp_port = RTCP_PORT) 
        : seq_num(rand() % 65535), timestamp(0), ssrc(12345), packet_count(0), octet_count(0) {
        rtp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (rtp_sockfd < 0) {
            throw std::runtime_error("RTP 소켓 생성 실패");
        }

        rtcp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (rtcp_sockfd < 0) {
            throw std::runtime_error("RTCP 소켓 생성 실패");
        }

        memset(&rtp_dest_addr, 0, sizeof(rtp_dest_addr));
        rtp_dest_addr.sin_family = AF_INET;
        rtp_dest_addr.sin_port = htons(rtp_port);
        inet_pton(AF_INET, ip.c_str(), &rtp_dest_addr.sin_addr);

        memset(&rtcp_dest_addr, 0, sizeof(rtcp_dest_addr));
        rtcp_dest_addr.sin_family = AF_INET;
        rtcp_dest_addr.sin_port = htons(rtcp_port);
        inet_pton(AF_INET, ip.c_str(), &rtcp_dest_addr.sin_addr);
    }

    void sendRTPPacket(unsigned char *payload, int payload_size) {
        RTPHeader::rtp_header header;
        RTPHeader::create(header, seq_num, timestamp, ssrc);

        unsigned char packet[1500];
        memcpy(packet, &header, sizeof(header));
        memcpy(packet + sizeof(header), payload, payload_size);

        int packet_size = sizeof(header) + payload_size;
        if (sendto(rtp_sockfd, packet, packet_size, 0, (struct sockaddr *)&rtp_dest_addr, sizeof(rtp_dest_addr)) < 0) {
            throw std::runtime_error("RTP 패킷 전송 오류");
        }
        std::cout << "RTP 패킷 " << ntohs(header.seq_num) << " 전송됨, 크기 " << packet_size << " 바이트" << std::endl;

        seq_num++;
        timestamp += PCM_FRAME_SIZE;
        packet_count++;
        octet_count += payload_size;

        if (packet_count % 50 == 0) {
            RTCPSenderReport::rtcp_sr sr;
            RTCPSenderReport::create(sr, ssrc, timestamp, packet_count, octet_count);
            if (sendto(rtcp_sockfd, &sr, sizeof(sr), 0, (struct sockaddr *)&rtcp_dest_addr, sizeof(rtcp_dest_addr)) < 0) {
                throw std::runtime_error("RTCP SR 전송 오류");
            }
            std::cout << "RTCP SR 전송됨" << std::endl;
        }
    }

    ~RTPStreamer() {
        close(rtp_sockfd);
        close(rtcp_sockfd);
    }
};

int main() {
    try {
        AudioCapture audioCapture(OPUS_SAMPLE_RATE);  // Opus는 48kHz를 선호
        RTPStreamer rtpStreamer;
        OpusEncoder opusEncoder;

        short pcm_buffer[OPUS_FRAME_SIZE * CHANNELS];
        unsigned char encoded_buffer[MAX_PACKET_SIZE];
        unsigned int timestamp_increment = OPUS_FRAME_SIZE;

        while (true) {
            int rc = audioCapture.read(pcm_buffer, OPUS_FRAME_SIZE);
            if (rc != OPUS_FRAME_SIZE) {
                continue;
            }

            // PCM 데이터를 Opus로 인코딩
            int encoded_bytes = opusEncoder.encode(pcm_buffer, OPUS_FRAME_SIZE, encoded_buffer);
            if (encoded_bytes > 0) {
                // 인코딩된 Opus 데이터를 RTP로 전송
                rtpStreamer.sendRTPPacket(encoded_buffer, encoded_bytes);
            }
        }

    } catch (const std::exception &e) {
        std::cerr << "오류: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
