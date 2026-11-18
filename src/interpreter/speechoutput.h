/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QBuffer>
#include <QLocale>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QTimer>

class QAudioSink;
class QIODevice;
class QTextToSpeech;

// 16 kHz などの mono float を、出力デバイスの形式の PCM に変換する
namespace AudioConvert {
// 引数: mono = mono float、inputRate = その周波数、format = 出力の形式。戻り値: 出力の PCM
QByteArray fromMono(const QVector<float> &mono, int inputRate, const QAudioFormat &format);
// 出力デバイスで使う形式を選ぶ（望ましい周波数・mono float が使えればそれ、だめなら既定の形式）
QAudioFormat chooseFormat(const QAudioDevice &device, int preferredRate);
// QAudioDevice::id() から出力デバイスを探す。空・見つからなければ既定の出力
QAudioDevice findOutput(const QByteArray &deviceId);
}

// 読み上げのキュー。文を 1 つずつ合成し、指定の出力デバイスへ順番に流す
class SpeechOutput : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(SpeechOutput)
public:
    explicit SpeechOutput(QObject *parent = nullptr);
    ~SpeechOutput() override;

    // 出力デバイスを選ぶ（QAudioDevice::id()、空なら既定の出力）
    void setDevice(const QByteArray &deviceId);
    // 読み上げの速さ（-1.0〜1.0）
    void setRate(double rate);
    // 文を読み上げキューに入れる
    void enqueue(const QString &text, const QLocale &locale);
    // キューを空にして読み上げを止める
    void clear();
    // 待っている文の数
    int pending() const;
    // 指定デバイスへ流せるか（合成機能があるか）。false のときは既定の出力で say() する
    bool canRouteToDevice() const;

signals:
    // 読み上げ中かどうかが変わった
    void speakingChanged(bool speaking);
    void errorOccurred(const QString &error);

private:
    struct Item {
        QString text; // 読み上げる文
        QLocale locale; // 文の言語
    };

    void next();
    void play();
    void finishItem();
    void setSpeaking(bool speaking);

    QTextToSpeech *m_tts; // 音声合成
    QQueue<Item> m_queue; // 待っている文
    QAudioDevice m_device; // 出力デバイス
    QPointer<QAudioSink> m_sink; // 再生中の出力
    QBuffer m_buffer; // 再生中の PCM
    QVector<float> m_synthesized; // 合成中の音声（mono float）
    int m_synthesizedRate = 0; // 合成した音声の周波数
    bool m_busy = false; // 1 文を処理中か
    bool m_speaking = false; // 読み上げ中か（speakingChanged 用）
    QTimer m_releaseTimer; // 読み上げ後、少し待ってから speaking を下ろすタイマー
};

// マイクの音をそのまま出力デバイスへ流す（仮想ケーブルへ元の声も送るとき）
class AudioPassthrough : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(AudioPassthrough)
public:
    explicit AudioPassthrough(QObject *parent = nullptr);
    ~AudioPassthrough() override;

    // 引数: deviceId = 出力デバイスの QAudioDevice::id()。戻り値: 開けたか
    bool start(const QByteArray &deviceId);
    void stop();
    // 16 kHz mono float の音声を流す
    void write(const QVector<float> &samples);

private:
    QPointer<QAudioSink> m_sink; // 出力
    QIODevice *m_output = nullptr; // 書き込み先
    QAudioFormat m_format; // 出力の形式
};
