/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "speakerclusterer.h"

#include <algorithm>
#include <cmath>

void SpeakerClusterer::setThreshold(float threshold)
{
    m_threshold = std::clamp(threshold, 0.0f, 1.0f);
}

float SpeakerClusterer::threshold() const
{
    return m_threshold;
}

void SpeakerClusterer::setMaximumSpeakers(int maximum)
{
    m_maximumSpeakers = (std::max)(1, maximum);
}

void SpeakerClusterer::reset()
{
    m_clusters.clear();
}

int SpeakerClusterer::speakerCount() const
{
    return int(m_clusters.size());
}

QVector<float> SpeakerClusterer::centroid(int index) const
{
    if (index < 0 || index >= m_clusters.size())
        return {};
    return m_clusters.at(index).centroid;
}

// 発話を話者に振り分ける
int SpeakerClusterer::assign(const QVector<float> &embedding, double durationSeconds)
{
    const QVector<float> unit = normalized(embedding); // 正規化した埋め込み
    if (unit.isEmpty())
        return -1;
    int best = -1; // 最も近い話者
    float bestSimilarity = -2.0f; // 最も近い話者との類似度
    for (int index = 0; index < m_clusters.size(); ++index) {
        const float similarity = cosine(unit, m_clusters.at(index).centroid); // この話者との類似度
        if (similarity > bestSimilarity) {
            bestSimilarity = similarity;
            best = index;
        }
    }
    const bool isShort = durationSeconds < shortUtteranceSeconds; // 短い発話か
    if (isShort)
        return best >= 0 && bestSimilarity >= m_threshold ? best : -1;

    if (best < 0 || bestSimilarity < m_threshold) {
        if (m_clusters.size() < m_maximumSpeakers) {
            Cluster cluster; // 新しい話者
            cluster.sum = unit;
            cluster.centroid = unit;
            cluster.count = 1;
            m_clusters.append(cluster);
            return int(m_clusters.size()) - 1;
        }
        // 上限に達したら最も近い人にする（重心は動かさない）
        return best;
    }
    // 同じ人: 重心を更新する
    Cluster &cluster = m_clusters[best]; // 更新する話者
    for (int dimension = 0; dimension < cluster.sum.size(); ++dimension)
        cluster.sum[dimension] += unit.at(dimension);
    cluster.centroid = normalized(cluster.sum);
    ++cluster.count;
    return best;
}

QVector<float> SpeakerClusterer::normalized(const QVector<float> &vector)
{
    double squareSum = 0.0; // 二乗の合計
    for (const float value : vector)
        squareSum += double(value) * value;
    if (squareSum <= 0.0)
        return {};
    const float scale = static_cast<float>(1.0 / std::sqrt(squareSum)); // 長さを 1 にする倍率
    QVector<float> output(vector.size()); // 出力
    for (int index = 0; index < vector.size(); ++index)
        output[index] = vector.at(index) * scale;
    return output;
}

float SpeakerClusterer::cosine(const QVector<float> &first, const QVector<float> &second)
{
    if (first.size() != second.size() || first.isEmpty())
        return 0.0f;
    double dot = 0.0; // 内積
    double firstSquare = 0.0; // first の二乗和
    double secondSquare = 0.0; // second の二乗和
    for (int index = 0; index < first.size(); ++index) {
        dot += double(first.at(index)) * second.at(index);
        firstSquare += double(first.at(index)) * first.at(index);
        secondSquare += double(second.at(index)) * second.at(index);
    }
    if (firstSquare <= 0.0 || secondSquare <= 0.0)
        return 0.0f;
    return static_cast<float>(dot / std::sqrt(firstSquare * secondSquare));
}

QString SpeakerClusterer::letterFor(int index)
{
    if (index < 0)
        return QStringLiteral("?");
    const QChar letter(QLatin1Char(char('A' + index % 26))); // 記号の文字
    const int round = index / 26; // 26 人を超えた周回
    return round == 0 ? QString(letter) : QString(letter) + QString::number(round + 1);
}
