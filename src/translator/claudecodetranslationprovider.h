/*
 * SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CLAUDECODETRANSLATIONPROVIDER_H
#define CLAUDECODETRANSLATIONPROVIDER_H

#include "translator/atranslationprovider.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

// One ephemeral CLI invocation per request. No conversation or repository
// context is shared between translations; authentication stays with ClaudeCode.
class ClaudeCodeTranslationProvider : public ATranslationProvider
{
    Q_OBJECT
    Q_DISABLE_COPY(ClaudeCodeTranslationProvider)
public:
    explicit ClaudeCodeTranslationProvider(QObject *parent = nullptr);
    ~ClaudeCodeTranslationProvider() override;

    QString getProviderType() const override;
    QVector<Language> supportedSourceLanguages() override;
    QVector<Language> supportedDestinationLanguages() override;
    bool supportsAutodetection() const override;
    Language detectLanguage(const QString &text) override;
    void abort() override;
    void reset() override;
    void finish() override;
    void applyOptions(const ProviderOptions &options) override;
    std::unique_ptr<ProviderOptions> getDefaultOptions() const override;
    QStringList getAvailableOptions() const override;
    ProviderCapabilities capabilities() const override;
    void saveOptionToSettings(const QString &key, const QVariant &value) override;
    static QString findClaudeCodeExecutable(const QString &configured = QStringLiteral("claude"));

public slots:
    void translate(const QString &text, const Language &destination, const Language &source) override;

private:
    void startRequest(const QString &text, bool detectOnly);
    void stopProcess();
    void fail(const QString &message);
    void readResult();

    QString m_executable;
    QString m_model;
    QString m_reasoningEffort;
    QString m_instructions;
    QString m_prompt;
    int m_timeout = 300;
    QProcess *m_process = nullptr;
    QTimer m_timer;
    std::unique_ptr<QTemporaryDir> m_directory;
    QByteArray m_diagnostics;
    QByteArray m_output;
    bool m_detectOnly = false;
#ifdef Q_OS_WIN
    void *m_job = nullptr;
#endif
};

#endif // CLAUDECODETRANSLATIONPROVIDER_H
