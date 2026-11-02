/* SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "translator/localmodelstranslationprovider.h"
#include "core/translationsession.h"
#include "modulestatus.h"
#include "optionalcomponents.h"
#include "provideroptionsmanager.h"
#include "testisolation.h"
#include <QScopeGuard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

class LocalModelsProviderTest : public QObject
{
    Q_OBJECT
    static void configure(LocalModelsTranslationProvider &provider, const QString &root)
    {
#ifdef Q_OS_WIN
        const QString python = QStringLiteral("/venv/Scripts/python.exe");
#else
        const QString python = QStringLiteral("/venv/bin/python");
#endif
        QVERIFY(QDir().mkpath(QFileInfo(root + python).absolutePath()));
        QVERIFY(QFile::copy(QString::fromUtf8(LOCALMODELS_TEST_BINARY_PATH), root + python));
        ProviderOptions options;
        options.setOption("root", root);
        provider.applyOptions(options);
    }
private slots:
    void modelInstructionsReachTheWorkerAndCanChangeWithoutReloading()
    {
        AppSettings settings;
        QTemporaryDir directory;
        LocalModelsTranslationProvider provider;
        configure(provider, directory.path());
        settings.setLocalModelsRoot(directory.path());
        settings.setLocalModelsVramReserve(384);
        ProviderOptionsManager manager;
        for (const QString &model : LocalModelsTranslationProvider::modelIds()) {
            const QString prompt = QStringLiteral("Translate {text} into {target_lang}. ") + model;
            const QString extra = QString::fromUtf8("用語を保持\n") + model;
            settings.setLocalModelsModel(model);
            settings.setLocalModelsPrompt(model, prompt);
            settings.setLocalModelsInstructions(model, extra);
            manager.applySettingsToTranslationProvider(&provider);
            for (int request = 0; request < 2; ++request) {
                if (request) {
                    ProviderOptions update;
                    update.setOption("instructions", extra + QStringLiteral(" updated"));
                    provider.applyOptions(update);
                }
                provider.translate(QStringLiteral("Hello {text}"), Language(QStringLiteral("ja")), Language::autoLanguage());
                QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
                QFile file(directory.filePath(QStringLiteral("request.json")));
                QVERIFY(file.open(QIODevice::ReadOnly));
                const auto command = QJsonDocument::fromJson(file.readAll()).object();
                QCOMPARE(command.value("prompt").toString(), prompt);
                QCOMPARE(command.value("instructions").toString(), extra + (request ? QStringLiteral(" updated") : QString()));
                QCOMPARE(command.value("text").toString(), QStringLiteral("Hello {text}"));
                QCOMPARE(command.value("vram_reserve_mib").toInt(), 384);
            }
        }
        QFile starts(directory.filePath(QStringLiteral("starts.txt")));
        QVERIFY(starts.open(QIODevice::ReadOnly));
        QCOMPARE(starts.readAll().count("started"), 4);
        settings.setLocalModelsVramReserve(256);
        for (const QString &model : LocalModelsTranslationProvider::modelIds()) {
            settings.setLocalModelsPrompt(model, QString());
            settings.setLocalModelsInstructions(model, QString());
        }
    }

    void localeVariants_data()
    {
        QTest::addColumn<Language>("source");
        QTest::addColumn<Language>("destination");
        QLocale japanese(QLocale::Japanese);
        japanese.setNumberOptions(QLocale::OmitGroupSeparator);
        const Language english(QStringLiteral("en"));
        QTest::newRow("Japanese-display-options") << Language::autoLanguage() << Language(japanese);
        QTest::newRow("British-English-target") << Language(QStringLiteral("ja")) << Language(QStringLiteral("en_GB"));
        QTest::newRow("British-English-source") << Language(QStringLiteral("en_GB")) << Language(QStringLiteral("ja"));
        QTest::newRow("Japanese-source-display-options") << Language(japanese) << english;
        const Language system(QLocale::system());
        if(system.toCode() == QLatin1String("en") || system.toCode() == QLatin1String("ja"))
            QTest::newRow("system-locale") << Language::autoLanguage() << system;
    }

    void localeVariants()
    {
        QFETCH(Language, source);
        QFETCH(Language, destination);
        QTemporaryDir directory;
        LocalModelsTranslationProvider provider;
        configure(provider, directory.path());
        provider.translate(QStringLiteral("The meeting starts tomorrow."), destination, source);
        QVERIFY2(provider.error == ATranslationProvider::TranslationError::NoError, qPrintable(provider.getErrorString()));
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
        QCOMPARE(provider.translationLanguage, destination);
        QCOMPARE(provider.sourceLanguage.toCode(), source == Language::autoLanguage() ? QStringLiteral("en") : source.toCode());
    }

    void autoDestinationFromSessionAcceptsSystemLocale()
    {
        const Language system(QLocale::system());
        if(system.toCode() != QLatin1String("en") && system.toCode() != QLatin1String("ja"))
            QSKIP("The configured local models support English and Japanese system languages");
        AppSettings settings;
        settings.setPrimaryLanguage(Language::autoLanguage());
        settings.setSecondaryLanguage(Language(QStringLiteral("en")));
        QTemporaryDir directory;
        TranslationSession session;
        session.setTranslationBackend(ATranslationProvider::ProviderBackend::LocalModels);
        auto *provider = qobject_cast<LocalModelsTranslationProvider *>(session.translator());
        QVERIFY(provider);
        configure(*provider, directory.path());
        ProviderOptions options;
        options.setOption("model", "seed-x-ppo-7b");
        provider->applyOptions(options);
        QSignalSpy started(&session, &TranslationSession::translationStarted);
        session.requestTranslation(QStringLiteral("The meeting starts tomorrow."), Language::autoLanguage(), Language::autoLanguage());
        QCOMPARE(started.count(), 1);
        QVERIFY2(provider->error == ATranslationProvider::TranslationError::NoError, qPrintable(provider->getErrorString()));
        QTRY_COMPARE(provider->getState(), ATranslationProvider::State::Processed);
        QCOMPARE(provider->translationLanguage, started.first().at(1).value<Language>());
    }

    void unsupportedLanguageIsStillRejected()
    {
        LocalModelsTranslationProvider provider;
        provider.translate(QStringLiteral("hello"), Language(QStringLiteral("fr")), Language(QStringLiteral("en")));
        QCOMPARE(provider.getState(), ATranslationProvider::State::Finished);
        QVERIFY(provider.getErrorString().contains(QStringLiteral("destination")));
    }

    void embeddedPythonCanTranslateWithoutAVenvRedirector()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        const QString root = directory.path();
        QVERIFY(QDir().mkpath(root + QStringLiteral("/venv")));
        QVERIFY(QFile::copy(QString::fromUtf8(LOCALMODELS_TEST_BINARY_PATH), root + QStringLiteral("/venv/python.exe")));
        LocalModelsTranslationProvider provider;
        ProviderOptions options;
        options.setOption("root", root);
        provider.applyOptions(options);
        provider.translate(QStringLiteral("hello"), Language(QStringLiteral("ja")), Language(QStringLiteral("en")));
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
        QCOMPARE(provider.result.translation, QString::fromUtf8("日本語の訳文\n\n123"));
#endif
    }

    void optionalSetupResolvesAUserSelectedDataDirectory()
    {
#ifdef Q_OS_WIN
        QTemporaryDir local;
        QTemporaryDir models;
        const QByteArray previous = qgetenv("LOCALAPPDATA");
        const auto restore = qScopeGuard([previous]() { qputenv("LOCALAPPDATA", previous); });
        qputenv("LOCALAPPDATA", local.path().toUtf8());
        QVERIFY(QDir().mkpath(local.path() + QStringLiteral("/Crow Translate")));
        QFile descriptor(local.path() + QStringLiteral("/Crow Translate/components.json"));
        QVERIFY(descriptor.open(QIODevice::WriteOnly));
        descriptor.write(QJsonDocument(QJsonObject{{QStringLiteral("root"), models.path()}}).toJson());
        descriptor.close();
        QCOMPARE(OptionalComponents::root(), models.path());
        QVERIFY(QDir().mkpath(models.path() + QStringLiteral("/local-translation/venv")));
        QFile python(models.path() + QStringLiteral("/local-translation/venv/python.exe"));
        QVERIFY(python.open(QIODevice::WriteOnly));
        python.close();
        // An explicit legacy virtual environment keeps its established path.
        QVERIFY(OptionalComponents::python(models.path() + QStringLiteral("/local-translation")).endsWith(QStringLiteral("/venv/python.exe")));
#endif
    }

    void catalogHasExactlyFourDistinctModels()
    {
        const auto ids = LocalModelsTranslationProvider::modelIds();
        QCOMPARE(ids.size(), 4);
        for (const auto &id : ids) {
            QVERIFY(!LocalModelsTranslationProvider::modelName(id).isEmpty());
            QVERIFY(LocalModelsTranslationProvider::modelFile(id).endsWith(QStringLiteral(".gguf")));
        }
        QVERIFY(!LocalModelsTranslationProvider::isInstalled(QStringLiteral("missing"), ids.first()));
    }
    void translationAndWarmReuse()
    {
        QTemporaryDir directory;
        LocalModelsTranslationProvider provider;
        configure(provider, directory.path());
        QSignalSpy states(&provider, &ATranslationProvider::stateChanged);
        QSignalSpy loading(&provider, &ATranslationProvider::modelLoading);
        provider.translate(QStringLiteral("hello"), Language(QStringLiteral("ja")), Language(QStringLiteral("en")));
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
        QCOMPARE(provider.result.translation, QString::fromUtf8("日本語の訳文\n\n123"));
        QCOMPARE(loading.count(), 1);
        provider.finish();
        provider.reset();
        provider.translate(QStringLiteral("again"), Language(QStringLiteral("ja")), Language(QStringLiteral("en")));
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
        QFile starts(directory.filePath(QStringLiteral("starts.txt")));
        QVERIFY(starts.open(QIODevice::ReadOnly));
        QCOMPARE(starts.readAll().count("started"), 1);
    }
    void detectionUsesRealResultAndDoesNotStartTranslation()
    {
        QTemporaryDir directory;
        LocalModelsTranslationProvider provider;
        configure(provider, directory.path());
        QSignalSpy detected(&provider, &ATranslationProvider::languageDetected);
        QCOMPARE(provider.detectLanguage(QStringLiteral("hello")), Language::autoLanguage());
        QTRY_COMPARE(detected.count(), 1);
        QCOMPARE(detected.first().first().value<Language>(), Language(QStringLiteral("en")));
        QCOMPARE(detected.first().at(1).toBool(), false);
        QCOMPARE(provider.getState(), ATranslationProvider::State::Ready);
    }
    void failureAndRecovery()
    {
        QTemporaryDir directory;
        LocalModelsTranslationProvider provider;
        configure(provider, directory.path());
        QFile fail(directory.filePath(QStringLiteral("fail")));
        QVERIFY(fail.open(QIODevice::WriteOnly));
        fail.close();
        provider.translate(QStringLiteral("hello"), Language(QStringLiteral("ja")), Language(QStringLiteral("en")));
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Finished);
        QVERIFY(provider.getErrorString().contains(QStringLiteral("could not load")));
        QVERIFY(provider.result.translation.isEmpty());
        QVERIFY(fail.remove());
        provider.reset();
        provider.translate(QStringLiteral("hello"), Language(QStringLiteral("ja")), Language::autoLanguage());
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
        QCOMPARE(provider.sourceLanguage, Language(QStringLiteral("en")));
    }
    void abortAndModelSwitch()
    {
        QTemporaryDir directory;
        LocalModelsTranslationProvider provider;
        configure(provider, directory.path());
        QFile slow(directory.filePath(QStringLiteral("slow")));
        QVERIFY(slow.open(QIODevice::WriteOnly));
        slow.close();
        QSignalSpy loading(&provider, &ATranslationProvider::modelLoading);
        provider.translate(QStringLiteral("hello"), Language(QStringLiteral("ja")), Language(QStringLiteral("en")));
        QTRY_COMPARE(loading.count(), 1);
        provider.abort();
        QCOMPARE(provider.getState(), ATranslationProvider::State::Finished);
        QCOMPARE(provider.error, ATranslationProvider::TranslationError::Aborted);
        QVERIFY(slow.remove());
        provider.reset();
        ProviderOptions options;
        options.setOption("model", "seed-x-ppo-7b");
        provider.applyOptions(options);
        provider.translate(QStringLiteral("hello"), Language(QStringLiteral("ja")), Language(QStringLiteral("en")));
        QTRY_COMPARE(provider.getState(), ATranslationProvider::State::Processed);
    }
    void invalidConfigurationFailsWithoutFallback()
    {
        LocalModelsTranslationProvider provider;
        ProviderOptions options;
        options.setOption("model", "unknown-model");
        provider.applyOptions(options);
        provider.translate(QStringLiteral("hello"), Language(QStringLiteral("ja")), Language(QStringLiteral("en")));
        QCOMPARE(provider.getState(), ATranslationProvider::State::Finished);
        QVERIFY(provider.getErrorString().contains(QStringLiteral("Unknown")));
    }
    void timeoutRejectsPartialWork()
    {
        QTemporaryDir directory;
        LocalModelsTranslationProvider provider;
        configure(provider, directory.path());
        QFile timeout(directory.filePath(QStringLiteral("timeout")));
        QVERIFY(timeout.open(QIODevice::WriteOnly));
        timeout.close();
        ProviderOptions options;
        options.setOption("timeout", 1);
        provider.applyOptions(options);
        provider.translate(QStringLiteral("hello"), Language(QStringLiteral("ja")), Language(QStringLiteral("en")));
        QTRY_COMPARE_WITH_TIMEOUT(provider.getState(), ATranslationProvider::State::Finished, 5000);
        QVERIFY(provider.getErrorString().contains(QStringLiteral("timed out")));
        QVERIFY(provider.result.translation.isEmpty());
    }
    void abortAlsoStopsTheOwnedChildProcess()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        LocalModelsTranslationProvider provider;
        configure(provider, directory.path());
        for (const auto &name : {QStringLiteral("spawn-child"), QStringLiteral("slow")}) {
            QFile marker(directory.filePath(name));
            QVERIFY(marker.open(QIODevice::WriteOnly));
        }
        provider.translate(QStringLiteral("hello"), Language(QStringLiteral("ja")), Language(QStringLiteral("en")));
        QTRY_VERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("child.pid"))));
        QFile pid(directory.filePath(QStringLiteral("child.pid")));
        QVERIFY(pid.open(QIODevice::ReadOnly));
        const DWORD processId = pid.readAll().toUInt();
        HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, processId);
        QVERIFY(child != nullptr);
        provider.abort();
        const DWORD result = WaitForSingleObject(child, 3000);
        CloseHandle(child);
        QCOMPARE(result, static_cast<DWORD>(WAIT_OBJECT_0));
#else
        QSKIP("Windows process-job contract");
#endif
    }
};
int main(int argc, char **argv)
{
    isolateTestSettings();
    QCoreApplication application(argc, argv);
    LocalModelsProviderTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_localmodels_provider.moc"
