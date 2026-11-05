/*
 * SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "provideroptionsmanager.h"
#include "testisolation.h"
#include "core/translationsession.h"
#include "ocr/tesseractocr.h"
#include "translator/claudecodetranslationprovider.h"

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

class ClaudeCodeProviderTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_temp;

    ProviderOptions options() const
    {
        ProviderOptions opts;
        opts.setOption("executable", QString::fromUtf8(CLAUDECODE_TEST_BINARY_PATH));
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

    void haiku45NeverReceivesAdaptiveEffort()
    {
        ClaudeCodeTranslationProvider provider;
        auto opts = options();
        opts.setOption("model", AppSettings::defaultClaudeCodeModel());
        opts.setOption("reasoningEffort", QStringLiteral("high"));
        provider.applyOptions(opts);
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
        const QJsonArray args = record().value(QStringLiteral("arguments")).toArray();
        QVERIFY(args.contains(QStringLiteral("claude-haiku-4-5-20251001")));
        QVERIFY(!args.contains(QStringLiteral("--effort")));
        QCOMPARE(static_cast<int>(ATranslationProvider::ProviderBackend::LocalModels), 4);
        QCOMPARE(static_cast<int>(ATranslationProvider::ProviderBackend::ClaudeCode), 5);
    }

#ifdef Q_OS_WIN
    void discoversNativeWithMinimalPathAndHonorsExplicitSelection()
    {
        QTemporaryDir installation;
        QVERIFY(installation.isValid());
        const QByteArray previousPath = qgetenv("PATH"), previousProfile = qgetenv("USERPROFILE");
        const auto restore = qScopeGuard([&]() {
            qputenv("PATH", previousPath);
            qputenv("USERPROFILE", previousProfile);
        });
        const QString profile = installation.filePath(QString::fromUtf8("日本語 profile with spaces"));
        const QString executable = QDir(profile).filePath(QStringLiteral(".local/bin/claude.exe"));
        QVERIFY(QDir().mkpath(QFileInfo(executable).absolutePath()));
        QVERIFY(QFile::copy(QString::fromUtf8(CLAUDECODE_TEST_BINARY_PATH), executable));
        qputenv("PATH", qgetenv("SystemRoot") + "/System32");
        qputenv("USERPROFILE", profile.toLocal8Bit());
        QCOMPARE(ClaudeCodeTranslationProvider::findClaudeCodeExecutable(), executable);
        QCOMPARE(ClaudeCodeTranslationProvider::findClaudeCodeExecutable(executable), executable);
        QVERIFY(ClaudeCodeTranslationProvider::findClaudeCodeExecutable(installation.filePath(QStringLiteral("missing.exe"))).isEmpty());
        QVERIFY(ClaudeCodeTranslationProvider::findClaudeCodeExecutable(QStringLiteral("different-cli-name")).isEmpty());
        const QNtfsPermissionCheckGuard permissions;
        QCOMPARE(ClaudeCodeTranslationProvider::findClaudeCodeExecutable(), executable);
    }
#endif
    void init()
    {
        qunsetenv("CROW_CLAUDECODE_TEST_MODE");
        qputenv("CROW_CLAUDECODE_TEST_RECORD", m_temp.filePath(QStringLiteral("record.json")).toUtf8());
        QFile::remove(m_temp.filePath(QStringLiteral("record.json")));
    }

    void unicodeAndLongInput()
    {
        qputenv("CROW_CLAUDECODE_TEST_MODE", "echo");
        ClaudeCodeTranslationProvider provider;
        auto opts = options();
#ifdef Q_OS_WIN
        const QString spacedPath = m_temp.filePath(QString::fromUtf8("ClaudeCode ツール with spaces.exe"));
        QVERIFY(QFile::copy(QString::fromUtf8(CLAUDECODE_TEST_BINARY_PATH), spacedPath));
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
        QVERIFY(args.contains(QStringLiteral("--safe-mode")));
        QVERIFY(args.contains(QStringLiteral("--no-session-persistence")));
        QVERIFY(args.contains(QStringLiteral("--strict-mcp-config")));
        QVERIFY(args.contains(QStringLiteral("test-model")));
        QVERIFY(!args.contains(QStringLiteral("--effort")));
        const auto list = args.toVariantList();
        QCOMPARE(list.at(list.indexOf(QStringLiteral("--tools")) + 1).toString(), QString());
        QVERIFY(args.contains(QStringLiteral("{\"disableAllHooks\":true,\"fallbackModel\":[]}")));
        QVERIFY(!args.contains(text));
        QVERIFY(!QDir(capture.value(QStringLiteral("directory")).toString()).exists());
    }

    void reasoningEffortPassedToCli_data()
    {
        QTest::addColumn<QString>("effort");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<bool>("detectOnly");
        for (const auto &effort : AppSettings::claudeCodeReasoningEfforts())
            QTest::newRow(effort.isEmpty() ? "cli-default" : qPrintable(effort)) << effort << effort << false;
        QTest::newRow("invalid-defaults-to-cli") << QStringLiteral("unexpected") << QString() << false;
        QTest::newRow("detection") << QStringLiteral("high") << QStringLiteral("high") << true;
    }

    void reasoningEffortPassedToCli()
    {
        QFETCH(QString, effort);
        QFETCH(QString, expected);
        QFETCH(bool, detectOnly);
        ClaudeCodeTranslationProvider provider;
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
        const auto list = args.toVariantList();
        if (expected.isEmpty()) {
            QVERIFY(!args.contains(QStringLiteral("--effort")));
        } else {
            const qsizetype index = list.indexOf(QStringLiteral("--effort"));
            QVERIFY(index > 0);
            QCOMPARE(list.at(index + 1).toString(), expected);
        }
    }

    void autodetectionAndDetectOnly()
    {
        ClaudeCodeTranslationProvider provider;
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
        for (const auto &mode : {"invalid", "empty", "wrong-language", "error", "error-envelope", "missing-structured-output", "oversized", "wrong-source", "invalid-source"})
            QTest::newRow(mode) << QByteArray(mode);
    }

    void errors()
    {
        QFETCH(QByteArray, mode);
        qputenv("CROW_CLAUDECODE_TEST_MODE", mode);
        ClaudeCodeTranslationProvider provider;
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
        ClaudeCodeTranslationProvider provider;
        auto opts = options();
        opts.setOption("executable", m_temp.filePath(QStringLiteral("missing-claudeCode")));
        provider.applyOptions(opts);
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QCOMPARE(provider.getState(), ATranslationProvider::State::Finished);
        QVERIFY(provider.getErrorString().contains(QStringLiteral("not found")));
    }

    void timeoutAndRetry()
    {
        qputenv("CROW_CLAUDECODE_TEST_MODE", "hang");
        ClaudeCodeTranslationProvider provider;
        auto opts = options();
        opts.setOption("timeout", 1);
        provider.applyOptions(opts);
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Finished);
        QVERIFY(provider.getErrorString().contains(QStringLiteral("timed out")));
        qunsetenv("CROW_CLAUDECODE_TEST_MODE");
        provider.reset();
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
    }

    void abortAndResetDetection()
    {
        qputenv("CROW_CLAUDECODE_TEST_MODE", "hang");
        ClaudeCodeTranslationProvider provider;
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
        qunsetenv("CROW_CLAUDECODE_TEST_MODE");
        provider.translate(QStringLiteral("Hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
    }

#ifdef Q_OS_WIN
    void abortKillsDescendants()
    {
        qputenv("CROW_CLAUDECODE_TEST_MODE", "tree");
        ClaudeCodeTranslationProvider provider;
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
        settings.setClaudeCodeExecutable(QString::fromUtf8(CLAUDECODE_TEST_BINARY_PATH));
        TranslationSession session;
        session.setTranslationBackend(ATranslationProvider::ProviderBackend::ClaudeCode);
        session.setSelectedLanguages(Language(QStringLiteral("en")), Language(QStringLiteral("ja")));
        session.armOcrTranslation(session.tesseractOcr());
        session.tesseractOcr()->recognized(QStringLiteral("recognized text"));
        QTRY_COMPARE(session.translator()->getState(), ATranslationProvider::State::Processed);
        QCOMPARE(session.translator()->result.translation, QStringLiteral("contract test output"));
        QCOMPARE(record().value(QStringLiteral("input")).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("recognized text"));
        QVERIFY(!session.isOcrTranslationArmed());
        session.resetTranslator();
        QFile::remove(m_temp.filePath(QStringLiteral("record.json")));
        qputenv("CROW_CLAUDECODE_TEST_MODE", "hang");
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
        settings.setClaudeCodeExecutable(QStringLiteral("my-claudeCode"));
        settings.setClaudeCodeModel(QStringLiteral("chosen-model"));
        settings.setClaudeCodeReasoningEffort(QStringLiteral("high"));
        settings.setClaudeCodeTimeout(42);
        settings.setClaudeCodeInstructions(QString::fromUtf8("用語を保持\n敬語"));
        settings.setClaudeCodePrompt(QStringLiteral("Translate faithfully."));
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::ClaudeCode);
        AppSettings reloaded;
        QCOMPARE(reloaded.translationProviderBackend(), ATranslationProvider::ProviderBackend::ClaudeCode);
        ClaudeCodeTranslationProvider provider;
        ProviderOptionsManager manager;
        const auto opts = manager.createTranslationOptionsFromSettings(&provider);
        QCOMPARE(opts->getOption("model").toString(), QStringLiteral("chosen-model"));
        QCOMPARE(reloaded.claudeCodeReasoningEffort(), QStringLiteral("high"));
        QCOMPARE(opts->getOption("reasoningEffort").toString(), QStringLiteral("high"));
        QCOMPARE(opts->getOption("executable").toString(), QStringLiteral("my-claudeCode"));
        QCOMPARE(opts->getOption("timeout").toInt(), 42);
        QCOMPARE(opts->getOption("instructions").toString(), settings.claudeCodeInstructions());
        QCOMPARE(opts->getOption("prompt").toString(), QStringLiteral("Translate faithfully."));
        provider.saveOptionToSettings(QStringLiteral("prompt"), QString());
        QCOMPARE(reloaded.claudeCodePrompt(), AppSettings::defaultClaudeCodePrompt());
        provider.saveOptionToSettings(QStringLiteral("reasoningEffort"), QStringLiteral("max"));
        QCOMPARE(reloaded.claudeCodeReasoningEffort(), QStringLiteral("max"));
    }

    void defaultSettingsUseCliReasoning()
    {
        QSettings stored;
        stored.remove(QStringLiteral("ClaudeCode/ReasoningEffort"));
        AppSettings settings;
        QCOMPARE(settings.claudeCodeReasoningEffort(), QString());
        ClaudeCodeTranslationProvider provider;
        ProviderOptionsManager manager;
        QCOMPARE(manager.createTranslationOptionsFromSettings(&provider)->getOption("reasoningEffort").toString(), QString());
        QCOMPARE(provider.getDefaultOptions()->getOption("reasoningEffort").toString(), QString());
        QVERIFY(provider.getAvailableOptions().contains(QStringLiteral("reasoningEffort")));
        stored.setValue(QStringLiteral("ClaudeCode/ReasoningEffort"), QStringLiteral("unexpected"));
        QCOMPARE(settings.claudeCodeReasoningEffort(), QString());
    }
};

int main(int argc, char **argv)
{
    // QSettings uses the registry on Windows: isolate by test application name.
    isolateTestSettings();
    QCoreApplication::setApplicationName(QStringLiteral("crow-claudeCode-provider-tests"));
    QCoreApplication app(argc, argv);
    ClaudeCodeProviderTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_claudecode_provider.moc"
