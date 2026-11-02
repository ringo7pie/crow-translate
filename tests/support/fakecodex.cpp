/*
 * SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTextStream>
#include <QThread>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--child"))) {
        QThread::sleep(30);
        return 0;
    }
    QFile input;
    input.open(stdin, QIODevice::ReadOnly);
    const QByteArray prompt = input.readAll();
    const QJsonObject request = QJsonDocument::fromJson(prompt.mid(prompt.indexOf('{'))).object();
    const QStringList args = app.arguments();
    const QString mode = qEnvironmentVariable("CROW_CODEX_TEST_MODE");
    const QString recordPath = qEnvironmentVariable("CROW_CODEX_TEST_RECORD");
    QJsonObject record{{QStringLiteral("arguments"), QJsonArray::fromStringList(args)},
                       {QStringLiteral("prompt"), QString::fromUtf8(prompt)},
                       {QStringLiteral("input"), request},
                       {QStringLiteral("directory"), QDir::currentPath()}};
    QProcess child;
    if (mode == QLatin1String("tree")) {
        child.start(app.applicationFilePath(), {QStringLiteral("--child")});
        if (!child.waitForStarted())
            return 7;
        record.insert(QStringLiteral("child_pid"), static_cast<qint64>(child.processId()));
    }
    if (!recordPath.isEmpty()) {
        QFile recordFile(recordPath);
        if (!recordFile.open(QIODevice::WriteOnly))
            return 8;
        recordFile.write(QJsonDocument(record).toJson());
    }
    if (mode == QLatin1String("hang") || mode == QLatin1String("tree"))
        QThread::sleep(30);
    QTextStream(stdout) << "diagnostic stdout that must never appear in the translation\n";
    QTextStream(stderr) << "diagnostic stderr\n";
    if (mode == QLatin1String("error"))
        return 2;
    const int outputIndex = args.indexOf(QStringLiteral("--output-last-message"));
    if (outputIndex < 0 || outputIndex + 1 >= args.size())
        return 3;
    QFile output(args.at(outputIndex + 1));
    if (!output.open(QIODevice::WriteOnly))
        return 4;
    if (mode == QLatin1String("invalid")) {
        output.write("not JSON");
        return 0;
    }
    QString source = request.value(QStringLiteral("source_language")).toString();
    if (source == QLatin1String("auto"))
        source = QStringLiteral("en");
    QString target = request.value(QStringLiteral("target_language")).toString();
    const bool detection = target.isEmpty();
    QString translation = mode == QLatin1String("echo") ? request.value(QStringLiteral("text")).toString() : QStringLiteral("contract test output");
    if (mode == QLatin1String("empty") || detection)
        translation.clear();
    if (mode == QLatin1String("wrong-language"))
        target = QStringLiteral("ja");
    output.write(QJsonDocument(QJsonObject{{QStringLiteral("translation"), translation},
                                           {QStringLiteral("source_language"), source},
                                           {QStringLiteral("target_language"), target}})
                     .toJson());
    return 0;
}
