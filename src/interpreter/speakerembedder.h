/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QString>
#include <QVector>

// sherpa-onnx の C API（sherpa-onnx-c-api.dll）を実行時に読み込み、発話の話者埋め込みを求めるクラス。
// ビルド時には sherpa-onnx・ONNX Runtime に依存しない。1 つのスレッドからだけ使う（スレッド安全ではない）
class SpeakerEmbedder
{
public:
    SpeakerEmbedder();
    ~SpeakerEmbedder();
    SpeakerEmbedder(const SpeakerEmbedder &) = delete;
    SpeakerEmbedder &operator=(const SpeakerEmbedder &) = delete;

    // DLL とモデルを読み込む。引数: libraryPath = sherpa-onnx-c-api.dll、modelPath = 埋め込みモデル（.onnx）
    // 戻り値: 読み込めたか。失敗の理由は lastError() で得る
    bool load(const QString &libraryPath, const QString &modelPath);
    // 読み込み済みか
    bool isLoaded() const;
    // 埋め込みの次元（読み込み前は 0）
    int dimension() const;
    // 16 kHz mono float の音声から埋め込みを求める。失敗したら空を返す
    QVector<float> compute(const QVector<float> &samples, int sampleRate = 16000);
    // 最後の失敗の説明
    QString lastError() const;

private:
    void unload();

    struct Api; // C API の関数ポインタ
    Api *m_api; // 読み込んだ関数
    void *m_library = nullptr; // DLL のハンドル
    const void *m_extractor = nullptr; // 埋め込み抽出器
    int m_dimension = 0; // 埋め込みの次元
    QString m_lastError; // 最後の失敗の説明
};
