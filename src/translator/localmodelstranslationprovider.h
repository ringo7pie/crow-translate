/* SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOCALMODELSTRANSLATIONPROVIDER_H
#define LOCALMODELSTRANSLATIONPROVIDER_H

#include "atranslationprovider.h"
#include <QJsonObject>
#include <QTimer>
#include <memory>

class QProcess;
class QTemporaryDir;

class LocalModelsTranslationProvider : public ATranslationProvider
{
    Q_OBJECT
public:
    explicit LocalModelsTranslationProvider(QObject *parent = nullptr);
    ~LocalModelsTranslationProvider() override;
    QString getProviderType() const override;
    QVector<Language> supportedSourceLanguages() override;
    QVector<Language> supportedDestinationLanguages() override;
    bool supportsAutodetection() const override;
    Language detectLanguage(const QString &text) override;
    void translate(const QString &text, const Language &destination, const Language &source) override;
    void abort() override;
    void reset() override;
    void applyOptions(const ProviderOptions &options) override;
    std::unique_ptr<ProviderOptions> getDefaultOptions() const override;
    QStringList getAvailableOptions() const override;
    ProviderCapabilities capabilities() const override;
    void saveOptionToSettings(const QString &key, const QVariant &value) override;

    static QStringList modelIds();
    static QString modelName(const QString &id);
    static QString modelFile(const QString &id);
    static QString defaultPrompt(const QString &id);
    static QString defaultRoot();
    static bool isInstalled(const QString &root, const QString &id);

private:
    void startRequest(const QString &text, bool detectOnly);
    void stopWorker();
    void readEvents();
    void handleEvent(const QJsonObject &event);
    void fail(const QString &message);
    QString m_root;
    QString m_model = QStringLiteral("index-translate-2b");
    QString m_prompt;
    QString m_instructions;
    int m_timeout = 300;
    int m_vramReserve = 256;
    QProcess *m_process = nullptr;
    std::unique_ptr<QTemporaryDir> m_workerDirectory;
    QTimer m_timer;
    QByteArray m_buffer;
    QByteArray m_diagnostics;
    QJsonObject m_command;
    qint64 m_requestId = 0;
    bool m_active = false;
    bool m_detectOnly = false;
    bool m_sourceWasAuto = false;
#ifdef Q_OS_WIN
    void *m_job = nullptr;
#endif
};
#endif
