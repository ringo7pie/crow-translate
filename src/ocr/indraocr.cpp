/*
 * SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "indraocr.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>

IndraOcr::IndraOcr(QObject *parent)
    : AOcrProvider(parent)
    , m_executable(defaultExecutable())
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        fail(tr("Indra OCR timed out. Increase the timeout in OCR settings; the first run may need to download models."));
    });
}

IndraOcr::~IndraOcr()
{
    stopProcess();
}

QString IndraOcr::engineName() const
{
    return QStringLiteral("indra");
}

QString IndraOcr::defaultExecutable()
{
#ifdef Q_OS_WIN
    const QString bundled = QCoreApplication::applicationDirPath() + QStringLiteral("/indra-venv/Scripts/indra-ocr.exe");
#else
    const QString bundled = QCoreApplication::applicationDirPath() + QStringLiteral("/indra-venv/bin/indra-ocr");
#endif
    return QFileInfo(bundled).isExecutable() ? bundled : QStringLiteral("indra-ocr");
}

bool IndraOcr::isConfigured() const
{
    return !QStandardPaths::findExecutable(m_executable).isEmpty();
}

void IndraOcr::setExecutable(const QString &executable)
{
    m_executable = executable.trimmed().isEmpty() ? defaultExecutable() : executable.trimmed();
}

void IndraOcr::setModelDirectory(const QString &directory)
{
    m_modelDirectory = directory;
}

void IndraOcr::setTimeout(int seconds)
{
    m_timeout = qBound(1, seconds, 3600);
}

void IndraOcr::recognize(const QImage &image, int dpi)
{
    Q_UNUSED(dpi)
    stopProcess();
    if (!isConfigured()) {
        emit failed(tr("Indra OCR was not found. Install indra-ocr and select its executable in OCR settings."));
        return;
    }

    m_directory = std::make_unique<QTemporaryDir>();
    const QString inputPath = m_directory->filePath(QStringLiteral("capture.png"));
    if (!m_directory->isValid() || image.isNull() || !image.save(inputPath, "PNG")) {
        fail(tr("Unable to save the image for Indra OCR."));
        return;
    }

    QStringList arguments{inputPath, QStringLiteral("--format"), QStringLiteral("text"),
                          QStringLiteral("--mode"), QStringLiteral("ocr"), QStringLiteral("--cores"), QStringLiteral("1")};
    if (!m_modelDirectory.isEmpty()) {
        arguments << QStringLiteral("--model-dir") << m_modelDirectory;
    }

    m_process = new QProcess(this);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    environment.insert(QStringLiteral("HF_HUB_DISABLE_PROGRESS_BARS"), QStringLiteral("1"));
    m_process->setProcessEnvironment(environment);
    m_process->setWorkingDirectory(m_directory->path());
    connect(m_process, &QProcess::started, this, &AOcrProvider::started);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            fail(tr("Unable to start Indra OCR: %1").arg(m_process->errorString()));
        }
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
        const QString text = QString::fromUtf8(m_process->readAllStandardOutput()).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).trimmed();
        const QString diagnostics = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        if (status != QProcess::NormalExit || exitCode != 0) {
            fail(tr("Indra OCR failed (exit code %1): %2").arg(exitCode).arg(diagnostics.right(4000)));
            return;
        }
        stopProcess();
        emit recognized(text);
    });
    m_timer.start(m_timeout * 1000);
    m_process->start(QStandardPaths::findExecutable(m_executable), arguments);
}

void IndraOcr::stopProcess()
{
    m_timer.stop();
    if (m_process != nullptr) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_directory.reset();
}

void IndraOcr::fail(const QString &error)
{
    stopProcess();
    emit failed(error);
}

void IndraOcr::cancel()
{
    if (m_process == nullptr) {
        return;
    }
    stopProcess();
    emit canceled();
}
