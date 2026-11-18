/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QDateTime>
#include <QHash>
#include <QTableWidget>

// 同時通訳の字幕。2 列（言語 A / 言語 B）の表に、確定した文を 1 行ずつ追記する。
// 原文が出た時点で行を足し、訳文は同じ行の空いた列に 1 回だけ入れる。入れた文は書き換えない
class TranscriptView : public QTableWidget
{
    Q_OBJECT
    Q_DISABLE_COPY(TranscriptView)
public:
    explicit TranscriptView(QWidget *parent = nullptr);

    // 列の見出し（言語名）を設定する
    void setColumnTitles(const QString &titleA, const QString &titleB);
    // 原文の行を足す。引数: entryId = 対応付けの番号、fromSelf = 自分の発言か、column = 原文の列（0 / 1）、
    //   speakerLabel = 行見出しに出す話者名（空なら「自分」/「相手」）、speakerIndex = 相手の話者の番号（色分け。-1 なら無し）
    // 戻り値: 足した行
    int addEntry(quint64 entryId, bool fromSelf, int column, const QString &text, const QDateTime &time,
                 const QString &speakerLabel = QString(), int speakerIndex = -1);
    // 話者の番号に対応する文字色を返す（8 色を繰り返す）
    static QColor speakerColor(int speakerIndex);
    // 訳文を入れる。行が無い・既に入っているときは何もせず false を返す
    bool setTranslation(quint64 entryId, const QString &text);
    // 全行を消す
    void clearEntries();
    // 字幕をテキストにする（保存用）
    QString toPlainText() const;
    // 文字の大きさ（ポイント）
    void setFontPointSize(qreal pointSize);
    // 一番下に追従しているか（一番下までスクロールしてあれば true。行が増えると自動で一番下へ送る）
    bool isFollowingBottom() const;

signals:
    // Ctrl+ホイールで文字の大きさが変わった
    void fontPointSizeChanged(qreal pointSize);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QTableWidgetItem *makeItem(const QString &text) const;

    QHash<quint64, int> m_rows; // 対応付けの番号 → 行
    bool m_followBottom = true; // 一番下に追従しているか（利用者が一番下までスクロールしたら true、上へ戻したら false）
};
