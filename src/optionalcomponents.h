/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace OptionalComponents {
inline QString root()
{
#ifdef Q_OS_WIN
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty()) {
        const QDir directory(local + QStringLiteral("/Crow Translate"));
        QFile descriptor(directory.filePath(QStringLiteral("components.json")));
        if (descriptor.open(QIODevice::ReadOnly)) {
            const QString configured = QJsonDocument::fromJson(descriptor.readAll()).object().value(QStringLiteral("root")).toString();
            if (!configured.isEmpty() && QDir::isAbsolutePath(configured))
                return QDir::cleanPath(configured);
        }
        return directory.filePath(QStringLiteral("Components"));
    }
#endif
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

inline QString python(const QString &modelRoot)
{
#ifdef Q_OS_WIN
    const QString venv = modelRoot + QStringLiteral("/venv/Scripts/python.exe");
    const QString embedded = modelRoot + QStringLiteral("/venv/python.exe");
    return QFileInfo::exists(venv) || !QFileInfo::exists(embedded) ? venv : embedded;
#else
    return modelRoot + QStringLiteral("/venv/bin/python");
#endif
}

inline QString indraExecutable()
{
    return root() + QStringLiteral("/local-translation/venv/Scripts/indra-ocr.exe");
}
inline QString indraModels()
{
    return root() + QStringLiteral("/indra-models");
}

// 同時通訳で使う whisper.cpp の置き場（bin に whisper-server.exe と DLL、models にモデル）
// 戻り値: whisper のフォルダの絶対パス
inline QString whisperRoot()
{
    return root() + QStringLiteral("/speech/whisper");
}

// whisper-server の実行ファイルのパスを返す
inline QString whisperServer()
{
#ifdef Q_OS_WIN
    return whisperRoot() + QStringLiteral("/bin/whisper-server.exe");
#else
    return whisperRoot() + QStringLiteral("/bin/whisper-server");
#endif
}

// 既定の whisper モデル（large-v3-turbo の q5_0 量子化版）のパスを返す
inline QString whisperModel()
{
    return whisperRoot() + QStringLiteral("/models/ggml-large-v3-turbo-q5_0.bin");
}

// 高精度の whisper モデル（large-v3 の q5_0 量子化版）のパスを返す
inline QString whisperLargeModel()
{
    return whisperRoot() + QStringLiteral("/models/ggml-large-v3-q5_0.bin");
}

// whisper の実行ファイルとモデルが揃っているかを返す
inline bool whisperInstalled()
{
    return QFileInfo::exists(whisperServer()) && QFileInfo::exists(whisperModel());
}

// 話者の聞き分けの部品（sherpa-onnx と話者埋め込みモデル）の置き場
inline QString speakerRoot()
{
    return root() + QStringLiteral("/speech/speaker");
}

// sherpa-onnx の C API の DLL のパスを返す
inline QString speakerLibrary()
{
#ifdef Q_OS_WIN
    return speakerRoot() + QStringLiteral("/bin/sherpa-onnx-c-api.dll");
#else
    return speakerRoot() + QStringLiteral("/lib/libsherpa-onnx-c-api.so");
#endif
}

// 話者埋め込みモデル（3D-Speaker CAM++）のパスを返す
inline QString speakerModel()
{
    return speakerRoot() + QStringLiteral("/models/3dspeaker_speech_campplus_sv_zh_en_16k-common_advanced.onnx");
}

// 話者の聞き分けの部品が揃っているかを返す
inline bool speakerInstalled()
{
    return QFileInfo::exists(speakerLibrary()) && QFileInfo::exists(speakerModel());
}
}
