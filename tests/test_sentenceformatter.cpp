// SPDX-FileCopyrightText: 2026 ringo7pie
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sentenceformatter.h"
#include "sourcetextedit.h"
#include "settings/appsettings.h"
#include "core/translationsession.h"
#include "testisolation.h"

#include <QApplication>
#include <QClipboard>
#include <QInputMethodEvent>
#include <QSignalSpy>
#include <QTest>

class SentenceFormatterTest : public QObject
{
    Q_OBJECT
private slots:
    void sentences_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("English") << QStringLiteral("First sentence. Second sentence.") << QStringLiteral("First sentence.\nSecond sentence.");
        QTest::newRow("Japanese") << QString::fromUtf8("一文目です。二文目です。") << QString::fromUtf8("一文目です。\n二文目です。");
        QTest::newRow("decimal") << QStringLiteral("The price is 3.14. Version 1.2.3 is ready.") << QStringLiteral("The price is 3.14.\nVersion 1.2.3 is ready.");
        QTest::newRow("addresses") << QStringLiteral("See https://example.com/a.b. Email a.b@example.com. Next.") << QStringLiteral("See https://example.com/a.b.\nEmail a.b@example.com.\nNext.");
        QTest::newRow("abbreviations") << QStringLiteral("Dr. Smith lives in the U.S. and uses e.g. these examples. Next.") << QStringLiteral("Dr. Smith lives in the U.S. and uses e.g. these examples.\nNext.");
        QTest::newRow("quotes") << QString::fromUtf8("「一文です。」次の文です。 He said \"Hello.\" Then left.") << QString::fromUtf8("「一文です。」\n次の文です。\nHe said \"Hello.\"\nThen left.");
        QTest::newRow("paragraphs") << QStringLiteral("First.\n\nSecond.\n  Third.") << QStringLiteral("First.\n\nSecond.\n  Third.");
        QTest::newRow("ellipsis") << QStringLiteral("Wait... perhaps. Next.") << QStringLiteral("Wait... perhaps.\nNext.");
    }
    void sentences()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(withSentenceLineBreaks(input), expected);
        QCOMPARE(withSentenceLineBreaks(expected), expected);
    }
    void editorPasteUndoAndSelection()
    {
        SourceTextEdit edit;
        edit.setSentenceLineBreaks(true);
        edit.setSimplifySource(true);
        edit.setListenForEdits(true);
        QSignalSpy edited(&edit, &SourceTextEdit::textEdited);
        QApplication::clipboard()->setText(QStringLiteral("First. Second."));
        edit.paste();
        QCOMPARE(edit.toPlainText(), QStringLiteral("First.\nSecond."));
        QCOMPARE(edit.toSourceText(), edit.toPlainText());
        QTRY_COMPARE(edited.count(), 1);
        edit.undo();
        QVERIFY(edit.toPlainText().isEmpty());
        edit.redo();
        QCOMPARE(edit.toPlainText(), QStringLiteral("First.\nSecond."));
        edit.setSentenceLineBreaks(false);
        edit.setPlainText(QStringLiteral("First. Second."));
        QTextCursor cursor = edit.textCursor();
        cursor.setPosition(7); cursor.setPosition(13, QTextCursor::KeepAnchor);
        edit.setTextCursor(cursor);
        edit.setSentenceLineBreaks(true);
        QCOMPARE(edit.textCursor().selectedText(), QStringLiteral("Second"));
        QCOMPARE(edit.textCursor().anchor(), 7);
    }
    void imeAndProgrammaticInput()
    {
        SourceTextEdit edit;
        edit.setSentenceLineBreaks(true);
        edit.setListenForEdits(true);
        QSignalSpy edited(&edit, &SourceTextEdit::textEdited);
        edit.setPlainText(QString::fromUtf8("一文。二文。"));
        QInputMethodEvent composing(QString::fromUtf8("変換中"), {});
        QApplication::sendEvent(&edit, &composing);
        QTest::qWait(650);
        QCOMPARE(edit.toPlainText(), QString::fromUtf8("一文。二文。"));
        QCOMPARE(edited.count(), 0);
        QInputMethodEvent commit;
        QApplication::sendEvent(&edit, &commit);
        QTRY_COMPARE(edited.count(), 1);
        QCOMPARE(edit.toPlainText(), QString::fromUtf8("一文。\n二文。"));
        edited.clear();
        edit.replaceText(QStringLiteral("OCR one. OCR two."));
        QCOMPARE(edit.toPlainText(), QStringLiteral("OCR one.\nOCR two."));
        QTest::qWait(650);
        QCOMPARE(edited.count(), 0);
    }
    void sessionSendsExactlyOneFormattedRequest()
    {
        AppSettings settings;
        settings.setSentenceLineBreaks(true);
        TranslationSession session;
        session.setTranslationBackend(ATranslationProvider::ProviderBackend::Copy);
        QSignalSpy started(&session, &TranslationSession::translationStarted);
        const QString raw = QStringLiteral("First. Second.");
        session.requestTranslation(raw, Language(QStringLiteral("en")), Language(QStringLiteral("en")));
        QCOMPARE(started.count(), 1);
        QCOMPARE(started.first().first().toString(), QStringLiteral("First.\nSecond."));
        QTRY_COMPARE(session.translator()->getState(), ATranslationProvider::State::Processed);
        QCOMPARE(session.translator()->result.translation, QStringLiteral("First.\nSecond."));
        settings.setSentenceLineBreaks(false);
        session.resetTranslator();
        started.clear();
        session.requestTranslation(raw, Language(QStringLiteral("en")), Language(QStringLiteral("en")));
        QCOMPARE(started.first().first().toString(), raw);
    }
};
int main(int argc, char **argv)
{
    isolateTestSettings();
    QApplication app(argc, argv);
    SentenceFormatterTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_sentenceformatter.moc"
