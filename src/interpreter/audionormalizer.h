/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QVector>

// 文字起こしに送る前に、発話の音量をそろえる（純関数。試験対象）
namespace AudioNormalizer {
constexpr float targetRms = 0.1f; // そろえる実効値（約 -20 dBFS）
constexpr float maximumGain = 10.0f; // 増幅の上限（雑音を持ち上げすぎない）
constexpr float peakLimit = 0.95f; // ピークの上限（割れを防ぐ）
constexpr float silenceRms = 0.001f; // これ未満の実効値は無音とみなし増幅しない

// 直流分を除き、実効値を targetRms にそろえる。増幅は maximumGain まで、ピークは peakLimit 以下に抑える
// 引数: samples = 16 kHz mono float の音声。戻り値: そろえた音声（無音ならほぼそのまま）
QVector<float> normalize(const QVector<float> &samples);
}
