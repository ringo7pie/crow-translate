/*
 * SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mainwindow.h"
#include "popupwindow.h"
#include "singleapplication.h"
#include "sourcetextedit.h"
#include "testisolation.h"
#include "core/translationsession.h"
#include "gui/translationactivitywidget.h"
#include "settings/appsettings.h"
#include "translator/copytranslationprovider.h"

#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

class TranslationActivityWidgetTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_directory;

    static TranslationActivityWidget *banner(QWidget *window)
    {
        return window->findChild<TranslationActivityWidget *>(QStringLiteral("translationActivityWidget"));
    }

    static QPushButton *cancelButton(QWidget *widget)
    {
        return widget->findChild<QPushButton *>(QStringLiteral("cancelTranslationButton"));
    }

    static QLabel *message(QWidget *widget)
    {
        return widget->findChild<QLabel *>(QStringLiteral("translationActivityMessage"));
    }

    bool startTranslation(MainWindow &window)
    {
        window.sourceEdit()->setPlainText(QStringLiteral("Hello"));
        window.translationEdit()->setPlainText(QStringLiteral("Previous translation"));
        if (!QTest::qWaitFor([&window]() { return window.translateButton()->isEnabled(); }, 3000))
            return false;
        window.translateButton()->click();
        return true;
    }

private slots:
    void init()
    {
        qunsetenv("CROW_CODEX_TEST_MODE");
        qputenv("CROW_CODEX_TEST_RECORD", m_directory.filePath(QStringLiteral("record.json")).toUtf8());
        QFile::remove(m_directory.filePath(QStringLiteral("record.json")));
        AppSettings settings;
        settings.setShowPrivacyPopup(false);
        settings.setShowTrayIcon(false);
        settings.setStartMinimized(false);
        settings.setAutoTranslateEnabled(false);
        settings.setWindowMode(AppSettings::MainWindow);
        settings.setShowStatusBar(true);
        settings.setTTSProviderBackend(ATTSProvider::ProviderBackend::None);
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Codex);
        settings.setCodexExecutable(QString::fromUtf8(CODEX_TEST_BINARY_PATH));
        settings.setCodexModel(QStringLiteral("test-model"));
        settings.setCodexTimeout(30);
        settings.setCodexInstructions({});
        settings.setDetectModel({});
        settings.setForceSourceAutodetect(false);
        settings.setForceTranslationAutodetect(false);
        settings.setLanguages(AppSettings::Source, {Language(QStringLiteral("en"))});
        settings.setLanguages(AppSettings::Translation, {Language(QStringLiteral("ja"))});
        settings.setCheckedButton(AppSettings::Source, 0);
        settings.setCheckedButton(AppSettings::Translation, 0);
        settings.setLocale(QLocale(QLocale::English));
    }

    void modelClockAndLateView()
    {
        ModuleStatus model;
        CopyTranslationProvider provider;
        model.bindTranslator(&provider);
        TranslationActivityWidget first;
        first.setModel(&model);
        QVERIFY(first.isHidden());
        QCOMPARE(model.elapsedMilliseconds(ModuleStatus::Module::Translation), 0);

        provider.stateChanged(ATranslationProvider::State::Processing);
        QVERIFY(!first.isHidden());
        QCOMPARE(message(&first)->text(), QStringLiteral("Translating…"));
        auto *progress = first.findChild<QProgressBar *>(QStringLiteral("translationBusyIndicator"));
        QCOMPARE(progress->minimum(), 0);
        QCOMPARE(progress->maximum(), 0);
        QTRY_VERIFY(model.elapsedMilliseconds(ModuleStatus::Module::Translation) >= 1100);
        QTRY_VERIFY(first.findChild<QLabel *>(QStringLiteral("translationElapsedLabel"))->text() != QStringLiteral("0 s"));

        TranslationActivityWidget late;
        late.setModel(&model);
        QCOMPARE(late.findChild<QLabel *>(QStringLiteral("translationElapsedLabel"))->text(),
                 QStringLiteral("%1 s").arg(model.elapsedMilliseconds(ModuleStatus::Module::Translation) / 1000));
        QVERIFY(!late.isHidden());
        QSignalSpy cancelled(&late, &TranslationActivityWidget::cancelRequested);
        cancelButton(&late)->click();
        QCOMPARE(cancelled.size(), 1);

        provider.stateChanged(ATranslationProvider::State::Finished);
        QVERIFY(first.isHidden());
        QVERIFY(late.isHidden());
        QVERIFY(!first.findChild<QTimer *>(QStringLiteral("translationActivityTimer"))->isActive());
        QCOMPARE(model.elapsedMilliseconds(ModuleStatus::Module::Translation), 0);

        provider.detectionStarted();
        QCOMPARE(message(&first)->text(), QStringLiteral("Detecting language…"));
        provider.languageDetected(Language(QStringLiteral("en")), false);
        QVERIFY(first.isHidden());

        provider.stateChanged(ATranslationProvider::State::Processing);
        model.bindTranslator(nullptr);
        QVERIFY(first.isHidden());
    }

    void destroyingModelStopsTimer()
    {
        TranslationActivityWidget widget;
        auto model = std::make_unique<ModuleStatus>();
        CopyTranslationProvider provider;
        model->bindTranslator(&provider);
        widget.setModel(model.get());
        provider.stateChanged(ATranslationProvider::State::Processing);
        QVERIFY(!widget.isHidden());
        model.reset();
        QVERIFY(widget.isHidden());
        QVERIFY(!widget.findChild<QTimer *>(QStringLiteral("translationActivityTimer"))->isActive());
    }

    void visibleInBothWindowsAndCancel_data()
    {
        QTest::addColumn<bool>("popupCancel");
        QTest::addColumn<bool>("statusBar");
        QTest::newRow("main-status-shown") << false << true;
        QTest::newRow("main-status-hidden") << false << false;
        QTest::newRow("popup-status-shown") << true << true;
        QTest::newRow("popup-status-hidden") << true << false;
    }

    void visibleInBothWindowsAndCancel()
    {
        QFETCH(bool, popupCancel);
        QFETCH(bool, statusBar);
        AppSettings().setShowStatusBar(statusBar);
        qputenv("CROW_CODEX_TEST_MODE", "hang");
        MainWindow window;
        window.resize(1000, 650);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *mainBanner = banner(&window);
        QVERIFY(mainBanner);
        QVERIFY(mainBanner->isHidden());
        QVERIFY(startTranslation(window));
        QVERIFY(!mainBanner->isHidden());
        QTRY_VERIFY(!mainBanner->visibleRegion().isEmpty());
        QCOMPARE(window.translationEdit()->toPlainText(), QStringLiteral("Previous translation"));
        QVERIFY(!window.sourceEdit()->textCursor().hasSelection());
        QVERIFY(!window.translateButton()->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(m_directory.filePath(QStringLiteral("record.json"))), 10000);

        auto *popup = new PopupWindow(&window);
        popup->show();
        QVERIFY(QTest::qWaitForWindowExposed(popup));
        auto *popupBanner = banner(popup);
        QVERIFY(popupBanner);
        QVERIFY(!popupBanner->isHidden());
        QCOMPARE(message(mainBanner)->text(), message(popupBanner)->text());

        const QString screenshot = qEnvironmentVariable("CROW_ACTIVITY_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::qWait(50);
            QVERIFY(window.grab().save(screenshot + QStringLiteral("-main.png")));
            QVERIFY(popup->grab().save(screenshot + QStringLiteral("-popup.png")));
        }

        cancelButton(popupCancel ? popupBanner : mainBanner)->click();
        QVERIFY(mainBanner->isHidden());
        QVERIFY(popupBanner->isHidden());
        QCOMPARE(window.moduleStatus()->activity(ModuleStatus::Module::Translation), ModuleStatus::Activity::Idle);
        QVERIFY(window.translateButton()->isEnabled());
        QVERIFY(!mainBanner->findChild<QTimer *>(QStringLiteral("translationActivityTimer"))->isActive());
        QVERIFY(!popupBanner->findChild<QTimer *>(QStringLiteral("translationActivityTimer"))->isActive());

        qunsetenv("CROW_CODEX_TEST_MODE");
        window.translateButton()->click();
        QTRY_COMPARE_WITH_TIMEOUT(window.translationEdit()->toPlainText(), QStringLiteral("contract test output"), 10000);
        QVERIFY(mainBanner->isHidden());
        QVERIFY(popupBanner->isHidden());
        QVERIFY(!window.sourceEdit()->textCursor().hasSelection());
    }

    void terminalPathsHideBanner_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("failure") << QByteArray("error");
        QTest::newRow("timeout") << QByteArray("timeout");
        QTest::newRow("backend-switch") << QByteArray("switch");
    }

    void terminalPathsHideBanner()
    {
        QFETCH(QByteArray, mode);
        qputenv("CROW_CODEX_TEST_MODE", mode == "error" ? "error" : "hang");
        if (mode == "timeout")
            AppSettings().setCodexTimeout(1);
        MainWindow window;
        auto *activity = banner(&window);
        QVERIFY(activity);
        QVERIFY(startTranslation(window));
        QVERIFY(!activity->isHidden());
        if (mode == "switch") {
            auto *session = window.findChild<TranslationSession *>();
            QVERIFY(session);
            session->setTranslationBackend(ATranslationProvider::ProviderBackend::Copy);
            QVERIFY(activity->isHidden());
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(activity->isHidden(), 10000);
            QCOMPARE(window.moduleStatus()->activity(ModuleStatus::Module::Translation), ModuleStatus::Activity::Error);
            QVERIFY(window.translationEdit()->toPlainText().startsWith(QStringLiteral("Error:")));
        }
        QVERIFY(!activity->findChild<QTimer *>(QStringLiteral("translationActivityTimer"))->isActive());
    }

    void standaloneDetectionCanBeCancelled()
    {
        qputenv("CROW_CODEX_TEST_MODE", "hang");
        MainWindow window;
        auto *session = window.findChild<TranslationSession *>();
        QVERIFY(session);
        auto *activity = banner(&window);
        QVERIFY(activity);
        session->translator()->detectLanguage(QStringLiteral("Hello"));
        QVERIFY(!activity->isHidden());
        QCOMPARE(message(activity)->text(), QStringLiteral("Detecting language…"));
        cancelButton(activity)->click();
        QVERIFY(activity->isHidden());
        QCOMPARE(window.moduleStatus()->activity(ModuleStatus::Module::Translation), ModuleStatus::Activity::Idle);
        QCOMPARE(session->translator()->getState(), ATranslationProvider::State::Ready);
    }

    void cleanup()
    {
        qunsetenv("CROW_CODEX_TEST_MODE");
        qunsetenv("CROW_CODEX_TEST_RECORD");
    }
};

int main(int argc, char **argv)
{
    isolateTestSettings();
    QCoreApplication::setApplicationName(QStringLiteral("crow-translation-activity-tests"));
    SingleApplication app(argc, argv, true);
    // The offscreen Windows plugin may have no system font database. Load a
    // local font when taking optional review screenshots; tests don't require it.
#ifdef Q_OS_WIN
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/meiryo.ttc"));
    if (fontId >= 0)
        app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).value(0), 10));
#endif
    TranslationActivityWidgetTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_translationactivitywidget.moc"
