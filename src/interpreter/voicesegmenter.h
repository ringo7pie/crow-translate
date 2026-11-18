/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QObject>
#include <QVector>

// エネルギー（RMS）型の発話区間検出（VAD）。16 kHz mono の音声を受け取り、発話 1 つごとに utteranceReady() を出す
class VoiceSegmenter : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(VoiceSegmenter)
public:
    // 区切りの調整値
    struct Settings {
        float threshold = 0.012f; // 発話とみなす RMS の下限（騒音の床に応じて自動で上げる）
        int silenceMs = 600; // この長さの無音で発話を区切る
        int maxUtteranceMs = 15000; // 発話がこれより長いと強制的に区切る
        int minUtteranceMs = 300; // これより短い発話は捨てる
        int preRollMs = 200; // 発話の始まりの前に含める長さ
    };

    explicit VoiceSegmenter(QObject *parent = nullptr);

    void setSettings(const Settings &settings);
    Settings settings() const;
    // 音声を足す（16 kHz mono float）
    void push(const QVector<float> &samples);
    // 途中の発話を確定させて出す（停止時に使う）
    void flush();
    // 途中の発話を捨てる
    void reset();
    // 発話の途中かを返す
    bool inSpeech() const;

signals:
    // 発話 1 つ分の音声
    void utteranceReady(const QVector<float> &samples);

private:
    void processFrame(const float *frame);
    void finishUtterance();

    Settings m_settings; // 区切りの調整値
    QVector<float> m_pending; // 20 ms のフレームに満たない残り
    QVector<float> m_preRoll; // 発話前の直近の音声
    QVector<float> m_utterance; // 発話中の音声
    bool m_inSpeech = false; // 発話中か
    int m_silentFrames = 0; // 発話中に続いた無音フレーム数
    int m_voicedFrames = 0; // 発話中の有音フレーム数
    float m_noiseFloor = 0.0f; // 推定した騒音の RMS
};
