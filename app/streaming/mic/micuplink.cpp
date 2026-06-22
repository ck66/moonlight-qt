#include "micuplink.h"

#include <SDL_log.h>
#include <QHostAddress>
#include <QtEndian>

MicUplink::MicUplink(QObject* parent)
    : QObject(parent)
{
}

MicUplink::~MicUplink()
{
    stop();

    if (m_OpusEncoder) {
        opus_encoder_destroy(m_OpusEncoder);
        m_OpusEncoder = nullptr;
    }
}

bool MicUplink::start(const QString& hostAddress, const MicUplinkInfo& info,
                      const QString& deviceName)
{
    SDL_assert(!m_Running);
    SDL_assert(m_OpusEncoder == nullptr);

    m_HostAddress = hostAddress;
    m_HostPort = info.port;
    m_SessionId = info.sessionId;
    m_Token = info.token;
    m_Sequence = 0;
    m_Timestamp = 0;

    m_SamplesPerFrame = info.sampleRate * info.frameMs / 1000;
    m_Channels = info.channels;

    // ---- 1. 创建 Opus 编码器 ----
    int opusError;
    m_OpusEncoder = opus_encoder_create(info.sampleRate, info.channels,
                                         OPUS_APPLICATION_VOIP, &opusError);
    if (m_OpusEncoder == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "MicUplink: Failed to create Opus encoder: %d", opusError);
        return false;
    }

    opus_encoder_ctl(m_OpusEncoder, OPUS_SET_BITRATE(MIC_OPUS_BITRATE));
    // 不启用 DTX —— 保守稳定，避免 POC 中遇到的栈溢出问题

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "MicUplink: Opus encoder created (sampleRate=%d, channels=%d, bitrate=%d)",
                info.sampleRate, info.channels, MIC_OPUS_BITRATE);

    // ---- 2. 创建 UDP socket ----
    m_UdpSocket = new QUdpSocket(this);

    // ---- 3. 打开 SDL 音频采集设备 ----
    // 确保 SDL audio 子系统已初始化
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "MicUplink: SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s",
                         SDL_GetError());
            opus_encoder_destroy(m_OpusEncoder);
            m_OpusEncoder = nullptr;
            delete m_UdpSocket;
            m_UdpSocket = nullptr;
            return false;
        }
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = info.sampleRate;
    want.format = AUDIO_S16SYS;   // Opus 编码器需要 int16 输入
    want.channels = info.channels;
    want.samples = m_SamplesPerFrame;  // 20ms = 960 样本
    want.callback = micCaptureCallback;
    want.userdata = this;

    // deviceName 为空时使用系统默认设备
    const char* devName = deviceName.isEmpty() ? nullptr : deviceName.toUtf8().constData();
    m_CaptureDevice = SDL_OpenAudioDevice(devName, 1, &want, &have, 0);
    if (m_CaptureDevice == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "MicUplink: Failed to open capture device: %s",
                     SDL_GetError());
        opus_encoder_destroy(m_OpusEncoder);
        m_OpusEncoder = nullptr;
        delete m_UdpSocket;
        m_UdpSocket = nullptr;
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "MicUplink: Capture device opened (freq=%d, channels=%d, samples=%u, driver=%s)",
                have.freq, have.channels, have.samples, SDL_GetCurrentAudioDriver());

    // 开始采集
    SDL_PauseAudioDevice(m_CaptureDevice, 0);

    m_Running = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "MicUplink: Started — sending to %s:%u (sessionId=%u)",
                m_HostAddress.toUtf8().constData(), m_HostPort, m_SessionId);

    return true;
}

void MicUplink::stop()
{
    if (!m_Running) {
        return;
    }

    m_Running = false;

    // 停止并关闭采集设备
    if (m_CaptureDevice != 0) {
        SDL_PauseAudioDevice(m_CaptureDevice, 1);
        SDL_CloseAudioDevice(m_CaptureDevice);
        m_CaptureDevice = 0;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "MicUplink: Stopped");

    // 销毁 Opus 编码器
    if (m_OpusEncoder) {
        opus_encoder_destroy(m_OpusEncoder);
        m_OpusEncoder = nullptr;
    }

    // 关闭 UDP socket（Qt 对象会随 parent 删除，但显式删除更安全）
    if (m_UdpSocket) {
        delete m_UdpSocket;
        m_UdpSocket = nullptr;
    }

    // 清空缓冲区
    m_PcmBuffer.clear();
    m_PcmBufferSamples = 0;
}

void SDLCALL MicUplink::micCaptureCallback(void* userdata, Uint8* stream, int len)
{
    MicUplink* self = static_cast<MicUplink*>(userdata);
    if (!self->m_Running) {
        return;
    }

    int bytesPerSample = sizeof(opus_int16) * self->m_Channels;
    int sampleCount = len / bytesPerSample;

    self->processCapturedData(reinterpret_cast<const opus_int16*>(stream), sampleCount);
}

void MicUplink::processCapturedData(const opus_int16* pcm, int sampleCount)
{
    // 将数据追加到缓冲区
    int bytesToAdd = sampleCount * sizeof(opus_int16) * m_Channels;
    m_PcmBuffer.append(reinterpret_cast<const char*>(pcm), bytesToAdd);
    m_PcmBufferSamples += sampleCount;

    int frameBytes = m_SamplesPerFrame * sizeof(opus_int16) * m_Channels;

    // 攒满一帧就编码发送
    while (m_PcmBuffer.size() >= frameBytes) {
        encodeAndSendFrame(reinterpret_cast<const opus_int16*>(m_PcmBuffer.constData()));

        // 移除已处理的一帧
        m_PcmBuffer.remove(0, frameBytes);
        m_PcmBufferSamples -= m_SamplesPerFrame;
    }
}

void MicUplink::encodeAndSendFrame(const opus_int16* pcmFrame)
{
    // ---- Opus 编码 ----
    unsigned char opusData[4000];
    int encoded = opus_encode(m_OpusEncoder, pcmFrame, m_SamplesPerFrame,
                               opusData, sizeof(opusData));
    if (encoded <= 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "MicUplink: Opus encode failed: %d", encoded);
        // 仍然递增时间戳以保持连续性
        m_Timestamp += m_SamplesPerFrame;
        return;
    }

    // ---- 构造数据包 (32 字节头 + Opus 负载) ----
    // 头部为大端字节序，与服务端 axi_mic_packet_header_t 匹配
    QByteArray packet(32 + encoded, 0);
    char* data = packet.data();

    // session_id (4B, big-endian)
    qToBigEndian(m_SessionId, data);
    // sequence (4B, big-endian)
    qToBigEndian(m_Sequence++, data + 4);
    // timestamp (4B, big-endian)
    qToBigEndian(m_Timestamp, data + 8);
    // token (16B, 原始字节)
    memcpy(data + 12, m_Token.constData(), 16);
    // payload_type (1B) = 1 (OPUS)
    data[28] = MIC_OPUS_PAYLOAD_TYPE;
    // flags (1B) = 0
    data[29] = 0;
    // payload_size (2B, big-endian)
    qToBigEndian(static_cast<quint16>(encoded), data + 30);
    // Opus 负载
    memcpy(data + 32, opusData, encoded);

    // 递增时间戳（每帧 += samples_per_frame，48kHz/20ms = 960）
    m_Timestamp += m_SamplesPerFrame;

    // ---- UDP 发送 ----
    qint64 written = m_UdpSocket->writeDatagram(packet,
                                                  QHostAddress(m_HostAddress),
                                                  m_HostPort);
    if (written < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "MicUplink: UDP send failed: %s",
                    m_UdpSocket->errorString().toUtf8().constData());
    }
}
