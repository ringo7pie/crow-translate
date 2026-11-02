// SPDX-FileCopyrightText: 2026 ringo7pie
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentenceformatter.h"

#include <QRegularExpression>

QVector<SentenceBreakEdit> sentenceBreakEdits(const QString &text)
{
    // Conservative English rules: protect common abbreviations and address
    // tokens; an English sentence boundary must be followed by whitespace.
    static const QRegularExpression protectedTokens(
        QStringLiteral(R"rx(\b(?:https?://|ftp://|www\.)[^\s<>"「」]+|\b[\w.+-]+@[\w.-]+\.[a-z]{2,}\b|\b(?:Mr|Mrs|Ms|Dr|Prof|Sr|Jr|St|vs|etc)\.|\b(?:[a-z]\.){2,})rx"),
        QRegularExpression::CaseInsensitiveOption);
    QVector<QPair<int, int>> protectedRanges;
    auto matches = protectedTokens.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        int end = int(match.capturedEnd());
        if (match.captured().contains(QStringLiteral("://")) || match.captured().startsWith(QStringLiteral("www."), Qt::CaseInsensitive)) {
            while (end > match.capturedStart() && QStringLiteral(".,!?;:)]}\"'」』）").contains(text.at(end - 1)))
                --end;
        }
        protectedRanges.append({int(match.capturedStart()), end});
    }
    QVector<SentenceBreakEdit> edits;
    qsizetype range = 0;
    const QString closers = QString::fromUtf8("\"'”’»)]}」』】）");
    for (int i = 0; i < text.size(); ++i) {
        const QChar punctuation = text.at(i);
        if (punctuation != QLatin1Char('.') && punctuation != QChar(0x3002))
            continue;
        while (range < protectedRanges.size() && protectedRanges.at(range).second <= i)
            ++range;
        if (range < protectedRanges.size() && protectedRanges.at(range).first <= i)
            continue;
        if (punctuation == QLatin1Char('.')) {
            if ((i > 0 && text.at(i - 1) == QLatin1Char('.')) || (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('.')))
                continue;
            if (i > 0 && i + 1 < text.size() && text.at(i - 1).isDigit() && text.at(i + 1).isDigit())
                continue;
        }
        int start = i + 1;
        while (start < text.size() && closers.contains(text.at(start)))
            ++start;
        if (punctuation == QLatin1Char('.') && start < text.size() && !text.at(start).isSpace())
            continue;
        int end = start;
        while (end < text.size() && (text.at(end) == QLatin1Char(' ') || text.at(end) == QLatin1Char('\t')))
            ++end;
        if (end == text.size() || text.at(end) == QLatin1Char('\n') || text.at(end) == QLatin1Char('\r'))
            continue;
        edits.append({start, end - start});
        i = end - 1;
    }
    return edits;
}

QString withSentenceLineBreaks(const QString &text)
{
    QString result = text;
    const auto edits = sentenceBreakEdits(text);
    for (auto it = edits.crbegin(); it != edits.crend(); ++it)
        result.replace(it->start, it->length, QLatin1String("\n"));
    return result;
}
