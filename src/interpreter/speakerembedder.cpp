/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "speakerembedder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>

#include <cstdint>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
// sherpa-onnx の c-api.h（v2.0.1）の SherpaOnnxSpeakerEmbeddingExtractorConfig と同じ並び
struct ExtractorConfig {
    const char *model; // モデルのパス（UTF-8）
    int32_t num_threads; // 推論のスレッド数
    int32_t debug; // 1 ならログを出す
    const char *provider; // "cpu" など
};

using CreateExtractor = const void *(*)(const ExtractorConfig *); // SherpaOnnxCreateSpeakerEmbeddingExtractor
using DestroyExtractor = void (*)(const void *); // SherpaOnnxDestroySpeakerEmbeddingExtractor
using ExtractorDim = int32_t (*)(const void *); // SherpaOnnxSpeakerEmbeddingExtractorDim
using CreateStream = const void *(*)(const void *); // SherpaOnnxSpeakerEmbeddingExtractorCreateStream
using AcceptWaveform = void (*)(const void *, int32_t, const float *, int32_t); // SherpaOnnxOnlineStreamAcceptWaveform
using InputFinished = void (*)(const void *); // SherpaOnnxOnlineStreamInputFinished
using IsReady = int32_t (*)(const void *, const void *); // SherpaOnnxSpeakerEmbeddingExtractorIsReady
using ComputeEmbedding = const float *(*)(const void *, const void *); // SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding
using DestroyEmbedding = void (*)(const float *); // SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding
using DestroyStream = void (*)(const void *); // SherpaOnnxDestroyOnlineStream
}

struct SpeakerEmbedder::Api {
    CreateExtractor createExtractor = nullptr; // 抽出器を作る
    DestroyExtractor destroyExtractor = nullptr; // 抽出器を消す
    ExtractorDim dimension = nullptr; // 次元を得る
    CreateStream createStream = nullptr; // 入力の流れを作る
    AcceptWaveform acceptWaveform = nullptr; // 音声を渡す
    InputFinished inputFinished = nullptr; // 入力の終わりを知らせる
    IsReady isReady = nullptr; // 計算できるか
    ComputeEmbedding computeEmbedding = nullptr; // 埋め込みを求める
    DestroyEmbedding destroyEmbedding = nullptr; // 埋め込みを解放する
    DestroyStream destroyStream = nullptr; // 入力の流れを消す
};

SpeakerEmbedder::SpeakerEmbedder()
    : m_api(new Api)
{
}

SpeakerEmbedder::~SpeakerEmbedder()
{
    unload();
    delete m_api;
}

bool SpeakerEmbedder::isLoaded() const
{
    return m_extractor != nullptr;
}

int SpeakerEmbedder::dimension() const
{
    return m_dimension;
}

QString SpeakerEmbedder::lastError() const
{
    return m_lastError;
}

// DLL とモデルを読み込む
bool SpeakerEmbedder::load(const QString &libraryPath, const QString &modelPath)
{
    unload();
    if (!QFileInfo::exists(libraryPath) || !QFileInfo::exists(modelPath)) {
        m_lastError = QCoreApplication::translate("SpeakerEmbedder", "The speaker identification component is not installed.");
        return false;
    }
#ifdef Q_OS_WIN
    // DLL のフォルダを先に探させ、同じフォルダの onnxruntime.dll を使う（System32 の古いものを避ける）
    const std::wstring nativePath = QDir::toNativeSeparators(QFileInfo(libraryPath).absoluteFilePath()).toStdWString(); // DLL の絶対パス
    HMODULE module = LoadLibraryExW(nativePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH); // 読み込んだ DLL
    if (!module) {
        m_lastError = QCoreApplication::translate("SpeakerEmbedder", "Could not load %1 (error %2).").arg(QDir::toNativeSeparators(libraryPath)).arg(GetLastError());
        return false;
    }
    m_library = module;
    const auto resolve = [module](const char *name) {
        return reinterpret_cast<void *>(GetProcAddress(module, name));
    };
#else
    auto *library = new QLibrary(libraryPath); // 読み込んだライブラリ
    if (!library->load()) {
        m_lastError = library->errorString();
        delete library;
        return false;
    }
    m_library = library;
    const auto resolve = [library](const char *name) {
        return reinterpret_cast<void *>(library->resolve(name));
    };
#endif
    m_api->createExtractor = reinterpret_cast<CreateExtractor>(resolve("SherpaOnnxCreateSpeakerEmbeddingExtractor"));
    m_api->destroyExtractor = reinterpret_cast<DestroyExtractor>(resolve("SherpaOnnxDestroySpeakerEmbeddingExtractor"));
    m_api->dimension = reinterpret_cast<ExtractorDim>(resolve("SherpaOnnxSpeakerEmbeddingExtractorDim"));
    m_api->createStream = reinterpret_cast<CreateStream>(resolve("SherpaOnnxSpeakerEmbeddingExtractorCreateStream"));
    m_api->acceptWaveform = reinterpret_cast<AcceptWaveform>(resolve("SherpaOnnxOnlineStreamAcceptWaveform"));
    m_api->inputFinished = reinterpret_cast<InputFinished>(resolve("SherpaOnnxOnlineStreamInputFinished"));
    m_api->isReady = reinterpret_cast<IsReady>(resolve("SherpaOnnxSpeakerEmbeddingExtractorIsReady"));
    m_api->computeEmbedding = reinterpret_cast<ComputeEmbedding>(resolve("SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding"));
    m_api->destroyEmbedding = reinterpret_cast<DestroyEmbedding>(resolve("SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding"));
    m_api->destroyStream = reinterpret_cast<DestroyStream>(resolve("SherpaOnnxDestroyOnlineStream"));
    if (!m_api->createExtractor || !m_api->destroyExtractor || !m_api->dimension || !m_api->createStream || !m_api->acceptWaveform
        || !m_api->inputFinished || !m_api->isReady || !m_api->computeEmbedding || !m_api->destroyEmbedding || !m_api->destroyStream) {
        m_lastError = QCoreApplication::translate("SpeakerEmbedder", "The speaker identification library does not have the expected functions.");
        unload();
        return false;
    }

    const QByteArray modelUtf8 = QDir::toNativeSeparators(QFileInfo(modelPath).absoluteFilePath()).toUtf8(); // モデルのパス（UTF-8）
    ExtractorConfig config{}; // 抽出器の設定
    config.model = modelUtf8.constData();
    config.num_threads = 2;
    config.debug = 0;
    config.provider = "cpu";
    m_extractor = m_api->createExtractor(&config);
    if (!m_extractor) {
        m_lastError = QCoreApplication::translate("SpeakerEmbedder", "Could not load the speaker model %1.").arg(QDir::toNativeSeparators(modelPath));
        unload();
        return false;
    }
    m_dimension = m_api->dimension(m_extractor);
    m_lastError.clear();
    return true;
}

// 埋め込みを求める
QVector<float> SpeakerEmbedder::compute(const QVector<float> &samples, int sampleRate)
{
    if (!m_extractor || samples.isEmpty())
        return {};
    const void *stream = m_api->createStream(m_extractor); // 入力の流れ
    if (!stream) {
        m_lastError = QCoreApplication::translate("SpeakerEmbedder", "Could not compute the speaker embedding.");
        return {};
    }
    m_api->acceptWaveform(stream, sampleRate, samples.constData(), int32_t(samples.size()));
    m_api->inputFinished(stream);
    QVector<float> embedding; // 求めた埋め込み
    if (m_api->isReady(m_extractor, stream)) {
        const float *values = m_api->computeEmbedding(m_extractor, stream); // C API が返す配列
        if (values) {
            embedding = QVector<float>(values, values + m_dimension);
            m_api->destroyEmbedding(values);
        }
    }
    m_api->destroyStream(stream);
    if (embedding.isEmpty())
        m_lastError = QCoreApplication::translate("SpeakerEmbedder", "The utterance was too short to compute the speaker embedding.");
    return embedding;
}

// 抽出器と DLL を解放する
void SpeakerEmbedder::unload()
{
    if (m_extractor && m_api->destroyExtractor)
        m_api->destroyExtractor(m_extractor);
    m_extractor = nullptr;
    m_dimension = 0;
    *m_api = Api();
    if (m_library) {
#ifdef Q_OS_WIN
        FreeLibrary(static_cast<HMODULE>(m_library));
#else
        auto *library = static_cast<QLibrary *>(m_library); // 読み込んだライブラリ
        library->unload();
        delete library;
#endif
        m_library = nullptr;
    }
}
