/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "mainwindow.h"
#include "singleapplication.h"
#include "testisolation.h"
#include "settings/optionalcomponentsdialog.h"
#include "settings/settingsdialog.h"
#include "settings/shortcutsmodel/shortcutsmodel.h"
#include "settings/shortcutsmodel/shortcutsview.h"
#include "settings/shortcutsmodel/shortcutitem.h"
#include "translator/localmodelstranslationprovider.h"
#include <QCheckBox>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProcess>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QTreeWidget>
#include <QTranslator>

static bool writeFile(const QString &name, const QByteArray &bytes = {})
{
    QDir().mkpath(QFileInfo(name).absolutePath());
    QFile file(name);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

static void createBundle(const QString &path)
{
    LocalModelsTranslationProvider::modelIds(); // initialize resource
    QFile file(QStringLiteral(":/local-models/optional-components.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto catalog = QJsonDocument::fromJson(file.readAll()).object();
    QJsonArray components;
    const QByteArray license("A complete fixture license.\n");
    const QJsonObject record{{"file", "test-license.txt"}, {"size", license.size()},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(license, QCryptographicHash::Sha256).toHex())}};
    for (const auto &value : catalog.value("components").toArray()) {
        auto component = value.toObject();
        component.insert("licenses", QJsonArray{record});
        component.insert("notices", QJsonArray{});
        components.append(component);
    }
    catalog.insert("components", components);
    QVERIFY(writeFile(path + "/optional-components.json", QJsonDocument(catalog).toJson()));
    QVERIFY(writeFile(path + "/runtime-manifest.json", "{}"));
    QVERIFY(writeFile(path + "/notices/test-license.txt", license));
    // Real PowerShell is launched suspended and owned by a real Windows Job.
    // All writes are confined to the fixture directory, and no network is used.
    QVERIFY(writeFile(path + "/Install-OptionalComponents.ps1", R"(
param([switch]$Worker,[string]$DataRoot,[string]$Receipt,[string]$ProgressFile,[switch]$NoConfigure)
$ErrorActionPreference='Stop'
$r=Get-Content -LiteralPath $Receipt -Raw|ConvertFrom-Json
if(-not $Worker -or -not $NoConfigure -or -not $r.accepted){exit 3}
New-Item -ItemType Directory -Path $DataRoot -Force|Out-Null
if(Test-Path -LiteralPath (Join-Path $DataRoot 'fail')){
    [IO.File]::WriteAllText($ProgressFile,'{"done":true,"message":"fixture failure","percent":0}')
    exit 1
}
if(Test-Path -LiteralPath (Join-Path $DataRoot 'cancel')){
    $child=Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList @('-NoProfile','-File',('"'+(Join-Path $PSScriptRoot 'child.ps1')+'"'),('"'+$DataRoot+'"')) -PassThru -WindowStyle Hidden
    [IO.File]::WriteAllText((Join-Path $DataRoot 'child.pid'),[string]$child.Id)
    Start-Sleep -Seconds 120
}
[IO.File]::WriteAllText((Join-Path $DataRoot 'installed-fixture'),'ok')
[IO.File]::WriteAllText($ProgressFile,'{"done":true,"message":"fixture installed","percent":100,"completed":["index-translate-2b"],"failures":{}}')
)"));
    QVERIFY(writeFile(path + "/child.ps1", R"(
param([string]$DataRoot)
while($true){[IO.File]::WriteAllText((Join-Path $DataRoot 'heartbeat'),[DateTime]::UtcNow.Ticks.ToString());Start-Sleep -Milliseconds 100}
)"));
}

class LocalAiManagementTest : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        QSettings store;
        store.clear();
        store.setValue("MainWindow/ShowPrivacyPopup", false);
        AppSettings settings;
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
        settings.setTTSProviderBackend(ATTSProvider::ProviderBackend::None);
        settings.setGlobalShortcutsEnabled(false);
    }

    void consentGatesEveryInstall()
    {
        QTemporaryDir fixture;
        createBundle(fixture.path());
        OptionalComponentsDialog dialog({}, nullptr, fixture.path());
        auto *choices = dialog.findChild<QTreeWidget *>("componentChoices");
        auto *root = dialog.findChild<QLineEdit *>("componentRootEdit");
        auto *install = dialog.findChild<QPushButton *>("installComponentsButton");
        auto *consent = dialog.findChild<QCheckBox *>("componentConsent");
        auto *eligibility = dialog.findChild<QCheckBox *>("componentEligibility");
        root->setText(fixture.path() + "/components");
        QCOMPARE(choices->topLevelItemCount(), 6);
        QVERIFY(choices->topLevelItem(5)->text(0).contains(QStringLiteral("recommended")));
        choices->topLevelItem(0)->setCheckState(0, Qt::Checked);
        QVERIFY(!install->isEnabled());
        consent->setChecked(true);
        QVERIFY(!install->isEnabled());
        eligibility->setChecked(true);
#ifdef Q_OS_WIN
        QVERIFY(install->isEnabled());
#endif
        choices->topLevelItem(1)->setCheckState(0, Qt::Checked);
        QVERIFY(!install->isEnabled());
        QVERIFY(!consent->isChecked());
        choices->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
        choices->topLevelItem(1)->setCheckState(0, Qt::Unchecked);
        choices->topLevelItem(2)->setCheckState(0, Qt::Checked);
        consent->setChecked(true);
#ifdef Q_OS_WIN
        QVERIFY(install->isEnabled());
#endif
        root->setText("relative");
        QVERIFY(!install->isEnabled());
        root->setText(fixture.path() + "/components");
        QVERIFY(!consent->isChecked());
        QVERIFY(writeFile(fixture.path() + "/notices/test-license.txt", "tampered"));
        choices->topLevelItem(3)->setCheckState(0, Qt::Checked);
        consent->setChecked(true);
        QVERIFY(!install->isEnabled());
    }

#ifdef Q_OS_WIN
    void workerFailureRetryAndOwnedCancellation()
    {
        QTemporaryDir fixture;
        createBundle(fixture.path());
        const QString root = fixture.path() + QString::fromUtf8("/保存先 with spaces");
        QVERIFY(writeFile(root + "/fail"));
        OptionalComponentsDialog dialog({}, nullptr, fixture.path());
        dialog.findChild<QLineEdit *>("componentRootEdit")->setText(root);
        auto *choices = dialog.findChild<QTreeWidget *>("componentChoices");
        choices->topLevelItem(2)->setCheckState(0, Qt::Checked);
        auto *install = dialog.findChild<QPushButton *>("installComponentsButton");
        auto *cancel = dialog.findChild<QPushButton *>("cancelComponentsButton");
        auto *consent = dialog.findChild<QCheckBox *>("componentConsent");
        auto *status = dialog.findChild<QLabel *>("componentStatus");
        consent->setChecked(true);
        QVERIFY(install->isEnabled());
        install->click();
        QVERIFY(cancel->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 15000);
        QVERIFY(status->text().contains("fixture failure"));
        QVERIFY(QFile::remove(root + "/fail"));
        consent->setChecked(true);
        install->click();
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 15000);
        QVERIFY(QFileInfo::exists(root + "/installed-fixture"));
        QVERIFY(status->text().contains("completed"));
        QVERIFY(writeFile(root + "/cancel"));
        consent->setChecked(true);
        install->click();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root + "/heartbeat"), 15000);
        cancel->click();
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QFile heartbeat(root + "/heartbeat");
        QVERIFY(heartbeat.open(QIODevice::ReadOnly));
        const QByteArray stopped = heartbeat.readAll();
        heartbeat.close();
        QTest::qWait(600);
        QVERIFY(heartbeat.open(QIODevice::ReadOnly));
        QCOMPARE(heartbeat.readAll(), stopped);
        QVERIFY(QFileInfo::exists(root + "/installed-fixture"));
    }
#endif

    void shortcutMigrationRepairsOldBindings_data()
    {
        QTest::addColumn<QString>("recognize");
        QTest::addColumn<QString>("invert");
        QTest::addColumn<QString>("delayed");
        QTest::addColumn<QString>("playPause");
        QTest::addColumn<QString>("occupied");
        QTest::addColumn<QString>("expectedRecognize");
        QTest::addColumn<QString>("expectedInvert");
        QTest::addColumn<QString>("expectedPlayPause");
        const auto row = [](const char *name, const QStringList &values) {
            auto &data = QTest::newRow(name);
            for (const auto &value : values)
                data << value;
        };
        row("v10-ocr-default", {"Ctrl+Shift+Z", "", "", "Ctrl+Alt+G", "", "Ctrl+Alt+I", "", "Ctrl+Alt+D"});
        row("inversion-on-z", {"Ctrl+Alt+I", "Ctrl+Shift+Z", "", "Ctrl+Alt+G", "", "Ctrl+Alt+I", "", "Ctrl+Alt+D"});
        row("already-correct", {"Ctrl+Alt+I", "", "Ctrl+Shift+Z", "", "", "Ctrl+Alt+I", "", ""});
        row("custom-keys", {"Alt+F8", "Alt+F9", "Alt+F10", "Alt+F11", "", "Alt+F8", "Alt+F9", "Alt+F11"});
        row("disabled-keys", {"", "", "", "", "", "", "", ""});
        row("occupied-play-pause", {"Ctrl+Shift+Z", "", "", "Ctrl+Alt+G", "Ctrl+Alt+D", "Ctrl+Alt+I", "", ""});
        row("occupied-ocr-fallback", {"Ctrl+Shift+Z", "", "", "Ctrl+Alt+D", "Ctrl+Alt+I", "", "", "Ctrl+Alt+D"});
        row("other-action-on-z", {"Ctrl+Alt+I", "", "", "Ctrl+Alt+D", "Ctrl+Shift+Z", "Ctrl+Alt+I", "", "Ctrl+Alt+D"});
        row("other-action-duplicates-stop", {"Ctrl+Alt+I", "", "", "Ctrl+Alt+D", "Ctrl+Alt+G", "Ctrl+Alt+I", "", "Ctrl+Alt+D"});
    }

    void shortcutMigrationRepairsOldBindings()
    {
        QFETCH(QString, recognize);
        QFETCH(QString, invert);
        QFETCH(QString, delayed);
        QFETCH(QString, playPause);
        QFETCH(QString, occupied);
        QFETCH(QString, expectedRecognize);
        QFETCH(QString, expectedInvert);
        QFETCH(QString, expectedPlayPause);
        QSettings store;
        store.setValue("Shortcuts/OcrCaptureV2", true);
        store.remove("Shortcuts/CaptureTranslationV3");
        store.setValue("Shortcuts/RecognizeScreenAreaShortcut", QKeySequence(recognize));
        store.setValue("Shortcuts/ToggleOcrNegate", QKeySequence(invert));
        store.setValue("Shortcuts/DelayedTranslateScreenAreaShortcut", QKeySequence(delayed));
        store.setValue("Shortcuts/StopSelection", QKeySequence("Ctrl+Alt+G"));
        store.setValue("Shortcuts/PlayPauseSpeakingSelection", QKeySequence(playPause));
        store.setValue("Shortcuts/OpenSettings", QKeySequence(occupied));
        AppSettings settings;
        QCOMPARE(settings.recognizeScreenAreaShortcut(), QKeySequence(expectedRecognize));
        QCOMPARE(settings.toggleOcrNegateShortcut(), QKeySequence(expectedInvert));
        QCOMPARE(settings.delayedTranslateScreenAreaShortcut(), QKeySequence("Ctrl+Shift+Z"));
        QCOMPARE(settings.stopSpeakingShortcut(), QKeySequence("Ctrl+Alt+G"));
        QCOMPARE(settings.playPauseSpeakingShortcut(), QKeySequence(expectedPlayPause));
        ShortcutsModel model;
        model.loadShortcuts(settings);
        QVERIFY2(model.conflictDescription().isEmpty(), qPrintable(model.conflictDescription()));
        // A later user edit must survive every subsequent AppSettings read.
        settings.setDelayedTranslateScreenAreaShortcut(QKeySequence("Alt+F10"));
        QCOMPARE(AppSettings().delayedTranslateScreenAreaShortcut(), QKeySequence("Alt+F10"));
    }

    void shortcutDefaultsAndExistingCustomKeys()
    {
        QCOMPARE(AppSettings::defaultDelayedTranslateScreenAreaShortcut(), QKeySequence("Ctrl+Shift+Z"));
        QCOMPARE(AppSettings::defaultRecognizeScreenAreaShortcut(), QKeySequence("Ctrl+Alt+I"));
        QSettings store;
        store.setValue("Shortcuts/Swap", QKeySequence("Ctrl+Shift+M"));
        store.remove("Shortcuts/OcrCaptureV2");
        AppSettings migrated;
        QCOMPARE(migrated.swapShortcut(), QKeySequence("Ctrl+Shift+M"));
        QVERIFY(migrated.extraShortcut("ManageLocalAi").isEmpty());
    }

    void shortcutsValidateDuplicatesAndReset()
    {
        ShortcutsModel model;
        model.loadShortcuts(AppSettings());
        QVERIFY(model.conflictDescription().isEmpty());
        const auto window = model.index(1, 0);
        auto *translate = static_cast<ShortcutItem *>(model.index(0, 0, window).internalPointer());
        auto *swap = static_cast<ShortcutItem *>(model.index(1, 0, window).internalPointer());
        swap->setShortcut(translate->shortcut());
        QVERIFY(!model.conflictDescription().isEmpty());
        swap->setShortcut({});
        QVERIFY(model.conflictDescription().isEmpty());
        model.resetAllShortcuts();
        QCOMPARE(swap->shortcut(), AppSettings::defaultSwapShortcut());
    }

    void globalRegistrationFailureRestoresPreviousBinding()
    {
        const QKeySequence occupied("Ctrl+Alt+Shift+F11");
        QProcess holder;
        holder.start(QStringLiteral(HOTKEY_HOLDER_PATH), {occupied.toString(QKeySequence::PortableText)});
        QVERIFY(holder.waitForStarted());
        auto cleanup = qScopeGuard([&]() { holder.kill(); holder.waitForFinished(); });
        QVERIFY(holder.waitForReadyRead());
        if (!holder.readAllStandardOutput().contains("READY"))
            QSKIP("The fixture key is already owned by another process");
        MainWindow window;
        auto bindings = AppSettings().globalShortcutSequences();
        for (auto &key : bindings)
            key = {};
        bindings[7] = QKeySequence("Ctrl+Alt+Shift+F12");
        QString error;
        QVERIFY2(window.applyGlobalShortcuts(bindings, true, &error), qPrintable(error));
        auto changed = bindings;
        changed[7] = occupied;
        QVERIFY(!window.applyGlobalShortcuts(changed, true, &error));
        QVERIFY(error.contains(occupied.toString(QKeySequence::NativeText)));
        bool restored = false;
        for (QHotkey *hotkey : window.findChildren<QHotkey *>())
            restored |= hotkey->shortcut() == bindings[7] && hotkey->isRegistered();
        QVERIFY(restored);
        QVERIFY(window.applyGlobalShortcuts(bindings, false));
    }

    void settingsKeepModelSelectionAndShortcutDraftsUntilOk()
    {
        AppSettings settings;
        const QString previous = settings.localModelsRoot();
        MainWindow window;
        QTemporaryDir destination;
        const QString modelRoot = destination.path() + "/local-translation";
        QVERIFY(writeFile(modelRoot + "/venv/python.exe"));
        QVERIFY(writeFile(modelRoot + "/runtime/llama-server.exe"));
        QVERIFY(writeFile(modelRoot + "/models/" + LocalModelsTranslationProvider::modelFile("cat-translate-3.3b")));
        QVERIFY(writeFile(modelRoot + "/tokenizers/cat-translate-3.3b/tokenizer.json"));
        for (bool save : {false, true}) {
            SettingsDialog dialog(&window);
            QTimer::singleShot(0, &dialog, [&]() {
                auto *manager = dialog.findChild<OptionalComponentsDialog *>();
                QVERIFY(manager);
                manager->findChild<QLineEdit *>("componentRootEdit")->setText(destination.path());
                auto *choices = manager->findChild<QTreeWidget *>("componentChoices");
                choices->setCurrentItem(choices->topLevelItem(5));
                auto *use = manager->findChild<QPushButton *>("useComponentButton");
                QVERIFY(use->isEnabled());
                use->click();
            });
            dialog.findChild<QPushButton *>("manageLocalAiButton")->click();
            QCOMPARE(settings.localModelsRoot(), previous);
            QCOMPARE(dialog.findChild<QLineEdit *>("localModelsRootEdit")->text(), modelRoot);
            auto *view = dialog.findChild<ShortcutsView *>("shortcutsTreeView");
            view->setCurrentIndex(view->model()->index(0, 0, view->model()->index(1, 0)));
            dialog.findChild<QKeySequenceEdit *>("shortcutSequenceEdit")->setKeySequence(QKeySequence("Alt+F7"));
            dialog.findChild<QCheckBox *>("hideWindowsForCaptureCheckBox")->setChecked(false);
            if (save) dialog.accept(); else dialog.reject();
            QCOMPARE(settings.localModelsRoot(), save ? modelRoot : previous);
            QCOMPARE(settings.translateShortcut(), save ? QKeySequence("Alt+F7") : AppSettings::defaultTranslateShortcut());
            QCOMPARE(settings.hideWindowsForCapture(), !save);
        }
    }

    void preparedWindowsBundleIntegration()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows optional installer");
#else
        const QString bundle = qEnvironmentVariable("CROW_OPTIONAL_TEST_BUNDLE");
        const QString root = qEnvironmentVariable("CROW_OPTIONAL_TEST_ROOT");
        if (bundle.isEmpty() || root.isEmpty())
            QSKIP("Set CROW_OPTIONAL_TEST_BUNDLE and a private CROW_OPTIONAL_TEST_ROOT for the prepared-payload integration check");
        QVERIFY(QFileInfo::exists(root + "/local-translation/venv/crow-python.json"));
        const QString descriptor = qEnvironmentVariable("LOCALAPPDATA") + "/Crow Translate/components.json";
        QVERIFY(!QFileInfo::exists(descriptor));
        QTranslator japanese;
        if (japanese.load(bundle + "/../data/locale/ja/LC_MESSAGES/crow-translate_qt.qm"))
            QCoreApplication::installTranslator(&japanese);
        auto removeTranslator = qScopeGuard([&]() { QCoreApplication::removeTranslator(&japanese); });
        OptionalComponentsDialog dialog(root + "/local-translation", nullptr, bundle);
        auto *choices = dialog.findChild<QTreeWidget *>("componentChoices");
        choices->setCurrentItem(choices->topLevelItem(5));
        choices->topLevelItem(5)->setCheckState(0, Qt::Checked);
        auto *install = dialog.findChild<QPushButton *>("installComponentsButton");
        auto *cancel = dialog.findChild<QPushButton *>("cancelComponentsButton");
        QVERIFY(!install->isEnabled());
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const QString screenshot = qEnvironmentVariable("CROW_OPTIONAL_TEST_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(dialog.grab().save(screenshot));
        dialog.findChild<QCheckBox *>("componentConsent")->setChecked(true);
        QVERIFY(install->isEnabled());
        install->click();
        QVERIFY(cancel->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 180000);
        QVERIFY2(dialog.findChild<QLabel *>("componentStatus")->text().contains(QString::fromUtf8("完了")),
                 qPrintable(dialog.findChild<QLabel *>("componentStatus")->text()));
        QVERIFY(LocalModelsTranslationProvider::isInstalled(root + "/local-translation", "cat-translate-3.3b"));
        QVERIFY(!QFileInfo::exists(descriptor));
#endif
    }
};

int main(int argc, char **argv)
{
    isolateTestSettings();
    SingleApplication application(argc, argv, true);
    // Keep the installer descriptor isolated as well as the application settings.
    QTemporaryDir local;
    qputenv("LOCALAPPDATA", local.path().toUtf8());
    LocalAiManagementTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_local_ai_management.moc"
