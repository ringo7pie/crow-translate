/*
 * SPDX-FileCopyrightText: 2018 Hennadii Chernyshchyk <genaloner@gmail.com>
 * SPDX-FileCopyrightText: 2022 Volk Milit <javirrdar@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sourcetextedit.h"

#include "contextmenu.h"
#include "core/sentenceformatter.h"

#include <QTimer>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QWheelEvent>

using namespace std::chrono_literals;

SourceTextEdit::SourceTextEdit(QWidget *parent)
    : QPlainTextEdit(parent)
    , m_textEditedTimer(new QTimer(this))
{
    m_textEditedTimer->setSingleShot(true);
    m_textEditedTimer->callOnTimeout(this, [this]() {
        if (m_composing) {
            startTimerDelay();
            return;
        }
        if (!m_skipFormat)
            formatSentences();
        m_skipFormat = false;
        emit textEdited();
    });
    connect(this, &SourceTextEdit::textChanged, this, &SourceTextEdit::checkSourceEmptyChanged);
}

void SourceTextEdit::setListenForEdits(bool listen)
{
    m_listenForEdits = listen;

    if (m_listenForEdits) {
        connect(this, &SourceTextEdit::textChanged, this, &SourceTextEdit::startTimerDelay);
    } else {
        m_textEditedTimer->stop();
        disconnect(this, &SourceTextEdit::textChanged, this, &SourceTextEdit::startTimerDelay);
    }
}

void SourceTextEdit::setSimplifySource(bool enabled)
{
    m_simplifySource = enabled;
}

QString SourceTextEdit::toSourceText()
{
    if (m_sentenceLineBreaks) {
        if (!m_composing && !m_skipFormat)
            formatSentences();
        return withSentenceLineBreaks(toPlainText()).trimmed();
    }
    return m_simplifySource ? toPlainText().simplified() : toPlainText().trimmed();
}

void SourceTextEdit::setSentenceLineBreaks(bool enabled)
{
    m_sentenceLineBreaks = enabled;
    if (enabled && !m_composing)
        formatSentences();
}

void SourceTextEdit::formatSentences()
{
    if (!m_sentenceLineBreaks || m_formatting || m_composing)
        return;
    const auto edits = sentenceBreakEdits(toPlainText());
    if (edits.isEmpty())
        return;
    const QTextCursor original = textCursor();
    const auto mapped = [&edits](int position) {
        int delta = 0;
        for (const auto &edit : edits) {
            if (position < edit.start)
                break;
            if (position < edit.start + edit.length)
                return edit.start + delta + 1;
            delta += 1 - edit.length;
        }
        return position + delta;
    };
    m_formatting = true;
    QTextCursor editor(document());
    editor.joinPreviousEditBlock();
    for (auto it = edits.crbegin(); it != edits.crend(); ++it) {
        editor.setPosition(it->start);
        editor.setPosition(it->start + it->length, QTextCursor::KeepAnchor);
        editor.insertText(QStringLiteral("\n"));
    }
    editor.endEditBlock();
    QTextCursor restored(document());
    restored.setPosition(mapped(original.anchor()));
    restored.setPosition(mapped(original.position()), QTextCursor::KeepAnchor);
    setTextCursor(restored);
    m_formatting = false;
}

void SourceTextEdit::inputMethodEvent(QInputMethodEvent *event)
{
    m_composing = !event->preeditString().isEmpty();
    QPlainTextEdit::inputMethodEvent(event);
    if (!m_composing && m_listenForEdits)
        startTimerDelay();
}

void SourceTextEdit::insertFromMimeData(const QMimeData *source)
{
    QPlainTextEdit::insertFromMimeData(source);
    if (!m_composing)
        formatSentences();
}

void SourceTextEdit::keyPressEvent(QKeyEvent *event)
{
    m_skipFormat = event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo);
    QPlainTextEdit::keyPressEvent(event);
}

void SourceTextEdit::replaceText(const QString &text)
{
    QTextCursor cursor = textCursor();
    cursor.select(QTextCursor::Document);
    if (text.isEmpty())
        cursor.removeSelectedText();
    else
        cursor.insertText(m_sentenceLineBreaks ? withSentenceLineBreaks(text) : text);
    setTextCursor(cursor);

    // To avoid emitting textEdited signal
    m_textEditedTimer->stop();
}

void SourceTextEdit::removeText()
{
    replaceText({});
}

void SourceTextEdit::stopEditTimer()
{
    m_textEditedTimer->stop();
}

void SourceTextEdit::undo()
{
    m_skipFormat = true;
    QPlainTextEdit::undo();
}

void SourceTextEdit::redo()
{
    m_skipFormat = true;
    QPlainTextEdit::redo();
}

void SourceTextEdit::startTimerDelay()
{
    if (m_formatting)
        return;
    m_textEditedTimer->start(500ms);
}

void SourceTextEdit::checkSourceEmptyChanged()
{
    if (toPlainText().isEmpty() != m_sourceEmpty) {
        m_sourceEmpty = toPlainText().isEmpty();
        emit sourceEmpty(m_sourceEmpty);
    }
}

void SourceTextEdit::contextMenuEvent(QContextMenuEvent *event)
{
    m_skipFormat = true;
    auto *contextMenu = new ContextMenu(this, event);
    contextMenu->popup();
}

void SourceTextEdit::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        zoomInF(event->angleDelta().y() / 120.f);
        event->accept();
        return;
    }
    QPlainTextEdit::wheelEvent(event);
}
