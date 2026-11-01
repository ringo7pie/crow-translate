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
}
