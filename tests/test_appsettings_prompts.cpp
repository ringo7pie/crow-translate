/*
 * SPDX-FileCopyrightText: 2026 Mauritius Clemens <gitlab@janitor.chat>
 * SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// localAiPrompt() has to distinguish "the user wrote this prompt" from "an
// older build's settings dialog auto-saved its default here", because only the
// former may override the current default. Getting that wrong is what made
// ollama keep showing the superseded source-language prompt while a model added
// later showed the new one.

#include "testisolation.h"
#include "settings/appsettings.h"
#include "translator/localmodelstranslationprovider.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSettings>
#include <QTest>

namespace
{

// Byte-for-byte the default that master still ships, and that the old dialog
// persisted under every model whose page was opened.
QString supersededPrompt()
{
    return QStringLiteral(
        "You are a professional {source_lang} ({source_code}) to {target_lang} ({target_code}) translator. "
        "Your goal is to accurately convey the meaning and nuances of the original {source_lang} text "
        "while adhering to {target_lang} grammar, vocabulary, and cultural sensitivities.\n"
        "Produce only the {target_lang} translation, without any additional explanations or commentary. "
        "Please translate the following {source_lang} text into {target_lang}:\n\n\n{text}");
}

void storePrompt(const QString &model, const QString &prompt)
{
    QSettings settings;
    settings.setValue(QStringLiteral("LocalAI/Prompts/") + model, prompt);
    settings.sync();
}

} // namespace

class AppSettingsPromptsTest : public QObject
{
    Q_OBJECT

private slots:
#ifdef Q_OS_WIN
    void testInstallerDefaults_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<bool>("existing");
        for (const QString &model : LocalModelsTranslationProvider::modelIds()) {
            QTest::newRow(qPrintable(model + QStringLiteral("-new"))) << model << false;
            QTest::newRow(qPrintable(model + QStringLiteral("-existing"))) << model << true;
        }
        QTest::newRow("keep-existing") << QString() << true;
        QTest::newRow("invalid-model") << QStringLiteral("unknown-model") << true;
    }

    void testInstallerDefaults()
    {
        QFETCH(QString, model);
        QFETCH(bool, existing);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray previousLocal = qgetenv("LOCALAPPDATA");
        auto restore = qScopeGuard([&] { qputenv("LOCALAPPDATA", previousLocal); });
        qputenv("LOCALAPPDATA", directory.path().toUtf8());
        QSettings store;
        store.clear(); store.sync();
        if (existing) {
            AppSettings settings;
            settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
            settings.setLocalModelsModel(QStringLiteral("index-translate-2b"));
            settings.setOcrEngine(AppSettings::OcrEngine::Tesseract);
        }
        const QString root = directory.path() + QStringLiteral("/components");
        auto touch = [&](const QString &name) {
            const QString path = root + QLatin1Char('/') + name;
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path); return file.open(QIODevice::WriteOnly);
        };
        QVERIFY(touch(QStringLiteral("local-translation/venv/python.exe")));
        QVERIFY(touch(QStringLiteral("local-translation/venv/Scripts/indra-ocr.exe")));
        QVERIFY(touch(QStringLiteral("local-translation/runtime/llama-server.exe")));
        QVERIFY(touch(QStringLiteral("indra-models/meta.json")));
        const bool valid = LocalModelsTranslationProvider::modelIds().contains(model);
        if (valid) {
            QVERIFY(touch(QStringLiteral("local-translation/models/") + LocalModelsTranslationProvider::modelFile(model)));
            QVERIFY(touch(QStringLiteral("local-translation/tokenizers/seed-x-ppo-7b/tokenizer.json")));
        }
        const QString descriptor = directory.path() + QStringLiteral("/Crow Translate/components.json");
        QDir().mkpath(QFileInfo(descriptor).absolutePath());
        QFile file(descriptor);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QJsonObject request{{"id", "unique-test-request"}, {"translation_model", model}, {"indra_if_new", true}};
        file.write(QJsonDocument(QJsonObject{{"root", root}, {"installed", QJsonArray{model, "indra"}}, {"defaults_request", request}}).toJson());
        file.close();
        AppSettings settings;
        QCOMPARE(settings.ocrEngine(), existing ? AppSettings::OcrEngine::Tesseract : AppSettings::OcrEngine::Indra);
        QCOMPARE(settings.translationProviderBackend(), valid ? ATranslationProvider::ProviderBackend::LocalModels : ATranslationProvider::ProviderBackend::Copy);
        if (valid) {
            QCOMPARE(settings.localModelsModel(), model);
            QCOMPARE(settings.localModelsRoot(), root + QStringLiteral("/local-translation"));
        }
        // Reopening settings must not reapply a request after a user's change.
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
        settings.setOcrEngine(AppSettings::OcrEngine::Tesseract);
        AppSettings reopened;
        QCOMPARE(reopened.translationProviderBackend(), ATranslationProvider::ProviderBackend::Copy);
        QCOMPARE(reopened.ocrEngine(), AppSettings::OcrEngine::Tesseract);
        store.clear(); store.sync();
    }
#endif
    void init()
    {
        QSettings settings;
        settings.remove(QStringLiteral("LocalAI"));
        settings.sync();
    }

    void testUnsetPromptIsTheCurrentDefault()
    {
        const QString prompt = AppSettings().localAiPrompt(QStringLiteral("never-configured"));
        QCOMPARE(prompt, AppSettings::defaultLocalAiPrompt());
    }

    void testManagedModelInstructionsSurviveSettingsReload()
    {
        AppSettings settings;
        for (const QString &model : LocalModelsTranslationProvider::modelIds()) {
            settings.setLocalModelsPrompt(model, model + QStringLiteral(" {text}"));
            settings.setLocalModelsInstructions(model, QString::fromUtf8("敬語\n") + model);
        }
        AppSettings reloaded;
        for (const QString &model : LocalModelsTranslationProvider::modelIds()) {
            QCOMPARE(reloaded.localModelsPrompt(model), model + QStringLiteral(" {text}"));
            QCOMPARE(reloaded.localModelsInstructions(model), QString::fromUtf8("敬語\n") + model);
            settings.setLocalModelsPrompt(model, QStringLiteral(" \n"));
            QCOMPARE(reloaded.localModelsPrompt(model), LocalModelsTranslationProvider::defaultPrompt(model));
            settings.setLocalModelsInstructions(model, QString());
            QVERIFY(reloaded.localModelsInstructions(model).isEmpty());
        }
        settings.setCodexPrompt(QStringLiteral("Basic instructions"));
        settings.setCodexInstructions(QStringLiteral("Existing additional instructions"));
        QCOMPARE(reloaded.codexPrompt(), QStringLiteral("Basic instructions"));
        QCOMPARE(reloaded.codexInstructions(), QStringLiteral("Existing additional instructions"));
        settings.setCodexPrompt(QStringLiteral(" \n"));
        QCOMPARE(reloaded.codexPrompt(), AppSettings::defaultCodexPrompt());
        QCOMPARE(reloaded.codexInstructions(), QStringLiteral("Existing additional instructions"));
    }

    // The reported bug, at the level it actually lives: a stored copy of the
    // superseded default must not win over the current default.
    void testSupersededDefaultDoesNotOverrideCurrentDefault()
    {
        storePrompt(QStringLiteral("translategemma_12b_128k:latest"), supersededPrompt());

        const QString prompt = AppSettings().localAiPrompt(QStringLiteral("translategemma_12b_128k:latest"));
        QCOMPARE(prompt, AppSettings::defaultLocalAiPrompt());
        QVERIFY(!prompt.contains(QStringLiteral("{source_lang}")));
    }

    void testEmptyStoredPromptFallsBackToDefault()
    {
        storePrompt(QStringLiteral("half-typed"), QString());

        QCOMPARE(AppSettings().localAiPrompt(QStringLiteral("half-typed")), AppSettings::defaultLocalAiPrompt());
    }

    void testCustomPromptIsReturnedUntouched()
    {
        const QString mine = QStringLiteral("Translate into {target_lang}. Be terse. {text}");
        storePrompt(QStringLiteral("glm-4.5"), mine);

        QCOMPARE(AppSettings().localAiPrompt(QStringLiteral("glm-4.5")), mine);
    }

    // A prompt that merely resembles the superseded default - the user's own
    // edit of it - is still the user's, and must survive.
    void testEditedSupersededPromptIsRespected()
    {
        const QString edited = supersededPrompt() + QStringLiteral("\nKeep proper nouns untranslated.");
        storePrompt(QStringLiteral("my-model"), edited);

        QCOMPARE(AppSettings().localAiPrompt(QStringLiteral("my-model")), edited);
    }

    // A '/' in a model name would otherwise open a nested QSettings group,
    // filing the prompt under a key the reader never looks at.
    void testSlashInModelNameRoundTrips()
    {
        AppSettings settings;
        const QString mine = QStringLiteral("Transcribe, do not translate. {text}");
        settings.setLocalAiPrompt(QStringLiteral("openai/gpt-oss"), mine);

        QCOMPARE(settings.localAiPrompt(QStringLiteral("openai/gpt-oss")), mine);
    }
};

int main(int argc, char *argv[])
{
    isolateTestSettings();
    QCoreApplication app(argc, argv);
    AppSettingsPromptsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_appsettings_prompts.moc"
