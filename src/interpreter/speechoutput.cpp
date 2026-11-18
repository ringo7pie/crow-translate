/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "speechoutput.h"

#include "audiocapture.h"

#include <QAudioSink>
#include <QMediaDevices>
#include <QTextToSpeech>

#include <algorithm>
#include <cstring>

namespace {
constexpr int releaseDelayMs = 400; // 読み上げ後、取り込みを戻すまでの余韻（ミリ秒）
constexpr int passthroughRate = 16000; // 素通しする音声の周波数
}

// mono float を出力形式の PCM にする
QByteArray AudioConvert::fromMono(const QVector<float> &mono, int inputRate, const QAudioFormat &format)
{
    QVector<float> resampled; // 出力の周波数にした音声
    if (inputRate == format.sampleRate()) {
        resampled = mono;
    } else {
        MonoResampler resampler; // 周波数の変換
        resampler.setRates(inputRate, format.sampleRate());
        resampled.reserve(qsizetype(double(mono.size()) * format.sampleRate() / std::max(1, inputRate)) + 4);
        resampler.process(mono.constData(), mono.size(), resampled);
    }
    const int channels = std::max(1, format.channelCount()); // 出力のチャンネル数
    const int bytesPerSample = format.bytesPerSample(); // 1 サンプルのバイト数
    QByteArray pcm(resampled.size() * channels * bytesPerSample, Qt::Uninitialized); // 出力の PCM
    char *cursor = pcm.data(); // 書き込み位置
    for (const float value : std::as_const(resampled)) {
        const float clamped = std::clamp(value, -1.0f, 1.0f); // 範囲内にした値
        for (int channel = 0; channel < channels; ++channel) {
            switch (format.sampleFormat()) {
            case QAudioFormat::UInt8:
                *reinterpret_cast<quint8 *>(cursor) = quint8(clamped * 127.0f + 128.0f);
                break;
            case QAudioFormat::Int16: {
                const qint16 sample = qint16(clamped * 32767.0f); // 16 bit の値
                std::memcpy(cursor, &sample, sizeof(sample));
                break;
            }
            case QAudioFormat::Int32: {
                const qint32 sample = qint32(double(clamped) * 2147483647.0); // 32 bit の値
                std::memcpy(cursor, &sample, sizeof(sample));
                break;
            }
            case QAudioFormat::Float:
                std::memcpy(cursor, &clamped, sizeof(clamped));
                break;
            default:
                break;
            }
            cursor += bytesPerSample;
        }
    }
    return pcm;
}

// 出力デバイスの形式を選ぶ
QAudioFormat AudioConvert::chooseFormat(const QAudioDevice &device, int preferredRate)
{
    QAudioFormat format; // 望ましい形式
    format.setSampleRate(preferredRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Float);
    if (device.isFormatSupported(format))
        return format;
    format = device.preferredFormat();
    if (format.sampleFormat() == QAudioFormat::Unknown)
        format.setSampleFormat(QAudioFormat::Float);
    return format;
}

// 出力デバイスを探す
QAudioDevice AudioConvert::findOutput(const QByteArray &deviceId)
{
    if (!deviceId.isEmpty()) {
        const auto outputs = QMediaDevices::audioOutputs(); // 出力デバイスの一覧
        for (const QAudioDevice &device : outputs) {
            if (device.id() == deviceId)
                return device;
        }
    }
    return QMediaDevices::defaultAudioOutput();
}

SpeechOutput::SpeechOutput(QObject *parent)
    : QObject(parent)
    , m_tts(new QTextToSpeech(this))
    , m_device(QMediaDevices::defaultAudioOutput())
{
    m_releaseTimer.setSingleShot(true);
    m_releaseTimer.setInterval(releaseDelayMs);
    connect(&m_releaseTimer, &QTimer::timeout, this, [this]() {
        if (!m_busy && m_queue.isEmpty())
            setSpeaking(false);
    });
    connect(m_tts, &QTextToSpeech::stateChanged, this, [this](QTextToSpeech::State state) {
        if (!m_busy)
            return;
        if (state == QTextToSpeech::Error) {
            emit errorOccurred(m_tts->errorString());
            finishItem();
            return;
        }
        if (state != QTextToSpeech::Ready)
            return;
        if (canRouteToDevice())
            play(); // 合成が終わったので再生する
        else
            finishItem(); // say() の読み上げが終わった
    });
}

SpeechOutput::~SpeechOutput()
{
    clear();
}

void SpeechOutput::setDevice(const QByteArray &deviceId)
{
    m_device = AudioConvert::findOutput(deviceId);
}

void SpeechOutput::setRate(double rate)
{
    m_tts->setRate(std::clamp(rate, -1.0, 1.0));
}

bool SpeechOutput::canRouteToDevice() const
{
    return m_tts->engineCapabilities().testFlag(QTextToSpeech::Capability::Synthesize);
}

int SpeechOutput::pending() const
{
    return int(m_queue.size()) + (m_busy ? 1 : 0);
}

// 文をキューに入れる
void SpeechOutput::enqueue(const QString &text, const QLocale &locale)
{
    if (text.trimmed().isEmpty())
        return;
    m_queue.enqueue({text, locale});
    if (!m_busy)
        next();
}

// キューを空にして止める
void SpeechOutput::clear()
{
    m_queue.clear();
    m_busy = false;
    m_tts->stop();
    if (m_sink) {
        m_sink->disconnect(this);
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
    }
    m_buffer.close();
    setSpeaking(false);
}

// 次の文の合成を始める
void SpeechOutput::next()
{
    if (m_queue.isEmpty()) {
        m_releaseTimer.start();
        return;
    }
    const Item item = m_queue.dequeue(); // 今回読み上げる文
    m_busy = true;
    m_releaseTimer.stop();
    setSpeaking(true);
    if (m_tts->locale() != item.locale)
        m_tts->setLocale(item.locale);
    if (!canRouteToDevice()) {
        m_tts->say(item.text);
        return;
    }
    m_synthesized.clear();
    m_synthesizedRate = 0;
    m_tts->synthesize(item.text, this, [this](const QAudioFormat &format, const QByteArray &bytes) {
        m_synthesizedRate = format.sampleRate();
        m_synthesized.append(MonoResampler::toMono(bytes.constData(), bytes.size(), format.sampleFormat(), format.channelCount()));
    });
}

// 合成した音声を出力デバイスへ流す
void SpeechOutput::play()
{
    if (m_synthesized.isEmpty() || m_synthesizedRate <= 0) {
        finishItem();
        return;
    }
    const QAudioFormat format = AudioConvert::chooseFormat(m_device, m_synthesizedRate); // 出力の形式
    m_buffer.close();
    m_buffer.setData(AudioConvert::fromMono(m_synthesized, m_synthesizedRate, format));
    m_buffer.open(QIODevice::ReadOnly);
    m_synthesized.clear();
    m_sink = new QAudioSink(m_device, format, this);
    connect(m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
        if (!m_sink)
            return;
        if (state == QAudio::IdleState || (state == QAudio::StoppedState && m_sink->error() != QAudio::NoError))
            finishItem();
    });
    m_sink->start(&m_buffer);
    if (m_sink->error() != QAudio::NoError) {
        emit errorOccurred(tr("Could not play speech on \"%1\".").arg(m_device.description()));
        finishItem();
    }
}

// 1 文の処理を終えて次へ進む
void SpeechOutput::finishItem()
{
    if (m_sink) {
        QAudioSink *sink = m_sink; // 片付ける出力
        m_sink = nullptr;
        sink->disconnect(this);
        sink->stop();
        sink->deleteLater();
    }
    m_buffer.close();
    m_busy = false;
    next();
}

void SpeechOutput::setSpeaking(bool speaking)
{
    if (m_speaking == speaking)
        return;
    m_speaking = speaking;
    emit speakingChanged(speaking);
}

AudioPassthrough::AudioPassthrough(QObject *parent)
    : QObject(parent)
{
}

AudioPassthrough::~AudioPassthrough()
{
    stop();
}

// 素通しの出力を開く
bool AudioPassthrough::start(const QByteArray &deviceId)
{
    stop();
    const QAudioDevice device = AudioConvert::findOutput(deviceId); // 出力デバイス
    if (device.isNull())
        return false;
    m_format = AudioConvert::chooseFormat(device, passthroughRate);
    m_sink = new QAudioSink(device, m_format, this);
    m_sink->setBufferSize(m_format.bytesForDuration(300000));
    m_output = m_sink->start();
    return m_output != nullptr && m_sink->error() == QAudio::NoError;
}

void AudioPassthrough::stop()
{
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
    }
    m_output = nullptr;
}

// 音声を流す。出力が詰まっているときは遅延を増やさないよう捨てる
void AudioPassthrough::write(const QVector<float> &samples)
{
    if (!m_output || !m_sink)
        return;
    const QByteArray pcm = AudioConvert::fromMono(samples, passthroughRate, m_format); // 出力の PCM
    if (m_sink->bytesFree() >= pcm.size())
        m_output->write(pcm);
}
