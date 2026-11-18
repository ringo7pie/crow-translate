/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "voicesegmenter.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int sampleRate = 16000; // 入力のサンプル周波数
constexpr int frameMs = 20; // 判定の単位（ミリ秒）
constexpr int frameSamples = sampleRate * frameMs / 1000; // 1 フレームのサンプル数
constexpr float noiseFactor = 3.0f; // 騒音の何倍から発話とみなすか
constexpr int trailingKeepMs = 200; // 発話の終わりに残す無音の長さ

// ミリ秒をフレーム数にする
int framesFor(int milliseconds)
{
    return std::max(1, milliseconds / frameMs);
}
}

VoiceSegmenter::VoiceSegmenter(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<QVector<float>>();
}

void VoiceSegmenter::setSettings(const Settings &settings)
{
    m_settings = settings;
}

VoiceSegmenter::Settings VoiceSegmenter::settings() const
{
    return m_settings;
}

bool VoiceSegmenter::inSpeech() const
{
    return m_inSpeech;
}

// 音声を 20 ms ごとのフレームに分けて判定する
void VoiceSegmenter::push(const QVector<float> &samples)
{
    m_pending.append(samples);
    qsizetype offset = 0; // 処理済みの位置
    while (m_pending.size() - offset >= frameSamples) {
        processFrame(m_pending.constData() + offset);
        offset += frameSamples;
    }
    m_pending.remove(0, offset);
}

// 1 フレームを判定する
void VoiceSegmenter::processFrame(const float *frame)
{
    double energy = 0.0; // 二乗和
    for (int index = 0; index < frameSamples; ++index)
        energy += double(frame[index]) * double(frame[index]);
    const float rms = float(std::sqrt(energy / frameSamples)); // このフレームの RMS
    const float threshold = std::max(m_settings.threshold, m_noiseFloor * noiseFactor); // 実際に使う閾値
    const bool voiced = rms >= threshold; // 有音か

    if (!m_inSpeech) {
        // 無音と判定したフレームだけで騒音の床をゆっくり追う（発話の音で床が上がり、以降の発話を無音とみなすのを防ぐ）
        if (!voiced)
            m_noiseFloor = m_noiseFloor == 0.0f ? rms : m_noiseFloor * 0.95f + rms * 0.05f;
        if (voiced) {
            m_inSpeech = true;
            m_utterance = m_preRoll;
            m_preRoll.clear();
            m_silentFrames = 0;
            m_voicedFrames = 0;
        } else {
            // Qt 6 の QList::append は「ポインター＋個数」を受けないので、末尾に場所を足してから中身を写す
            m_preRoll.insert(m_preRoll.size(), frameSamples, 0.0f);
            std::copy(frame, frame + frameSamples, m_preRoll.end() - frameSamples);
            const qsizetype keep = qsizetype(m_settings.preRollMs) * sampleRate / 1000; // 残す発話前の長さ
            if (m_preRoll.size() > keep)
                m_preRoll.remove(0, m_preRoll.size() - keep);
            return;
        }
    }

    m_utterance.insert(m_utterance.size(), frameSamples, 0.0f); // 1 フレーム分の場所を足し、下で中身を写す
    std::copy(frame, frame + frameSamples, m_utterance.end() - frameSamples);
    if (voiced) {
        ++m_voicedFrames;
        m_silentFrames = 0;
    } else {
        ++m_silentFrames;
    }
    const int lengthMs = int(m_utterance.size() * 1000 / sampleRate); // 発話の長さ
    if (m_silentFrames >= framesFor(m_settings.silenceMs) || lengthMs >= m_settings.maxUtteranceMs)
        finishUtterance();
}

// 発話を確定させて出す
void VoiceSegmenter::finishUtterance()
{
    if (!m_inSpeech)
        return;
    // 終わりの無音は少しだけ残して切る
    const int dropFrames = std::max(0, m_silentFrames - framesFor(trailingKeepMs)); // 切る無音のフレーム数
    m_utterance.resize(std::max<qsizetype>(0, m_utterance.size() - qsizetype(dropFrames) * frameSamples));
    const bool longEnough = m_voicedFrames * frameMs >= m_settings.minUtteranceMs; // 捨てずに出す長さか
    QVector<float> utterance; // 出す発話
    utterance.swap(m_utterance);
    m_inSpeech = false;
    m_silentFrames = 0;
    m_voicedFrames = 0;
    if (longEnough)
        emit utteranceReady(utterance);
}

void VoiceSegmenter::flush()
{
    finishUtterance();
    m_pending.clear();
}

void VoiceSegmenter::reset()
{
    m_pending.clear();
    m_preRoll.clear();
    m_utterance.clear();
    m_inSpeech = false;
    m_silentFrames = 0;
    m_voicedFrames = 0;
}
