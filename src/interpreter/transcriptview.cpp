/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "transcriptview.h"

#include <QHeaderView>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>

namespace {
constexpr qreal minimumPointSize = 6.0; // 文字の大きさの下限
constexpr qreal maximumPointSize = 72.0; // 文字の大きさの上限
}

TranscriptView::TranscriptView(QWidget *parent)
    : QTableWidget(0, 2, parent)
{
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setWordWrap(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    verticalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    verticalHeader()->setDefaultAlignment(Qt::AlignTop | Qt::AlignHCenter);
    setAlternatingRowColors(true);
}

void TranscriptView::setColumnTitles(const QString &titleA, const QString &titleB)
{
    setHorizontalHeaderLabels({titleA, titleB});
}

// 表示用のセルを作る
QTableWidgetItem *TranscriptView::makeItem(const QString &text) const
{
    auto *item = new QTableWidgetItem(text); // セル
    item->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    return item;
}

// 最下部まで見ているか（自動スクロールの判定）
bool TranscriptView::isAtBottom() const
{
    const QScrollBar *bar = verticalScrollBar(); // 縦のスクロールバー
    return bar->value() >= bar->maximum() - 4;
}

// 原文の行を足す
int TranscriptView::addEntry(quint64 entryId, bool fromSelf, int column, const QString &text, const QDateTime &time, const QString &speakerLabel, int speakerIndex)
{
    const bool follow = isAtBottom(); // 足した後に最下部へ送るか
    const int row = rowCount(); // 足す行
    insertRow(row);
    const QString speakerName = !speakerLabel.isEmpty() ? speakerLabel : (fromSelf ? tr("Me") : tr("Remote")); // 表示する話者名
    auto *header = new QTableWidgetItem(time.toString(QStringLiteral("HH:mm:ss")) + QLatin1Char('\n') + speakerName); // 行見出し（時刻と話者）
    QTableWidgetItem *item = makeItem(text); // 原文のセル
    if (!fromSelf) {
        const QColor color = speakerIndex >= 0 ? speakerColor(speakerIndex) : palette().color(QPalette::Link); // 話者の色
        header->setForeground(color);
        if (speakerIndex >= 0)
            item->setForeground(color);
    }
    setVerticalHeaderItem(row, header);
    if (fromSelf) {
        QFont font = item->font(); // 自分の発言は太字にして見分ける
        font.setBold(true);
        item->setFont(font);
    }
    setItem(row, std::clamp(column, 0, 1), item);
    m_rows.insert(entryId, row);
    resizeRowToContents(row);
    if (follow)
        scrollToBottom();
    return row;
}

// 話者ごとの文字色（明るい背景でも暗い背景でも読める中間の明るさ）
QColor TranscriptView::speakerColor(int speakerIndex)
{
    static const QColor colors[] = {
        QColor(0x1f, 0x77, 0xb4), // 青
        QColor(0xd6, 0x27, 0x28), // 赤
        QColor(0x2c, 0xa0, 0x2c), // 緑
        QColor(0x94, 0x67, 0xbd), // 紫
        QColor(0xff, 0x7f, 0x0e), // 橙
        QColor(0x17, 0xbe, 0xcf), // 水色
        QColor(0xe3, 0x77, 0xc2), // 桃
        QColor(0x8c, 0x56, 0x4b), // 茶
    }; // 8 色
    constexpr int colorCount = int(sizeof(colors) / sizeof(colors[0])); // 色の数
    return colors[((speakerIndex % colorCount) + colorCount) % colorCount];
}

// 訳文を空いた列に 1 回だけ入れる
bool TranscriptView::setTranslation(quint64 entryId, const QString &text)
{
    const auto found = m_rows.constFind(entryId); // 対応する行
    if (found == m_rows.constEnd())
        return false;
    const int row = found.value(); // 行
    const int column = item(row, 0) == nullptr ? 0 : 1; // 空いている列
    if (item(row, column) != nullptr)
        return false;
    const bool follow = isAtBottom(); // 足した後に最下部へ送るか
    setItem(row, column, makeItem(text));
    m_rows.erase(found);
    resizeRowToContents(row);
    if (follow)
        scrollToBottom();
    return true;
}

void TranscriptView::clearEntries()
{
    m_rows.clear();
    setRowCount(0);
}

// 字幕をテキストにする
QString TranscriptView::toPlainText() const
{
    QString text; // 出力
    for (int row = 0; row < rowCount(); ++row) {
        const QTableWidgetItem *header = verticalHeaderItem(row); // 時刻と話者
        text += QLatin1Char('[') + (header ? header->text().replace(QLatin1Char('\n'), QLatin1Char(' ')) : QString()) + QStringLiteral("]\n");
        for (int column = 0; column < columnCount(); ++column) {
            const QTableWidgetItem *cell = item(row, column); // セル
            const QTableWidgetItem *title = horizontalHeaderItem(column); // 列の見出し
            text += (title ? title->text() : QString::number(column)) + QStringLiteral(": ") + (cell ? cell->text() : QString()) + QLatin1Char('\n');
        }
        text += QLatin1Char('\n');
    }
    return text;
}

void TranscriptView::setFontPointSize(qreal pointSize)
{
    QFont newFont = font(); // 新しい文字
    newFont.setPointSizeF(std::clamp(pointSize, minimumPointSize, maximumPointSize));
    setFont(newFont);
    resizeRowsToContents();
}

// Ctrl+ホイールで拡大縮小する（sourcetextedit.cpp と同じ操作）
void TranscriptView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const qreal steps = event->angleDelta().y() / 120.0; // ホイールの段数
        if (steps != 0.0) {
            setFontPointSize(font().pointSizeF() + steps);
            emit fontPointSizeChanged(font().pointSizeF());
        }
        event->accept();
        return;
    }
    QTableWidget::wheelEvent(event);
}

// 幅が変わると折り返しが変わるので行の高さを合わせ直す
void TranscriptView::resizeEvent(QResizeEvent *event)
{
    const bool follow = isAtBottom(); // 最下部を保つか
    QTableWidget::resizeEvent(event);
    resizeRowsToContents();
    if (follow)
        scrollToBottom();
}
