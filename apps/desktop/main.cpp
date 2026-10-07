// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "main_window.hpp"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DocSuite Project"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("docsuite.local"));
    QCoreApplication::setApplicationName(QStringLiteral("DocSuite Device Center"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.5.0"));

    docsuite::desktop::MainWindow window;
    window.show();
    return app.exec();
}
