/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "qhotkey.h"
#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>
int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2)
        return 2;
    QHotkey hotkey(QKeySequence(QString::fromLocal8Bit(argv[1])), true);
    QTextStream(stdout) << (hotkey.isRegistered() ? "READY\n" : "UNAVAILABLE\n") << Qt::flush;
    if (!hotkey.isRegistered())
        return 1;
    QTimer::singleShot(30000, &application, &QCoreApplication::quit);
    return application.exec();
}
