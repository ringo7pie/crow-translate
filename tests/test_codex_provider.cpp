/*
 * SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "provideroptionsmanager.h"
#include "testisolation.h"
#include "core/translationsession.h"
#include "ocr/tesseractocr.h"
#include "translator/codextranslationprovider.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QScopeGuard>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTest>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

class CodexProviderTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_temp;

    ProviderOptions options() const
    {
        ProviderOptions opts;
        opts.setOption("executable", QString::fromUtf8(CODEX_TEST_BINARY_PATH));
        opts.setOption("model", QStringLiteral("test-model"));
        opts.setOption("timeout", 5);
        return opts;
    }

    QJsonObject record() const
    {
        QFile file(m_temp.filePath(QStringLiteral("record.json")));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }

private slots:
#ifdef Q_OS_WIN
    void discoversStandaloneWithoutProcessPathAndHonorsExplicitPaths()
    {
        QTemporaryDir installation;
        const QByteArray oldPath = qgetenv("PATH"), oldLocal = qgetenv("LOCALAPPDATA"), oldInstall = qgetenv("CODEX_INSTALL_DIR");
        const auto restore = qScopeGuard([&]() { qputenv("PATH", oldPath); qputenv("LOCALAPPDATA", oldLocal); qputenv("CODEX_INSTALL_DIR", oldInstall); });
        qputenv("PATH", qgetenv("SystemRoot") + "/System32");
        qputenv("LOCALAPPDATA", installation.path().toUtf8());
        qunsetenv("CODEX_INSTALL_DIR");
        const QString executable = installation.filePath(QStringLiteral("Programs/OpenAI/Codex/bin/codex.exe"));
        QVERIFY(QDir().mkpath(QFileInfo(executable).absolutePath()));
        QVERIFY(QFile::copy(QString::fromUtf8(CODEX_TEST_BINARY_PATH), executable));
        QCOMPARE(CodexTranslationProvider::findCodexExecutable(), executable);
        QCOMPARE(CodexTranslationProvider::findCodexExecutable(executable), executable);
        QVERIFY(CodexTranslationProvider::findCodexExecutable(installation.filePath(QStringLiteral("missing.exe"))).isEmpty());
    }
#endif
    void init()
    {
        qunsetenv("CROW_CODEX_TEST_MODE");
        qputenv("CROW_CODEX_TEST_RECORD", m_temp.filePath(QStringLiteral("record.json")).toUtf8());
        QFile::remove(m_temp.filePath(QStringLiteral("record.json")));
    }

    void unicodeAndLongInput()
    {
        qputenv("CROW_CODEX_TEST_MODE", "echo");
        CodexTranslationProvider provider;
        auto opts = options();
#ifdef Q_OS_WIN
        const QString spacedPath = m_temp.filePath(QString::fromUtf8("Codex ツール with spaces.exe"));
        QVERIFY(QFile::copy(QString::fromUtf8(CODEX_TEST_BINARY_PATH), spacedPath));
        opts.setOption("executable", spacedPath);
#endif
        opts.setOption("instructions", QString::fromUtf8("敬語で翻訳"));
        const QString basic = QString::fromUtf8("意味を保って翻訳する。\nBraces {text}, quotes \" and $() stay literal.");
        opts.setOption("prompt", basic);
        provider.applyOptions(opts);
        const QString text = QString::fromUtf8("  日本語\n\"quotes\" ` $() <tag>\n") + QString(100000, QLatin1Char('x')) + QStringLiteral("\n ");
        provider.translate(text, Language(QStringLiteral("en")), Language(QStringLiteral("ja")));
        QTRY_VERIFY2(provider.getState() == ATranslationProvider::State::Processed, qPrintable(provider.getErrorString()));
        QCOMPARE(provider.result.translation, text);
        const QJsonObject capture = record();
        const QJsonObject request = capture.value(QStringLiteral("input")).toObject();
        QCOMPARE(request.value(QStringLiteral("text")).toString(), text);
        QCOMPARE(request.value(QStringLiteral("translation_preferences")).toString(), QString::fromUtf8("敬語で翻訳"));
        QCOMPARE(request.value(QStringLiteral("translation_instructions")).toString(), basic);
        QVERIFY(capture.value(QStringLiteral("prompt")).toString().startsWith(QStringLiteral("Do not use tools")));
        QVERIFY(capture.value(QStringLiteral("prompt")).toString().contains(QStringLiteral("never change the output schema")));
        const QJsonArray args = capture.value(QStringLiteral("arguments")).toArray();
        QVERIFY(args.contains(QStringLiteral("--ignore-user-config")));
        QVERIFY(args.contains(QStringLiteral("--ephemeral")));
        QVERIFY(args.contains(QStringLiteral("read-only")));
        QVERIFY(args.contains(QStringLiteral("test-model")));
        QVERIFY(args.contains(QStringLiteral("model_reasoning_effort=\"low\"")));
        QVERIFY(!args.contains(text));
        QVERIFY(!QDir(capture.value(QStringLiteral("directory")).toString()).exists());
    }

    void reasoningEffortPassedToCli_data()
    {
        QTest::addColumn<QString>("effort");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<bool>("detectOnly");
        for (const auto &effort : AppSettings::codexReasoningEfforts())
            QTest::newRow(qPrintable(effort)) << effort << effort << false;
        QTest::newRow("invalid-defaults-to-low") << QStringLiteral("unexpected") << QStringLiteral("low") << false;
        QTest::newRow("detection") << QStringLiteral("high") << QStringLiteral("high") << true;
    }

    void reasoningEffortPassedToCli()
    {
        QFETCH(QString, effort);
        QFETCH(QString, expected);
        QFETCH(bool, detectOnly);
        CodexTranslationProvider provider;
        auto opts = options();
        opts.setOption("reasoningEffort", effort);
        provider.applyOptions(opts);
        QSignalSpy detected(&provider, &ATranslationProvider::languageDetected);
        if (detectOnly) {
            provider.detectLanguage(QStringLiteral("Hello"));
            QTRY_COMPARE(detected.size(), 1);
        } else {
            provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
            QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
        }
        const QJsonArray args = record().value(QStringLiteral("arguments")).toArray();
        const QString config = QStringLiteral("model_reasoning_effort=\"%1\"").arg(expected);
        QVERIFY(args.contains(config));
        const qsizetype index = args.toVariantList().indexOf(config);
        QVERIFY(index > 0);
        QCOMPARE(args.at(index - 1).toString(), QStringLiteral("-c"));
    }

    void autodetectionAndDetectOnly()
    {
        CodexTranslationProvider provider;
        provider.applyOptions(options());
        QSignalSpy detected(&provider, &ATranslationProvider::languageDetected);
        QSignalSpy started(&provider, &ATranslationProvider::detectionStarted);
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
        QCOMPARE(provider.sourceLanguage, Language(QStringLiteral("en")));
        QCOMPARE(detected.size(), 1);
        QCOMPARE(detected.at(0).at(1).toBool(), true);
        provider.reset();
        QCOMPARE(provider.detectLanguage(QStringLiteral("Hello")), Language::autoLanguage());
        QCOMPARE(started.size(), 1);
        QTRY_COMPARE(detected.size(), 2);
        QCOMPARE(detected.at(1).at(1).toBool(), false);
        QCOMPARE(provider.getState(), ATranslationProvider::State::Ready);
        QVERIFY(provider.result.isEmpty());
    }

    void errors_data()
    {
        QTest::addColumn<QByteArray>("mode");
        for (const auto &mode : {"invalid", "empty", "wrong-language", "error"})
            QTest::newRow(mode) << QByteArray(mode);
    }

    void errors()
    {
        QFETCH(QByteArray, mode);
        qputenv("CROW_CODEX_TEST_MODE", mode);
        CodexTranslationProvider provider;
        provider.applyOptions(options());
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("en")), Language(QStringLiteral("es")));
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Finished);
        QCOMPARE(provider.error, ATranslationProvider::TranslationError::Custom);
        QVERIFY(!provider.getErrorString().isEmpty());
        QVERIFY(provider.result.isEmpty());
        QVERIFY(!QDir(record().value(QStringLiteral("directory")).toString()).exists());
    }

    void missingExecutable()
    {
        CodexTranslationProvider provider;
        auto opts = options();
        opts.setOption("executable", m_temp.filePath(QStringLiteral("missing-codex")));
        provider.applyOptions(opts);
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QCOMPARE(provider.getState(), ATranslationProvider::State::Finished);
        QVERIFY(provider.getErrorString().contains(QStringLiteral("not found")));
    }

    void timeoutAndRetry()
    {
        qputenv("CROW_CODEX_TEST_MODE", "hang");
        CodexTranslationProvider provider;
        auto opts = options();
        opts.setOption("timeout", 1);
        provider.applyOptions(opts);
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Finished);
        QVERIFY(provider.getErrorString().contains(QStringLiteral("timed out")));
        qunsetenv("CROW_CODEX_TEST_MODE");
        provider.reset();
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
    }

    void abortAndResetDetection()
    {
        qputenv("CROW_CODEX_TEST_MODE", "hang");
        CodexTranslationProvider provider;
        provider.applyOptions(options());
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_VERIFY(!record().isEmpty());
        const QString directory = record().value(QStringLiteral("directory")).toString();
        QSignalSpy states(&provider, &ATranslationProvider::stateChanged);
        provider.abort();
        QCOMPARE(provider.error, ATranslationProvider::TranslationError::Aborted);
        QCOMPARE(states.size(), 1);
        QVERIFY(!QDir(directory).exists());
        provider.reset();
        provider.detectLanguage(QStringLiteral("Hello"));
        provider.reset();
        qunsetenv("CROW_CODEX_TEST_MODE");
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
    }

#ifdef Q_OS_WIN
    void abortKillsDescendants()
    {
        qputenv("CROW_CODEX_TEST_MODE", "tree");
        CodexTranslationProvider provider;
        provider.applyOptions(options());
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_VERIFY(record().contains(QStringLiteral("child_pid")));
        const auto pid = static_cast<DWORD>(record().value(QStringLiteral("child_pid")).toInteger());
        HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, pid);
        QVERIFY(child != nullptr);
        provider.abort();
        QCOMPARE(WaitForSingleObject(child, 3000), DWORD(WAIT_OBJECT_0));
        CloseHandle(child);
    }
#endif

    void ocrChainingAndBackendSwitch()
    {
        AppSettings settings;
        settings.setCodexExecutable(QString::fromUtf8(CODEX_TEST_BINARY_PATH));
        TranslationSession session;
        session.setTranslationBackend(ATranslationProvider::ProviderBackend::Codex);
        session.setSelectedLanguages(Language(QStringLiteral("en")), Language(QStringLiteral("ja")));
        session.armOcrTranslation(session.tesseractOcr());
        session.tesseractOcr()->recognized(QStringLiteral("recognized text"));
        QTRY_COMPARE(session.translator()->getState(), ATranslationProvider::State::Processed);
        QCOMPARE(session.translator()->result.translation, QStringLiteral("contract test output"));
        QCOMPARE(record().value(QStringLiteral("input")).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("recognized text"));
        QVERIFY(!session.isOcrTranslationArmed());
        session.resetTranslator();
        QFile::remove(m_temp.filePath(QStringLiteral("record.json")));
        qputenv("CROW_CODEX_TEST_MODE", "hang");
        session.requestTranslation(QStringLiteral("next"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_VERIFY(!record().isEmpty());
        const QString directory = record().value(QStringLiteral("directory")).toString();
        QSignalSpy stopped(session.translator(), &ATranslationProvider::stateChanged);
        session.setTranslationBackend(ATranslationProvider::ProviderBackend::Copy);
        QVERIFY(!QDir(directory).exists());
        QCOMPARE(stopped.size(), 1);
    }

    void settingsRoundTrip()
    {
        AppSettings settings;
        settings.setCodexExecutable(QStringLiteral("my-codex"));
        settings.setCodexModel(QStringLiteral("chosen-model"));
        settings.setCodexReasoningEffort(QStringLiteral("high"));
        settings.setCodexTimeout(42);
        settings.setCodexInstructions(QString::fromUtf8("用語を保持\n敬語"));
        settings.setCodexPrompt(QStringLiteral("Translate faithfully."));
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Codex);
        AppSettings reloaded;
        QCOMPARE(reloaded.translationProviderBackend(), ATranslationProvider::ProviderBackend::Codex);
        CodexTranslationProvider provider;
        ProviderOptionsManager manager;
        const auto opts = manager.createTranslationOptionsFromSettings(&provider);
        QCOMPARE(opts->getOption("model").toString(), QStringLiteral("chosen-model"));
        QCOMPARE(reloaded.codexReasoningEffort(), QStringLiteral("high"));
        QCOMPARE(opts->getOption("reasoningEffort").toString(), QStringLiteral("high"));
        QCOMPARE(opts->getOption("executable").toString(), QStringLiteral("my-codex"));
        QCOMPARE(opts->getOption("timeout").toInt(), 42);
        QCOMPARE(opts->getOption("instructions").toString(), settings.codexInstructions());
        QCOMPARE(opts->getOption("prompt").toString(), QStringLiteral("Translate faithfully."));
        provider.saveOptionToSettings(QStringLiteral("prompt"), QString());
        QCOMPARE(reloaded.codexPrompt(), AppSettings::defaultCodexPrompt());
        provider.saveOptionToSettings(QStringLiteral("reasoningEffort"), QStringLiteral("max"));
        QCOMPARE(reloaded.codexReasoningEffort(), QStringLiteral("max"));
    }

    void legacySettingsUseLowReasoning()
    {
        QSettings stored;
        stored.remove(QStringLiteral("Codex/ReasoningEffort"));
        AppSettings settings;
        QCOMPARE(settings.codexReasoningEffort(), QStringLiteral("low"));
        CodexTranslationProvider provider;
        ProviderOptionsManager manager;
        QCOMPARE(manager.createTranslationOptionsFromSettings(&provider)->getOption("reasoningEffort").toString(), QStringLiteral("low"));
        QCOMPARE(provider.getDefaultOptions()->getOption("reasoningEffort").toString(), QStringLiteral("low"));
        QVERIFY(provider.getAvailableOptions().contains(QStringLiteral("reasoningEffort")));
        stored.setValue(QStringLiteral("Codex/ReasoningEffort"), QStringLiteral("unexpected"));
        QCOMPARE(settings.codexReasoningEffort(), QStringLiteral("low"));
    }
};

int main(int argc, char **argv)
{
    // QSettings uses the registry on Windows: isolate by test application name.
    isolateTestSettings();
    QCoreApplication::setApplicationName(QStringLiteral("crow-codex-provider-tests"));
    QCoreApplication app(argc, argv);
    CodexProviderTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_codex_provider.moc"
