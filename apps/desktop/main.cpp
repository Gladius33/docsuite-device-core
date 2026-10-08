// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QTabWidget>
#include <QTimer>

#include <cstdlib>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(true);
    QObject::connect(
        &app,
        &QGuiApplication::lastWindowClosed,
        &app,
        &QCoreApplication::quit,
        Qt::QueuedConnection);

    QCoreApplication::setOrganizationName(QStringLiteral("DocSuite Project"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("docsuite.local"));
    QCoreApplication::setApplicationName(QStringLiteral("DocSuite Device Center"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.5.0"));

    const bool smoke_test =
        QCoreApplication::arguments().contains(QStringLiteral("--smoke-test"));
    if (smoke_test) {
        qputenv("DOCSUITE_SMOKE_TEST", QByteArrayLiteral("1"));
    }

    docsuite::desktop::MainWindow window;
    window.show();

    if (smoke_test) {
        // Reproduce a user rapidly visiting every lazy-loaded page. Hardware
        // access remains disabled by DOCSUITE_SMOKE_TEST, but all page
        // constructors, watchers and tab replacement paths are exercised.
        QTimer::singleShot(0, &window, [&window]() {
            if (auto* tabs = window.findChild<QTabWidget*>(); tabs != nullptr) {
                for (int index = 0; index < tabs->count(); ++index) {
                    tabs->setCurrentIndex(index);
                }
                tabs->setCurrentIndex(0);
            }
        });
        QTimer::singleShot(1800, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
