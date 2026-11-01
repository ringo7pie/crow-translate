/*
 * SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ocr/indraocr.h"
#include "core/translationsession.h"
#include "settings/appsettings.h"
#include "testisolation.h"
#include "modulestatus.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class IndraOcrTest : public QObject
{
    Q_OBJECT

    static QImage image()
    {
        QImage input(32, 16, QImage::Format_RGB32);
        input.fill(Qt::white);
        return input;
    }

private slots:
    void plainTextAndUtf8()
    {
        IndraOcr ocr;
        ocr.setExecutable(QString::fromUtf8(INDRA_TEST_BINARY_PATH));
        QVERIFY(ocr.isConfigured());
        QSignalSpy started(&ocr, &AOcrProvider::started);
        QSignalSpy recognized(&ocr, &AOcrProvider::recognized);
        QSignalSpy failed(&ocr, &AOcrProvider::failed);
        ocr.recognize(image(), 96);
        QTRY_COMPARE(recognized.count(), 1);
        QCOMPARE(started.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(recognized.first().first().toString(), QString::fromUtf8("Indra test\n日本語の文字認識"));
    }

    void failedExitRejectsPartialOutput()
    {
        IndraOcr ocr;
        ocr.setExecutable(QString::fromUtf8(INDRA_TEST_BINARY_PATH));
        ocr.setModelDirectory(QStringLiteral("fail"));
        QSignalSpy recognized(&ocr, &AOcrProvider::recognized);
        QSignalSpy failed(&ocr, &AOcrProvider::failed);
        ocr.recognize(image(), 96);
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("Missing test models")));
        QCOMPARE(recognized.count(), 0);
    }

    void emptyImageTextIsSuccessful()
    {
        IndraOcr ocr;
        ocr.setExecutable(QString::fromUtf8(INDRA_TEST_BINARY_PATH));
        ocr.setModelDirectory(QStringLiteral("empty"));
        QSignalSpy recognized(&ocr, &AOcrProvider::recognized);
        ocr.recognize(image(), 96);
        QTRY_COMPARE(recognized.count(), 1);
        QVERIFY(recognized.first().first().toString().isEmpty());
    }

    void timeoutAndRecovery()
    {
        IndraOcr ocr;
        ocr.setExecutable(QString::fromUtf8(INDRA_TEST_BINARY_PATH));
        ocr.setModelDirectory(QStringLiteral("slow"));
        ocr.setTimeout(1);
        QSignalSpy recognized(&ocr, &AOcrProvider::recognized);
        QSignalSpy failed(&ocr, &AOcrProvider::failed);
        ocr.recognize(image(), 96);
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 4000);
        QCOMPARE(recognized.count(), 0);
        ocr.setModelDirectory(QString());
        ocr.recognize(image(), 96);
        QTRY_COMPARE(recognized.count(), 1);
        QCOMPARE(failed.count(), 1);
    }

    void cancelDuringStart()
    {
        IndraOcr ocr;
        ocr.setExecutable(QString::fromUtf8(INDRA_TEST_BINARY_PATH));
        ocr.setModelDirectory(QStringLiteral("slow"));
        QSignalSpy recognized(&ocr, &AOcrProvider::recognized);
        QSignalSpy failed(&ocr, &AOcrProvider::failed);
        QSignalSpy canceled(&ocr, &AOcrProvider::canceled);
        connect(&ocr, &AOcrProvider::started, &ocr, &AOcrProvider::cancel);
        ocr.recognize(image(), 96);
        QTRY_COMPARE(canceled.count(), 1);
        ocr.cancel();
        QCOMPARE(canceled.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(recognized.count(), 0);
    }

    void invalidExecutableAndImage()
    {
        IndraOcr ocr;
        ocr.setExecutable(QStringLiteral("crow-missing-indra-executable"));
        QVERIFY(!ocr.isConfigured());
        QSignalSpy failed(&ocr, &AOcrProvider::failed);
        ocr.recognize(image(), 96);
        QCOMPARE(failed.count(), 1);
        ocr.setExecutable(QString::fromUtf8(INDRA_TEST_BINARY_PATH));
        ocr.recognize(QImage(), 96);
        QCOMPARE(failed.count(), 2);
    }

    void executablePathWithSpacesAndUnicode()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
#ifdef Q_OS_WIN
        const QString executable = directory.filePath(QString::fromUtf8("日本語 OCR tool.exe"));
#else
        const QString executable = directory.filePath(QString::fromUtf8("日本語 OCR tool"));
#endif
        QVERIFY(QFile::copy(QString::fromUtf8(INDRA_TEST_BINARY_PATH), executable));
        IndraOcr ocr;
        ocr.setExecutable(executable);
        QSignalSpy recognized(&ocr, &AOcrProvider::recognized);
        ocr.recognize(image(), 96);
        QTRY_COMPARE(recognized.count(), 1);
    }

    void replacementDiscardsPreviousRequest()
    {
        IndraOcr ocr;
        ocr.setExecutable(QString::fromUtf8(INDRA_TEST_BINARY_PATH));
        ocr.setModelDirectory(QStringLiteral("slow"));
        QSignalSpy started(&ocr, &AOcrProvider::started);
        QSignalSpy recognized(&ocr, &AOcrProvider::recognized);
        QSignalSpy failed(&ocr, &AOcrProvider::failed);
        ocr.recognize(image(), 96);
        QTRY_COMPARE(started.count(), 1);
        ocr.setModelDirectory(QString());
        ocr.recognize(image(), 96);
        QTRY_COMPARE(recognized.count(), 1);
        QCOMPARE(started.count(), 2);
        QCOMPARE(failed.count(), 0);
    }

    void statusFollowsIndra()
    {
        IndraOcr ocr;
        ocr.setExecutable(QString::fromUtf8(INDRA_TEST_BINARY_PATH));
        ocr.setModelDirectory(QStringLiteral("slow"));
        ModuleStatus status;
        status.bindOcr(nullptr, nullptr, &ocr);
        ocr.recognize(image(), 96);
        QTRY_COMPARE(status.activity(ModuleStatus::Module::Ocr), ModuleStatus::Activity::Busy);
        ocr.cancel();
        QCOMPARE(status.activity(ModuleStatus::Module::Ocr), ModuleStatus::Activity::Idle);
        ocr.setModelDirectory(QStringLiteral("fail"));
        ocr.recognize(image(), 96);
        QTRY_COMPARE(status.activity(ModuleStatus::Module::Ocr), ModuleStatus::Activity::Error);
        QVERIFY(status.detail(ModuleStatus::Module::Ocr).contains(QStringLiteral("Missing test models")));
    }

    void sessionUsesIndraAndChainsTranslation()
    {
        AppSettings settings;
        settings.setOcrEngine(AppSettings::OcrEngine::Indra);
        settings.setIndraExecutable(QString::fromUtf8(INDRA_TEST_BINARY_PATH));
        settings.setIndraModelDirectory(QString());
        TranslationSession session;
        session.setTranslationBackend(ATranslationProvider::ProviderBackend::Copy);
        session.setSelectedLanguages(Language(QStringLiteral("en")), Language(QStringLiteral("ja")));
        QCOMPARE(session.ocr()->engineName(), QStringLiteral("indra"));
        QVERIFY(session.prepareOcr());
        QSignalSpy translated(&session, &TranslationSession::translationStarted);
        session.armOcrTranslation(session.ocr());
        session.ocr()->recognize(image(), 96);
        QTRY_COMPARE(translated.count(), 1);
        QCOMPARE(translated.first().first().toString(), QString::fromUtf8("Indra test\n日本語の文字認識"));
        QVERIFY(!session.isOcrTranslationArmed());
    }
};

int main(int argc, char **argv)
{
    isolateTestSettings();
    QCoreApplication app(argc, argv);
    IndraOcrTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_indraocr.moc"
