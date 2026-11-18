/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QString>
#include <QVector>

// 話者埋め込み（声の特徴のベクトル）を、発話の順にその場で話者へ振り分ける（オンラインのクラスタリング）。
// 各話者の重心とのコサイン類似度が閾値以上なら同じ人、未満なら新しい人とみなす。純粋な計算だけで試験対象
class SpeakerClusterer
{
public:
    static constexpr float defaultThreshold = 0.5f; // 同じ人とみなすコサイン類似度の既定値
    static constexpr int defaultMaximumSpeakers = 8; // 話者の数の上限の既定値
    static constexpr double shortUtteranceSeconds = 1.0; // これより短い発話は重心を更新しない（特徴が不安定なため）

    // 閾値（0〜1。大きいほど細かく分ける）を設定する
    void setThreshold(float threshold);
    float threshold() const;
    // 話者の数の上限を設定する（1 以上）
    void setMaximumSpeakers(int maximum);
    // これまでの話者を忘れる
    void reset();
    // 見つかった話者の数を返す
    int speakerCount() const;
    // 話者の重心（正規化済み）を返す。範囲外なら空
    QVector<float> centroid(int index) const;

    // 発話の埋め込みを話者に振り分ける
    // 引数: embedding = 埋め込み、durationSeconds = 発話の長さ（秒）
    // 戻り値: 話者の番号（0 = A、1 = B …）。決められないときは -1
    int assign(const QVector<float> &embedding, double durationSeconds);

    // 長さ 1 に正規化したベクトルを返す（長さ 0 なら空）
    static QVector<float> normalized(const QVector<float> &vector);
    // 2 つのベクトルのコサイン類似度を返す（次元が違う・長さ 0 なら 0）
    static float cosine(const QVector<float> &first, const QVector<float> &second);
    // 話者の番号を A, B, … Z, A2, B2 … の記号にする。-1 なら "?"
    static QString letterFor(int index);

private:
    // 1 人の話者
    struct Cluster {
        QVector<float> sum; // 正規化した埋め込みの合計
        QVector<float> centroid; // 重心（sum を正規化したもの）
        int count = 0; // 重心に使った発話の数
    };

    QVector<Cluster> m_clusters; // 見つかった話者
    float m_threshold = defaultThreshold; // 同じ人とみなすコサイン類似度
    int m_maximumSpeakers = defaultMaximumSpeakers; // 話者の数の上限
};
