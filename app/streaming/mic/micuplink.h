#pragma once

#include <QObject>
#include <QUdpSocket>
#include <QByteArray>
#include <QString>

#include <SDL.h>
#include <opus.h>

// 音频参数常量（与服务端 Axixi2233/Sunshine 匹配）
#define MIC_SAMPLE_RATE       48000
#define MIC_CHANNELS          1
#define MIC_FRAME_MS          20
#define MIC_SAMPLES_PER_FRAME (MIC_SAMPLE_RATE * MIC_FRAME_MS / 1000)  // 960
#define MIC_FRAME_BYTES       (MIC_SAMPLES_PER_FRAME * MIC_CHANNELS * sizeof(opus_int16))  // 1920
#define MIC_OPUS_BITRATE      48000  // 48 kbps
#define MIC_OPUS_PAYLOAD_TYPE 1

// 服务端 /mic-uplink 返回的协商信息
struct MicUplinkInfo
{
    bool enabled = false;
    quint16 port = 0;           // axiMicPort (UDP 目标端口)
    quint32 sessionId = 0;      // axiMicSessionId
    QByteArray token;           // axiMicToken (16 字节)
    int sampleRate = MIC_SAMPLE_RATE;
    int channels = MIC_CHANNELS;
    int frameMs = MIC_FRAME_MS;
    QString codec;              // "opus"
};

// 麦克风上行管理类
// 负责：SDL 音频采集 → Opus 编码 → UDP 发送到服务端 axiMicPort
class MicUplink : public QObject
{
    Q_OBJECT

public:
    explicit MicUplink(QObject* parent = nullptr);
    ~MicUplink();

    // 启动麦克风上行
    // hostAddress: 服务端 IP 地址
    // info: /mic-uplink 协商返回的信息
    // deviceName: SDL 音频设备名（空字符串 = 系统默认）
    // 返回 true 表示成功启动
    bool start(const QString& hostAddress, const MicUplinkInfo& info,
               const QString& deviceName);

    // 停止麦克风上行（阻塞直到 SDL 采集设备关闭）
    void stop();

    // 是否正在运行
    bool isRunning() const { return m_Running; }

private:
    // SDL 音频采集回调（在 SDL 内部音频线程中调用）
    static void SDLCALL micCaptureCallback(void* userdata, Uint8* stream, int len);

    // 处理采集到的 PCM 数据：攒满一帧后编码并发送
    void processCapturedData(const opus_int16* pcm, int sampleCount);

    // 编码一帧 Opus 并通过 UDP 发送
    void encodeAndSendFrame(const opus_int16* pcmFrame);

    SDL_AudioDeviceID m_CaptureDevice = 0;
    OpusEncoder* m_OpusEncoder = nullptr;
    QUdpSocket* m_UdpSocket = nullptr;

    // 网络目标信息
    QString m_HostAddress;
    quint16 m_HostPort = 0;
    quint32 m_SessionId = 0;
    QByteArray m_Token;

    // 包序号与时间戳
    quint32 m_Sequence = 0;
    quint32 m_Timestamp = 0;

    // PCM 缓冲区（累积不足一帧的数据）
    QByteArray m_PcmBuffer;
    int m_PcmBufferSamples = 0;

    int m_SamplesPerFrame = MIC_SAMPLES_PER_FRAME;
    int m_Channels = MIC_CHANNELS;

    bool m_Running = false;
};
