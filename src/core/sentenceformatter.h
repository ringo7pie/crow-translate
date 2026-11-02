// SPDX-FileCopyrightText: 2026 ringo7pie
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SENTENCEFORMATTER_H
#define SENTENCEFORMATTER_H

#include <QString>
#include <QVector>

struct SentenceBreakEdit {
    int start;
    int length;
};

QVector<SentenceBreakEdit> sentenceBreakEdits(const QString &text);
QString withSentenceLineBreaks(const QString &text);

#endif
