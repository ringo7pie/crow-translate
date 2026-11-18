/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "audiocapture.h"

#include <QAudioDevice>
#include <QAudioSource>
#include <QMediaDevices>
#include <QThread>
#include <QtEndian>

#include <cmath>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#endif

namespace {
constexpr int targetRate = 16000; // 認識に渡すサンプル周波数
}

// 入出力のサンプル周波数を設定する
void MonoResampler::setRates(int inputRate, int outputRate)
{
    m_step = inputRate > 0 && outputRate > 0 ? double(inputRate) / double(outputRate) : 1.0;
    m_position = 0.0;
    m_previous = 0.0f;
}

// mono float の入力を線形補間で変換する
void MonoResampler::process(const float *input, qsizetype frames, QVector<float> &output)
{
    if (frames <= 0)
        return;
    const double last = double(frames - 1); // 入力の最後の位置
    while (m_position <= last) {
        const auto index = static_cast<qsizetype>(std::floor(m_position)); // 補間の左側の位置（-1 は前回の最後）
        const double fraction = m_position - double(index); // 左右の間の割合
        const float left = index < 0 ? m_previous : input[index]; // 左側のサンプル
        const float right = index + 1 < frames ? input[index + 1] : left; // 右側のサンプル
        output.append(left + float((right - left) * fraction));
        m_position += m_step;
    }
    m_position -= double(frames);
    m_previous = input[frames - 1];
}

// 生の PCM を mono float にする
QVector<float> MonoResampler::toMono(const char *data, qsizetype bytes, QAudioFormat::SampleFormat format, int channels)
{
    QVector<float> mono; // 出力
    if (channels <= 0)
        return mono;
    int bytesPerSample = 0; // 1 サンプルのバイト数
    switch (format) {
    case QAudioFormat::UInt8:
        bytesPerSample = 1;
        break;
    case QAudioFormat::Int16:
        bytesPerSample = 2;
        break;
    case QAudioFormat::Int32:
    case QAudioFormat::Float:
        bytesPerSample = 4;
        break;
    default:
        return mono;
    }
    const qsizetype frames = bytes / (bytesPerSample * channels); // フレーム数
    mono.resize(frames);
    for (qsizetype frame = 0; frame < frames; ++frame) {
        float sum = 0.0f; // チャンネルの合計
        for (int channel = 0; channel < channels; ++channel) {
            const char *sample = data + (frame * channels + channel) * bytesPerSample; // このサンプルの先頭
            switch (format) {
            case QAudioFormat::UInt8:
                sum += (float(quint8(*sample)) - 128.0f) / 128.0f;
                break;
            case QAudioFormat::Int16:
                sum += float(qFromLittleEndian<qint16>(sample)) / 32768.0f;
                break;
            case QAudioFormat::Int32:
                sum += float(double(qFromLittleEndian<qint32>(sample)) / 2147483648.0);
                break;
            case QAudioFormat::Float: {
                float value = 0.0f; // float のサンプル
                std::memcpy(&value, sample, sizeof(value));
                sum += value;
                break;
            }
            default:
                break;
            }
        }
        mono[frame] = sum / float(channels);
    }
    return mono;
}

AudioCapture::AudioCapture(Kind kind, const QByteArray &deviceId, QObject *parent)
    : QObject(parent)
    , m_kind(kind)
    , m_deviceId(deviceId)
{
    qRegisterMetaType<QVector<float>>();
}

AudioCapture::~AudioCapture()
{
    stop();
}

AudioCapture::Kind AudioCapture::kind() const
{
    return m_kind;
}

void AudioCapture::setMuted(bool muted)
{
    m_muted = muted;
}

// 取り込みを始める
bool AudioCapture::start()
{
    stop();
    return m_kind == Kind::Microphone ? startMicrophone() : startLoopback();
}

// 取り込みを止める
void AudioCapture::stop()
{
    if (m_source) {
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
        m_sourceDevice = nullptr;
    }
    if (m_loopbackThread) {
        m_loopbackRunning = false;
        m_loopbackThread->wait(3000);
        delete m_loopbackThread;
        m_loopbackThread = nullptr;
    }
}

// マイクの取り込みを始める
bool AudioCapture::startMicrophone()
{
    QAudioDevice device = QMediaDevices::defaultAudioInput(); // 使う入力デバイス
    if (!m_deviceId.isEmpty()) {
        const auto inputs = QMediaDevices::audioInputs(); // 入力デバイスの一覧
        for (const QAudioDevice &candidate : inputs) {
            if (candidate.id() == m_deviceId) {
                device = candidate;
                break;
            }
        }
    }
    if (device.isNull()) {
        emit errorOccurred(tr("No microphone was found."));
        return false;
    }
    QAudioFormat format; // 望ましい形式（16 kHz mono float）
    format.setSampleRate(targetRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Float);
    if (!device.isFormatSupported(format))
        format = device.preferredFormat();
    m_format = format;
    m_resampler.setRates(format.sampleRate(), targetRate);

    m_source = new QAudioSource(device, format, this);
    m_source->setBufferSize(format.bytesForDuration(200000));
    m_sourceDevice = m_source->start();
    if (!m_sourceDevice || m_source->error() != QAudio::NoError) {
        emit errorOccurred(tr("Could not open the microphone \"%1\".").arg(device.description()));
        stop();
        return false;
    }
    connect(m_sourceDevice, &QIODevice::readyRead, this, &AudioCapture::readMicrophone);
    return true;
}

// マイクの音声を読み、16 kHz mono にして出す
void AudioCapture::readMicrophone()
{
    if (!m_sourceDevice)
        return;
    const QByteArray data = m_sourceDevice->readAll(); // 届いた PCM
    if (data.isEmpty() || m_muted)
        return;
    const QVector<float> mono = MonoResampler::toMono(data.constData(), data.size(), m_format.sampleFormat(), m_format.channelCount()); // mono にした音声
    QVector<float> output; // 16 kHz にした音声
    output.reserve(mono.size());
    m_resampler.process(mono.constData(), mono.size(), output);
    if (!output.isEmpty())
        emit samplesReady(output);
}

// PC の再生音（WASAPI ループバック）の取り込みを始める
bool AudioCapture::startLoopback()
{
#ifdef Q_OS_WIN
    m_loopbackRunning = true;
    const QString endpointId = QString::fromUtf8(m_deviceId); // 取り込む出力デバイスのエンドポイント ID
    m_loopbackThread = QThread::create([this, endpointId]() {
        // 失敗をメインスレッドへ知らせる
        const auto reportError = [this](const QString &message) {
            QMetaObject::invokeMethod(this, [this, message]() { emit errorOccurred(message); }, Qt::QueuedConnection);
        };
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // このスレッドの COM 初期化
        IMMDeviceEnumerator *enumerator = nullptr; // デバイスの列挙
        IMMDevice *device = nullptr; // 取り込む出力デバイス
        IAudioClient *client = nullptr; // 音声クライアント
        IAudioCaptureClient *capture = nullptr; // 取り込みクライアント
        WAVEFORMATEX *mixFormat = nullptr; // デバイスのミックス形式
        const auto cleanup = [&]() {
            if (client)
                client->Stop();
            if (mixFormat)
                CoTaskMemFree(mixFormat);
            if (capture)
                capture->Release();
            if (client)
                client->Release();
            if (device)
                device->Release();
            if (enumerator)
                enumerator->Release();
            if (SUCCEEDED(comResult))
                CoUninitialize();
        };
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator)); // COM 呼び出しの結果
        if (SUCCEEDED(result) && !endpointId.isEmpty())
            result = enumerator->GetDevice(reinterpret_cast<LPCWSTR>(endpointId.utf16()), &device);
        if (SUCCEEDED(result) && !device)
            result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(result) && enumerator && !device) {
            qWarning("Interpreter: loopback device %s not found, using default output", qPrintable(endpointId));
            result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        if (SUCCEEDED(result))
            result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client));
        if (SUCCEEDED(result))
            result = client->GetMixFormat(&mixFormat);
        if (SUCCEEDED(result))
            result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mixFormat, nullptr);
        if (SUCCEEDED(result))
            result = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture));
        if (SUCCEEDED(result))
            result = client->Start();
        if (FAILED(result)) {
            reportError(tr("Could not capture the output device audio (error 0x%1).").arg(quint32(result), 8, 16, QLatin1Char('0')));
            cleanup();
            return;
        }

        // ミックス形式を QAudioFormat の形式に読み替える
        QAudioFormat::SampleFormat sampleFormat = QAudioFormat::Unknown; // 1 サンプルの形式
        bool isFloat = mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT; // float 形式か
        if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mixFormat)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
        if (isFloat && mixFormat->wBitsPerSample == 32)
            sampleFormat = QAudioFormat::Float;
        else if (mixFormat->wBitsPerSample == 16)
            sampleFormat = QAudioFormat::Int16;
        else if (mixFormat->wBitsPerSample == 32)
            sampleFormat = QAudioFormat::Int32;
        if (sampleFormat == QAudioFormat::Unknown) {
            reportError(tr("Unsupported output device audio format (%1 bit).").arg(mixFormat->wBitsPerSample));
            cleanup();
            return;
        }
        const int channels = mixFormat->nChannels; // チャンネル数
        const int blockAlign = mixFormat->nBlockAlign; // 1 フレームのバイト数
        MonoResampler resampler; // ループバック用の変換
        resampler.setRates(int(mixFormat->nSamplesPerSec), targetRate);

        while (m_loopbackRunning) {
            QThread::msleep(20);
            UINT32 packetFrames = 0; // 次のパケットのフレーム数
            while (m_loopbackRunning && SUCCEEDED(capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
                BYTE *data = nullptr; // パケットの先頭
                UINT32 frames = 0; // パケットのフレーム数
                DWORD flags = 0; // パケットの状態
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                    break;
                if (!m_muted && frames > 0) {
                    QVector<float> mono; // mono にした音声
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                        mono.fill(0.0f, frames);
                    else
                        mono = MonoResampler::toMono(reinterpret_cast<const char *>(data), qsizetype(frames) * blockAlign, sampleFormat, channels);
                    QVector<float> output; // 16 kHz にした音声
                    output.reserve(mono.size() / 2);
                    resampler.process(mono.constData(), mono.size(), output);
                    if (!output.isEmpty())
                        emit samplesReady(output);
                }
                capture->ReleaseBuffer(frames);
            }
        }
        cleanup();
    });
    m_loopbackThread->start();
    return true;
#else
    emit errorOccurred(tr("Capturing the output device audio is only supported on Windows."));
    return false;
#endif
}
