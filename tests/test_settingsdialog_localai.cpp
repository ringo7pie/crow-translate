/*
 * SPDX-FileCopyrightText: 2026 Mauritius Clemens <gitlab@janitor.chat>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Gotchas #6 and #7 from review-localai-backend.md. Both fixes (cb74d186)
// only exist on localai-backend-salvage, not this branch - the affected
// assertions are expected to start RED here and flip green once this test
// file lands on top of the salvage merge.

#include "mainwindow.h"
#include "mockhttpserver.h"
#include "singleapplication.h"
#include "testisolation.h"
#include "settings/appsettings.h"
#include "settings/settingsdialog.h"
#include "translator/atranslationprovider.h"
#include "translator/localmodelstranslationprovider.h"

#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTest>
#include <QTimer>

class SettingsDialogLocalAiTest : public QObject
{
    Q_OBJECT

private slots:
    void testMemoryAndSentenceSettingsAreTransactional()
    {
        AppSettings settings;
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
        settings.setLocalModelsVramReserve(256);
        settings.setSentenceLineBreaks(false);
        MainWindow window;
        for (bool accept : {false, true}) {
            SettingsDialog dialog(&window);
            auto *reserve = dialog.findChild<QSpinBox *>(QStringLiteral("localModelsVramReserveSpinBox"));
            auto *sentences = dialog.findChild<QCheckBox *>(QStringLiteral("sentenceLineBreaksCheckBox"));
            QVERIFY(reserve && sentences);
            QCOMPARE(reserve->value(), 256);
            QVERIFY(!sentences->isChecked());
            reserve->setValue(384);
            sentences->setChecked(true);
            QVERIFY(!dialog.findChild<QCheckBox *>(QStringLiteral("sourceSimplificationCheckBox"))->isEnabled());
            if (accept) dialog.accept(); else dialog.reject();
            QCOMPARE(settings.localModelsVramReserve(), accept ? 384 : 256);
            QCOMPARE(settings.sentenceLineBreaks(), accept);
        }
        {
            SettingsDialog dialog(&window);
            QCOMPARE(dialog.findChild<QSpinBox *>(QStringLiteral("localModelsVramReserveSpinBox"))->value(), 384);
            QVERIFY(dialog.findChild<QCheckBox *>(QStringLiteral("sentenceLineBreaksCheckBox"))->isChecked());
            QVERIFY(QMetaObject::invokeMethod(&dialog, "restoreDefaults", Qt::DirectConnection));
            QCOMPARE(dialog.findChild<QSpinBox *>(QStringLiteral("localModelsVramReserveSpinBox"))->value(), 256);
            QVERIFY(!dialog.findChild<QCheckBox *>(QStringLiteral("sentenceLineBreaksCheckBox"))->isChecked());
            dialog.reject();
            QCOMPARE(settings.localModelsVramReserve(), 384);
            QVERIFY(settings.sentenceLineBreaks());
        }
        settings.setLocalModelsVramReserve(256);
        settings.setSentenceLineBreaks(false);
    }

    void testInstructionDraftsAreIndependentAndOnlySavedOnAccept()
    {
        AppSettings settings;
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
        for (const QString &id : LocalModelsTranslationProvider::modelIds()) {
            settings.setLocalModelsPrompt(id, QString());
            settings.setLocalModelsInstructions(id, QString());
        }
        settings.setCodexPrompt(QString());
        settings.setCodexInstructions(QStringLiteral("Existing preference"));
        MainWindow window;
        {
            SettingsDialog dialog(&window);
            auto *models = dialog.findChild<QComboBox *>(QStringLiteral("localModelsModelComboBox"));
            auto *basic = dialog.findChild<QPlainTextEdit *>(QStringLiteral("localModelsPromptEdit"));
            auto *extra = dialog.findChild<QPlainTextEdit *>(QStringLiteral("localModelsInstructionsEdit"));
            auto *codex = dialog.findChild<QPlainTextEdit *>(QStringLiteral("codexPromptEdit"));
            QVERIFY(models && basic && extra && codex);
            QCOMPARE(codex->toPlainText(), AppSettings::defaultCodexPrompt());
            QCOMPARE(dialog.findChild<QPlainTextEdit *>(QStringLiteral("codexInstructionsEdit"))->toPlainText(), QStringLiteral("Existing preference"));
            for (const QString &id : LocalModelsTranslationProvider::modelIds()) {
                models->setCurrentIndex(models->findData(id));
                QCOMPARE(basic->toPlainText(), LocalModelsTranslationProvider::defaultPrompt(id));
                QVERIFY(extra->toPlainText().isEmpty());
                basic->setPlainText(id + QStringLiteral(" {text} {target_lang}"));
                extra->setPlainText(QString::fromUtf8("用語を保持 ") + id);
                QCOMPARE(settings.localModelsPrompt(id), LocalModelsTranslationProvider::defaultPrompt(id));
                QVERIFY(settings.localModelsInstructions(id).isEmpty());
            }
            for (const QString &id : LocalModelsTranslationProvider::modelIds()) {
                models->setCurrentIndex(models->findData(id));
                QCOMPARE(basic->toPlainText(), id + QStringLiteral(" {text} {target_lang}"));
                QCOMPARE(extra->toPlainText(), QString::fromUtf8("用語を保持 ") + id);
            }
            codex->setPlainText(QStringLiteral("Faithful translation."));
            dialog.accept();
            QCOMPARE(dialog.result(), int(QDialog::Accepted));
        }
        {
            SettingsDialog dialog(&window);
            auto *models = dialog.findChild<QComboBox *>(QStringLiteral("localModelsModelComboBox"));
            auto *basic = dialog.findChild<QPlainTextEdit *>(QStringLiteral("localModelsPromptEdit"));
            auto *extra = dialog.findChild<QPlainTextEdit *>(QStringLiteral("localModelsInstructionsEdit"));
            for (const QString &id : LocalModelsTranslationProvider::modelIds()) {
                models->setCurrentIndex(models->findData(id));
                QCOMPARE(basic->toPlainText(), id + QStringLiteral(" {text} {target_lang}"));
                QCOMPARE(extra->toPlainText(), QString::fromUtf8("用語を保持 ") + id);
            }
            QCOMPARE(dialog.findChild<QPlainTextEdit *>(QStringLiteral("codexPromptEdit"))->toPlainText(), QStringLiteral("Faithful translation."));
            dialog.findChild<QPushButton *>(QStringLiteral("localModelsResetPromptButton"))->click();
            QCOMPARE(basic->toPlainText(), LocalModelsTranslationProvider::defaultPrompt(models->currentData().toString()));
            QVERIFY(!extra->toPlainText().isEmpty());
            dialog.findChild<QPushButton *>(QStringLiteral("codexResetPromptButton"))->click();
            QCOMPARE(dialog.findChild<QPlainTextEdit *>(QStringLiteral("codexPromptEdit"))->toPlainText(), AppSettings::defaultCodexPrompt());
            dialog.reject();
        }
        QCOMPARE(settings.codexPrompt(), QStringLiteral("Faithful translation."));
        for (const QString &id : LocalModelsTranslationProvider::modelIds()) {
            QCOMPARE(settings.localModelsPrompt(id), id + QStringLiteral(" {text} {target_lang}"));
            QCOMPARE(settings.localModelsInstructions(id), QString::fromUtf8("用語を保持 ") + id);
        }
        // A global restore is also provisional, and clears both instruction fields.
        {
            SettingsDialog dialog(&window);
            QVERIFY(QMetaObject::invokeMethod(&dialog, "restoreDefaults", Qt::DirectConnection));
            auto *models = dialog.findChild<QComboBox *>(QStringLiteral("localModelsModelComboBox"));
            for (const QString &id : LocalModelsTranslationProvider::modelIds()) {
                models->setCurrentIndex(models->findData(id));
                QCOMPARE(dialog.findChild<QPlainTextEdit *>(QStringLiteral("localModelsPromptEdit"))->toPlainText(), LocalModelsTranslationProvider::defaultPrompt(id));
                QVERIFY(dialog.findChild<QPlainTextEdit *>(QStringLiteral("localModelsInstructionsEdit"))->toPlainText().isEmpty());
            }
            dialog.accept();
        }
        for (const QString &id : LocalModelsTranslationProvider::modelIds()) {
            QCOMPARE(settings.localModelsPrompt(id), LocalModelsTranslationProvider::defaultPrompt(id));
            QVERIFY(settings.localModelsInstructions(id).isEmpty());
        }
        QCOMPARE(settings.codexPrompt(), AppSettings::defaultCodexPrompt());
        QVERIFY(settings.codexInstructions().isEmpty());
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
    }

    void testLocalModelsSettingsAndMainWindowSelection()
    {
        AppSettings settings;
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
        settings.setLocalModelsModel(QStringLiteral("index-translate-2b"));
        MainWindow window;
        {
            SettingsDialog dialog(&window);
            auto *backend = dialog.findChild<QComboBox *>(QStringLiteral("translationProviderComboBox"));
            auto *models = dialog.findChild<QComboBox *>(QStringLiteral("localModelsModelComboBox"));
            auto *root = dialog.findChild<QLineEdit *>(QStringLiteral("localModelsRootEdit"));
            auto *timeout = dialog.findChild<QSpinBox *>(QStringLiteral("localModelsTimeoutSpinBox"));
            auto *group = dialog.findChild<QGroupBox *>(QStringLiteral("localModelsGroupBox"));
            QVERIFY(backend && models && root && timeout && group);
            QCOMPARE(models->count(), LocalModelsTranslationProvider::modelIds().size());
            QCOMPARE(models->currentData().toString(), QStringLiteral("index-translate-2b"));
            const int index = backend->findData(QVariant::fromValue(ATranslationProvider::ProviderBackend::LocalModels));
            QVERIFY(index >= 0);
            backend->setCurrentIndex(index);
            QVERIFY(!group->isHidden());
            QVERIFY(dialog.findChild<QComboBox *>(QStringLiteral("detectProviderComboBox"))->isHidden());
            QVERIFY(dialog.findChild<QComboBox *>(QStringLiteral("detectModelComboBox"))->isHidden());
            models->setCurrentIndex(models->findData(QStringLiteral("seed-x-ppo-7b")));
            root->setText(QStringLiteral("C:/Translation models/日本語"));
            timeout->setValue(90);
            dialog.accept();
        }
        QCOMPARE(settings.localModelsModel(), QStringLiteral("seed-x-ppo-7b"));
        QCOMPARE(settings.localModelsRoot(), QStringLiteral("C:/Translation models/日本語"));
        QCOMPARE(settings.localModelsTimeout(), 90);
        {
            SettingsDialog dialog(&window);
            QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("localModelsModelComboBox"))->currentData().toString(), settings.localModelsModel());
            QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("localModelsRootEdit"))->text(), settings.localModelsRoot());
            dialog.findChild<QComboBox *>(QStringLiteral("localModelsModelComboBox"))->setCurrentIndex(0);
            dialog.reject();
        }
        QCOMPARE(settings.localModelsModel(), QStringLiteral("seed-x-ppo-7b"));
        MainWindow reloaded;
        auto *combo = reloaded.getEngineComboBox();
        QCOMPARE(combo->count(), LocalModelsTranslationProvider::modelIds().size());
        QCOMPARE(combo->currentData().toString(), QStringLiteral("seed-x-ppo-7b"));
        combo->setCurrentIndex(combo->findData(QStringLiteral("hy-mt2-7b")));
        QCOMPARE(settings.localModelsModel(), QStringLiteral("hy-mt2-7b"));
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
    }

    void testIndraSettingsRoundTrip()
    {
        AppSettings settings;
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
        settings.setOcrEngine(AppSettings::OcrEngine::Tesseract);
        MainWindow window;
        {
            SettingsDialog dialog(&window);
            auto *engine = dialog.findChild<QComboBox *>(QStringLiteral("ocrEngineCombo"));
            auto *executable = dialog.findChild<QLineEdit *>(QStringLiteral("indraExecutableEdit"));
            auto *models = dialog.findChild<QLineEdit *>(QStringLiteral("indraModelDirectoryEdit"));
            auto *timeout = dialog.findChild<QSpinBox *>(QStringLiteral("indraTimeoutSpin"));
            auto *page = dialog.findChild<QWidget *>(QStringLiteral("indraOcrPage"));
            QVERIFY(engine && executable && models && timeout && page);
            const int index = engine->findData(static_cast<int>(AppSettings::OcrEngine::Indra));
            QVERIFY(index >= 0);
            engine->setCurrentIndex(index);
            QVERIFY(!page->isHidden());
            QVERIFY(dialog.findChild<QGroupBox *>(QStringLiteral("languagesGroupBox"))->isHidden());
            QVERIFY(dialog.findChild<QGroupBox *>(QStringLiteral("ocrParametersGroupBox"))->isHidden());
            executable->setText(QStringLiteral("C:/OCR tools/indra-ocr.exe"));
            models->setText(QStringLiteral("C:/OCR models/日本語"));
            timeout->setValue(45);
            dialog.accept();
        }
        QCOMPARE(settings.ocrEngine(), AppSettings::OcrEngine::Indra);
        QCOMPARE(settings.indraExecutable(), QStringLiteral("C:/OCR tools/indra-ocr.exe"));
        QCOMPARE(settings.indraModelDirectory(), QStringLiteral("C:/OCR models/日本語"));
        QCOMPARE(settings.indraTimeout(), 45);
        {
            SettingsDialog dialog(&window);
            QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("ocrEngineCombo"))->currentData().toInt(),
                     static_cast<int>(AppSettings::OcrEngine::Indra));
            QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("indraExecutableEdit"))->text(), settings.indraExecutable());
            QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("indraModelDirectoryEdit"))->text(), settings.indraModelDirectory());
            QCOMPARE(dialog.findChild<QSpinBox *>(QStringLiteral("indraTimeoutSpin"))->value(), 45);
        }
        settings.setOcrEngine(AppSettings::OcrEngine::Tesseract);
        settings.setIndraExecutable(QString());
        settings.setIndraModelDirectory(QString());
    }

    void testCodexControlsVisibleOnTranslationPage_data()
    {
        QTest::addColumn<bool>("alreadySelected");
        QTest::newRow("select-codex") << false;
        QTest::newRow("saved-codex") << true;
    }

    void testCodexControlsVisibleOnTranslationPage()
    {
        QFETCH(bool, alreadySelected);
        AppSettings settings;
        settings.setTranslationProviderBackend(alreadySelected ? ATranslationProvider::ProviderBackend::Codex
                                                              : ATranslationProvider::ProviderBackend::Copy);
        MainWindow window;
        SettingsDialog dialog(&window);
        auto *page = dialog.findChild<QWidget *>(QStringLiteral("translationPage"));
        auto *stack = dialog.findChild<QStackedWidget *>(QStringLiteral("pagesStackedWidget"));
        auto *list = dialog.findChild<QListWidget *>(QStringLiteral("pagesListWidget"));
        auto *backend = dialog.findChild<QComboBox *>(QStringLiteral("translationProviderComboBox"));
        auto *group = dialog.findChild<QGroupBox *>(QStringLiteral("codexGroupBox"));
        QVERIFY(page && stack && list && backend && group);
        list->setCurrentRow(stack->indexOf(page));
        dialog.resize(904, 746);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        // A narrow display starts compact mode with the content area hidden;
        // navigate using the same click that opens it for the user.
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          list->visualItemRect(list->item(stack->indexOf(page))).center());
        QTRY_VERIFY(page->isVisible());
        backend->setCurrentIndex(backend->findData(QVariant::fromValue(ATranslationProvider::ProviderBackend::Codex)));

        QVERIFY2(page->isAncestorOf(group), "Codex settings must belong to the Translation page");
        QTRY_VERIFY(group->isVisible());
        QTRY_VERIFY(group->visibleRegion().contains(group->rect()));
        for (const auto *name : {"codexExecutableEdit", "codexModelComboBox", "codexReasoningEffortComboBox", "codexTimeoutSpinBox", "codexPromptEdit", "codexInstructionsEdit"}) {
            auto *control = group->findChild<QWidget *>(QString::fromLatin1(name));
            QVERIFY(control);
            QVERIFY(control->isVisible());
            QTRY_VERIFY(control->visibleRegion().contains(control->rect()));
            QVERIFY(page->rect().contains(QRect(control->mapTo(page, QPoint()), control->size())));
        }
        const QString screenshot = qEnvironmentVariable("CROW_CODEX_SETTINGS_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::qWait(50);
            QVERIFY(dialog.grab().save(screenshot));
        }
        backend->setCurrentIndex(backend->findData(QVariant::fromValue(ATranslationProvider::ProviderBackend::Copy)));
        QVERIFY(group->isHidden());
        backend->setCurrentIndex(backend->findData(QVariant::fromValue(ATranslationProvider::ProviderBackend::Codex)));
        QTRY_VERIFY(group->isVisible());
        backend->setCurrentIndex(backend->findData(QVariant::fromValue(ATranslationProvider::ProviderBackend::LocalModels)));
        auto *localGroup = dialog.findChild<QGroupBox *>(QStringLiteral("localModelsGroupBox"));
        QTRY_VERIFY(localGroup->isVisible());
        for (const auto *name : {"localModelsPromptEdit", "localModelsInstructionsEdit", "localModelsResetPromptButton"}) {
            auto *control = localGroup->findChild<QWidget *>(QString::fromLatin1(name));
            QVERIFY(control && control->isVisible());
            QTRY_VERIFY(control->visibleRegion().contains(control->rect()));
        }
        const QString localScreenshot = qEnvironmentVariable("CROW_LOCALMODELS_SETTINGS_SCREENSHOT");
        if (!localScreenshot.isEmpty())
            QVERIFY(dialog.grab().save(localScreenshot));
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
    }

    void testCodexSettingsRoundTrip_data()
    {
        QTest::addColumn<QString>("modelName");
        QTest::addColumn<QString>("effort");
        QTest::newRow("listed-model") << QStringLiteral("gpt-6.1-sol") << QStringLiteral("high");
        QTest::newRow("custom-model") << QStringLiteral("my-codex-model") << QStringLiteral("xhigh");
    }

    void testCodexSettingsRoundTrip()
    {
        QFETCH(QString, modelName);
        QFETCH(QString, effort);
        AppSettings settings;
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
        settings.setCodexModel(QStringLiteral("saved-custom-model"));
        settings.setCodexReasoningEffort(QStringLiteral("medium"));
        MainWindow window;
        {
            SettingsDialog dialog(&window);
            auto *backend = dialog.findChild<QComboBox *>(QStringLiteral("translationProviderComboBox"));
            auto *group = dialog.findChild<QGroupBox *>(QStringLiteral("codexGroupBox"));
            auto *executable = dialog.findChild<QLineEdit *>(QStringLiteral("codexExecutableEdit"));
            auto *model = dialog.findChild<QComboBox *>(QStringLiteral("codexModelComboBox"));
            auto *reasoning = dialog.findChild<QComboBox *>(QStringLiteral("codexReasoningEffortComboBox"));
            auto *timeout = dialog.findChild<QSpinBox *>(QStringLiteral("codexTimeoutSpinBox"));
            auto *instructions = dialog.findChild<QPlainTextEdit *>(QStringLiteral("codexInstructionsEdit"));
            QVERIFY(backend && group && executable && model && reasoning && timeout && instructions);
            QVERIFY(model->isEditable());
            QVERIFY(!reasoning->isEditable());
            QCOMPARE(model->currentText(), QStringLiteral("saved-custom-model"));
            QCOMPARE(reasoning->currentData().toString(), QStringLiteral("medium"));
            for (const auto &level : AppSettings::codexReasoningEfforts())
                QVERIFY(reasoning->findData(level) >= 0);
            const int index = backend->findData(QVariant::fromValue(ATranslationProvider::ProviderBackend::Codex));
            QVERIFY(index >= 0);
            QVERIFY(group->isHidden());
            backend->setCurrentIndex(index);
            QVERIFY(!group->isHidden());
            executable->setText(QStringLiteral("C:/Codex with spaces/codex.exe"));
            if (modelName == QLatin1String("gpt-6.1-sol")) {
                QVERIFY(model->findText(modelName) >= 0);
                model->setCurrentIndex(model->findText(modelName));
            } else {
                model->setEditText(modelName);
            }
            reasoning->setCurrentIndex(reasoning->findData(effort));
            timeout->setValue(42);
            instructions->setPlainText(QStringLiteral("Keep product names unchanged."));
            dialog.accept();
        }
        QCOMPARE(settings.translationProviderBackend(), ATranslationProvider::ProviderBackend::Codex);
        {
            SettingsDialog reloaded(&window);
            QCOMPARE(reloaded.findChild<QComboBox *>(QStringLiteral("codexModelComboBox"))->currentText(), modelName);
            QCOMPARE(reloaded.findChild<QComboBox *>(QStringLiteral("codexReasoningEffortComboBox"))->currentData().toString(), effort);
            QCOMPARE(reloaded.findChild<QLineEdit *>(QStringLiteral("codexExecutableEdit"))->text(), QStringLiteral("C:/Codex with spaces/codex.exe"));
            QCOMPARE(reloaded.findChild<QSpinBox *>(QStringLiteral("codexTimeoutSpinBox"))->value(), 42);
            QCOMPARE(reloaded.findChild<QPlainTextEdit *>(QStringLiteral("codexInstructionsEdit"))->toPlainText(), QStringLiteral("Keep product names unchanged."));
        }
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
    }

    void testCodexRestoreDefaults()
    {
        AppSettings settings;
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
        settings.setCodexModel(QStringLiteral("saved-custom-model"));
        settings.setCodexReasoningEffort(QStringLiteral("max"));
        MainWindow window;
        SettingsDialog dialog(&window);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "restoreDefaults", Qt::DirectConnection));
        QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("codexModelComboBox"))->currentText(), AppSettings::defaultCodexModel());
        QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("codexReasoningEffortComboBox"))->currentData().toString(), AppSettings::defaultCodexReasoningEffort());
        // Restoring defaults is provisional until OK is pressed.
        dialog.reject();
        QCOMPARE(settings.codexModel(), QStringLiteral("saved-custom-model"));
        QCOMPARE(settings.codexReasoningEffort(), QStringLiteral("max"));
    }

    void initTestCase()
    {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
        if (qgetenv("DISPLAY").isEmpty() && qgetenv("WAYLAND_DISPLAY").isEmpty())
            QSKIP("No display server available - skipping GUI tests");
#endif
    }

    // Gotcha #6: "Refresh models" blocks the whole dialog on a nested
    // QEventLoop for up to the network timeout, and stays clickable while a
    // refresh is already in flight - a second click before the first
    // response arrives fires a second concurrent request. Reproducing the
    // re-entrant-click race itself proved unreliable to do synchronously in
    // this unit test (scheduling a callback to fire *during* the nested
    // QEventLoop hung rather than racing cleanly, likely an interaction
    // between the nested loop, QTest's synthetic event delivery, and this
    // sandboxed environment's event dispatcher) - that half of the
    // regression moves to crow-flake/e2e/, which can drive a real click on
    // a real running app instead of fighting a synthetic one. This test
    // keeps the functional smoke coverage: a refresh against a real
    // (mocked) endpoint actually populates the model list.
    void testRefreshModelsPopulatesModelList()
    {
        MockHttpServer server;
        server.queueJson(200, QByteArrayLiteral(R"({"data":[{"id":"model-a"},{"id":"model-b"}]})"));

        AppSettings settings;
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);

        MainWindow window;
        SettingsDialog dialog(&window);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));

        // LocalAI is one page among several in the settings sidebar
        // (General/Interface/.../LocalAI/OCR/...) - the dialog defaults to
        // showing a different page, and a page that isn't current in its
        // QStackedWidget never gets real geometry (its children end up 0px
        // tall and !isVisible(), so QTest::mouseClick lands on nothing).
        auto *localAiPage = dialog.findChild<QWidget *>(QStringLiteral("localAiPage"));
        QVERIFY(localAiPage != nullptr);
        auto *pagesStack = dialog.findChild<QStackedWidget *>(QStringLiteral("pagesStackedWidget"));
        QVERIFY(pagesStack != nullptr);
        // SettingsDialog::setCurrentPage() is private; the .ui file wires
        // pagesListWidget's currentRowChanged(int) to it via a designer
        // connection (list row index == stack page index), so driving the
        // public list widget is the real, supported way to switch pages.
        auto *pagesList = dialog.findChild<QListWidget *>(QStringLiteral("pagesListWidget"));
        QVERIFY(pagesList != nullptr);
        pagesList->setCurrentRow(pagesStack->indexOf(localAiPage));
        QTest::mouseClick(pagesList->viewport(), Qt::LeftButton, Qt::NoModifier,
                          pagesList->visualItemRect(pagesList->currentItem()).center());
        QTRY_VERIFY(localAiPage->isVisible());

        auto *tabs = dialog.findChild<QTabWidget *>(QStringLiteral("localAiTabWidget"));
        QVERIFY(tabs != nullptr);
        tabs->setCurrentIndex(0);
        QWidget *ollamaPage = tabs->widget(0);
        QVERIFY(ollamaPage != nullptr);

        auto *urlEdit = ollamaPage->findChild<QLineEdit *>(QStringLiteral("localAiUrlEdit"));
        QVERIFY(urlEdit != nullptr);
        urlEdit->setText(server.baseUrl());

        // The tab has a single text-model combo; "Refresh models" against a
        // plain chat-completions mock exercises the /v1/models probe.
        auto *modelCombo = ollamaPage->findChild<QComboBox *>(QStringLiteral("localAiTextModelCombo"));
        QVERIFY(modelCombo != nullptr);

        QPushButton *refreshButton = ollamaPage->findChild<QPushButton *>(QStringLiteral("localAiRefreshButton"));
        QVERIFY(refreshButton != nullptr);
        QVERIFY(QTest::qWaitFor([refreshButton]() {
            return refreshButton->isVisible() && refreshButton->height() > 0;
        },
                                2000));

        QTest::mouseClick(refreshButton, Qt::LeftButton);

        // QNetworkAccessManager::get() is async - the request only reaches
        // the mock server after the event loop spins for connect+write.
        QVERIFY(QTest::qWaitFor([&server]() {
            return server.requestCount() == 1;
        },
                                5000));
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(server.requestPath(0).contains(QStringLiteral("/v1/models")));
        QVERIFY(QTest::qWaitFor([modelCombo]() {
            return modelCombo->count() == 2;
        },
                                3000));
        QCOMPARE(modelCombo->itemText(0), QStringLiteral("model-a"));
        QCOMPARE(modelCombo->itemText(1), QStringLiteral("model-b"));
    }

    // Gotcha #7: switching Copy -> LocalAI -> Mozhi must restore the
    // original Mozhi engine list. The restore heuristic
    // (itemText(0) != m_engineItemsText.at(0)) is redundant with the
    // capability branch it's already inside; fixed by dropping it and
    // restoring unconditionally.
    void testEngineComboRestoredAfterLocalAiRoundTrip()
    {
        AppSettings settings;
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Mozhi);

        MainWindow window;
        auto *engineCombo = window.findChild<QComboBox *>(QStringLiteral("engineComboBox"));
        QVERIFY(engineCombo != nullptr);
        QVERIFY2(engineCombo->count() > 0, "Mozhi engine combo should be populated at startup");

        const QStringList originalItems = [&] {
            QStringList items;
            for (int i = 0; i < engineCombo->count(); ++i)
                items << engineCombo->itemText(i);
            return items;
        }();

        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::LocalAI);
        window.close();
        MainWindow window2;
        auto *engineCombo2 = window2.findChild<QComboBox *>(QStringLiteral("engineComboBox"));
        QVERIFY(engineCombo2 != nullptr);

        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Mozhi);
        window2.close();
        MainWindow window3;
        auto *engineCombo3 = window3.findChild<QComboBox *>(QStringLiteral("engineComboBox"));
        QVERIFY(engineCombo3 != nullptr);

        QStringList restoredItems;
        for (int i = 0; i < engineCombo3->count(); ++i)
            restoredItems << engineCombo3->itemText(i);

        QCOMPARE(restoredItems, originalItems);
    }

    // The OCR settings page's Tesseract-only widgets (languages,
    // parameters) must only show up when Tesseract is the selected engine;
    // the engine-agnostic screen-capture options stay visible either way.
    void testOcrEngineVisibilitySwitchesTesseractSpecificGroups()
    {
        AppSettings settings;
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);

        MainWindow window;
        SettingsDialog dialog(&window);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));

        auto *ocrPage = dialog.findChild<QWidget *>(QStringLiteral("ocrPage"));
        QVERIFY(ocrPage != nullptr);
        auto *pagesStack = dialog.findChild<QStackedWidget *>(QStringLiteral("pagesStackedWidget"));
        QVERIFY(pagesStack != nullptr);
        auto *pagesList = dialog.findChild<QListWidget *>(QStringLiteral("pagesListWidget"));
        QVERIFY(pagesList != nullptr);
        pagesList->setCurrentRow(pagesStack->indexOf(ocrPage));
        QTest::mouseClick(pagesList->viewport(), Qt::LeftButton, Qt::NoModifier,
                          pagesList->visualItemRect(pagesList->currentItem()).center());
        QTRY_VERIFY(ocrPage->isVisible());

        auto *engineCombo = dialog.findChild<QComboBox *>(QStringLiteral("ocrEngineCombo"));
        QVERIFY(engineCombo != nullptr);
        auto *languagesGroupBox = dialog.findChild<QGroupBox *>(QStringLiteral("languagesGroupBox"));
        QVERIFY(languagesGroupBox != nullptr);
        auto *ocrParametersGroupBox = dialog.findChild<QGroupBox *>(QStringLiteral("ocrParametersGroupBox"));
        QVERIFY(ocrParametersGroupBox != nullptr);
        auto *screenCaptureGroupBox = dialog.findChild<QGroupBox *>(QStringLiteral("screenCaptureGroupBox"));
        QVERIFY(screenCaptureGroupBox != nullptr);

        const int tesseractIndex = engineCombo->findData(static_cast<int>(AppSettings::OcrEngine::Tesseract));
        const int llmIndex = engineCombo->findData(static_cast<int>(AppSettings::OcrEngine::Llm));
        QVERIFY(tesseractIndex >= 0);
        QVERIFY(llmIndex >= 0);

        engineCombo->setCurrentIndex(tesseractIndex);
        QVERIFY(languagesGroupBox->isVisible());
        QVERIFY(ocrParametersGroupBox->isVisible());
        QVERIFY(screenCaptureGroupBox->isVisible());

        engineCombo->setCurrentIndex(llmIndex);
        QVERIFY(!languagesGroupBox->isVisible());
        QVERIFY(!ocrParametersGroupBox->isVisible());
        QVERIFY(screenCaptureGroupBox->isVisible());
    }
};

int main(int argc, char *argv[])
{
    isolateTestSettings();

    SingleApplication app(argc, argv, true);
    SettingsDialogLocalAiTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "test_settingsdialog_localai.moc"
