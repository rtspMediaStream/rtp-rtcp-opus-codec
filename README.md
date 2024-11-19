# Manual

## rtp_opus.cpp 파일과 stream.sdp 파일 둘 다 다운 받으세요.

- Install
```bash
sudo apt update
sudo apt install libasound2-dev  // ALSA 
sudo apt install libopus-dev     // OPUS codec
sudo apt install ffmpeg		       // ffmpeg 
```

- Complie
```bash
g++ -o rtp_opus rtp_opus.cpp -lasound -lopus

```

- Start

한 개의 터미널을 켜서 실행 (Send)
```bash
./rtp_opus
```
또 하나의 터미널을 켜서 실행 (Receive)
```bash
ffmpeg -protocol_whitelist file,rtp,udp -i stream.sdp -f alsa default
```
