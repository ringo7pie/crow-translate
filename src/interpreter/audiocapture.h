/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QAudioFormat>
#include <QObject>
#include <QPointer>
#include <QVector>

#include <atomic>

class QAudioSource;
class QIODevice;
class QThread;

// 任意のサンプル周波数・チャンネル数の音声を 16 kHz mono float に変換する（線形補間）
class MonoResampler
{
public:
    // 引数: inputRate = 入力のサンプル周波数、outputRate = 出力のサンプル周波数
    void setRates(int inputRate, int outputRate = 16000);
    // mono float の入力を変換して output に足す
    void process(const float *input, qsizetype frames, QVector<float> &output);

    // 生の PCM（QAudioFormat の形式）を mono float にする。戻り値: mono float
    static QVector<float> toMono(const char *data, qsizetype bytes, QAudioFormat::SampleFormat format, int channels);

private:
    double m_step = 1.0; // 出力 1 サンプルあたりに進む入力サンプル数
    double m_position = 0.0; // 次に出す出力の、入力上の位置（直前のサンプルからの相対）
    float m_previous = 0.0f; // 直前の呼び出しの最後の入力サンプル
};

// 音声の取り込み（マイク、または PC の再生音のループバック）
// 取り込んだ音声は 16 kHz mono float で samplesReady() に出す
class AudioCapture : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(AudioCapture)
public:
    // 取り込み元の種類
    enum class Kind {
        Microphone, // マイク（入力デバイス）
        Loopback // 出力デバイスで再生中の音（WASAPI ループバック）
    };

    // 引数: kind = 取り込み元の種類、deviceId = QAudioDevice::id()（空なら既定のデバイス）
    AudioCapture(Kind kind, const QByteArray &deviceId, QObject *parent = nullptr);
    ~AudioCapture() override;

    // 取り込みを始める。失敗したら errorOccurred() を出して false を返す
    bool start();
    // 取り込みを止める
    void stop();
    // 一時停止の切り替え。止めている間の音声は捨てる（読み上げ音の再認識を防ぐ）
    void setMuted(bool muted);
    Kind kind() const;

signals:
    void samplesReady(const QVector<float> &samples);
    void errorOccurred(const QString &error);

private:
    bool startMicrophone();
    bool startLoopback();
    void readMicrophone();

    Kind m_kind; // 取り込み元の種類
    QByteArray m_deviceId; // 取り込み元のデバイス ID
    std::atomic_bool m_muted{false}; // 一時停止中か
    QPointer<QAudioSource> m_source; // マイクの取り込み
    QIODevice *m_sourceDevice = nullptr; // マイクの音声を読む先
    QAudioFormat m_format; // マイクの実際の形式
    MonoResampler m_resampler; // マイク用の変換
    QThread *m_loopbackThread = nullptr; // ループバックを読むスレッド
    std::atomic_bool m_loopbackRunning{false}; // ループバックのスレッドを続けるか
};
