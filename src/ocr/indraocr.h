/*
 * SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INDRAOCR_H
#define INDRAOCR_H

#include "aocrprovider.h"

#include <QTimer>
#include <memory>

class QProcess;
class QTemporaryDir;

// Runs the official local CLI. stdout is exclusively UTF-8 plain text;
// diagnostics and model-download progress stay on stderr.
class IndraOcr : public AOcrProvider
{
    Q_OBJECT
    Q_DISABLE_COPY(IndraOcr)

public:
    explicit IndraOcr(QObject *parent = nullptr);
    ~IndraOcr() override;

    QString engineName() const override;
    bool isConfigured() const override;
    void recognize(const QImage &image, int dpi) override;
    void cancel() override;

    void setExecutable(const QString &executable);
    void setModelDirectory(const QString &directory);
    void setTimeout(int seconds);
    static QString defaultExecutable();

private:
    void stopProcess();
    void fail(const QString &error);

    QString m_executable;
    QString m_modelDirectory;
    int m_timeout = 300;
    QProcess *m_process = nullptr;
    std::unique_ptr<QTemporaryDir> m_directory;
    QTimer m_timer;
};

#endif // INDRAOCR_H
