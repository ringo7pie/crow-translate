/*
 * SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QTextStream>
#include <QThread>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    QTextStream out(stdout);
    QTextStream err(stderr);
    out.setEncoding(QStringConverter::Utf8);
    err.setEncoding(QStringConverter::Utf8);
    if (args.size() < 8 || QImage(args.at(1)).isNull()
        || args.mid(2, 6) != QStringList{"--format", "text", "--mode", "ocr", "--cores", "1"}
        || qgetenv("PYTHONIOENCODING") != "utf-8") {
        err << "Invalid image, arguments or encoding";
        return 2;
    }
    const int modelIndex = args.indexOf(QStringLiteral("--model-dir"));
    const QString mode = modelIndex >= 0 ? args.value(modelIndex + 1) : QString();
    if (mode == QLatin1String("slow")) {
        QThread::sleep(10);
    } else if (mode == QLatin1String("fail")) {
        out << "Partial text must never be accepted";
        err << "Missing test models";
        return 1;
    } else if (mode == QLatin1String("empty")) {
        return 0;
    }
    err << "Model loading diagnostics must not enter the transcription";
    out << QString::fromUtf8("Indra test\n日本語の文字認識\n");
    return 0;
}
