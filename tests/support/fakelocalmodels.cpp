/* SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QThread>
#include <iostream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.contains(QStringLiteral("--child"))) {
        QThread::sleep(60);
        return 0;
    }
    const QString root = args.value(args.indexOf(QStringLiteral("--root")) + 1);
    QProcess child;
    if (QFile::exists(root + QStringLiteral("/spawn-child"))) {
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--child")});
        if (!child.waitForStarted(3000))
            return 3;
        QFile pid(root + QStringLiteral("/child.pid"));
        if (!pid.open(QIODevice::WriteOnly))
            return 4;
        pid.write(QByteArray::number(child.processId()));
    }
    QFile count(root + QStringLiteral("/starts.txt"));
    if (!count.open(QIODevice::WriteOnly | QIODevice::Append))
        return 2;
    count.write("started\n");
    count.close();
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto command = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        QFile record(root + QStringLiteral("/request.json"));
        if (!record.open(QIODevice::WriteOnly))
            return 5;
        record.write(QJsonDocument(command).toJson());
        record.close();
        const auto emitEvent = [&command](QJsonObject event) {
            event.insert(QStringLiteral("id"), command.value(QStringLiteral("id")));
            std::cout << QJsonDocument(event).toJson(QJsonDocument::Compact).constData() << std::endl;
        };
        if (!command.value("detect_only").toBool() && command.value("target").toString() != QLatin1String("en")
            && command.value("target").toString() != QLatin1String("ja")) {
            emitEvent({{"event", "error"}, {"message", "Target must be a concrete English/Japanese code"}});
            continue;
        }
        if (QFile::exists(root + QStringLiteral("/fail"))) {
            emitEvent({{"event", "error"}, {"message", "Test model could not load"}});
            continue;
        }
        emitEvent({{"event", "stage"}, {"stage", "loading"}});
        if (QFile::exists(root + QStringLiteral("/slow")))
            QThread::sleep(10);
        const QString source = command.value("source").toString() == QLatin1String("auto") ? QStringLiteral("en") : command.value("source").toString();
        // Stale output must never complete a newer request.
        std::cout << QJsonDocument(QJsonObject{{"id", -1}, {"event", "error"}, {"message", "stale"}}).toJson(QJsonDocument::Compact).constData() << std::endl;
        emitEvent({{"event", "stage"}, {"stage", "translating"}});
        if (QFile::exists(root + QStringLiteral("/timeout")))
            QThread::sleep(10);
        emitEvent({{"event", "result"}, {"source", source}, {"text", command.value("detect_only").toBool() ? QString() : QString::fromUtf8("日本語の訳文\n\n123")}});
    }
    return 0;
}
