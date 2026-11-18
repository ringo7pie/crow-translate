/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "audionormalizer.h"

#include <algorithm>
#include <cmath>

// 直流分を除いて実効値をそろえる
QVector<float> AudioNormalizer::normalize(const QVector<float> &samples)
{
    if (samples.isEmpty())
        return samples;
    double sum = 0.0; // 標本の合計（直流分を求める）
    for (const float sample : samples)
        sum += sample;
    const float offset = static_cast<float>(sum / samples.size()); // 直流分
    QVector<float> output(samples.size()); // 出力
    double squareSum = 0.0; // 二乗の合計（実効値を求める）
    float peak = 0.0f; // 直流分を除いた後のピーク
    for (int index = 0; index < samples.size(); ++index) {
        const float centered = samples.at(index) - offset; // 直流分を除いた値
        output[index] = centered;
        squareSum += double(centered) * centered;
        peak = (std::max)(peak, std::abs(centered));
    }
    const float rms = static_cast<float>(std::sqrt(squareSum / samples.size())); // 実効値
    if (rms < silenceRms || peak <= 0.0f)
        return output;
    float gain = (std::min)(targetRms / rms, maximumGain); // 掛ける倍率
    gain = (std::min)(gain, peakLimit / peak); // ピークが上限を超えない倍率に抑える
    for (float &sample : output)
        sample *= gain;
    return output;
}
