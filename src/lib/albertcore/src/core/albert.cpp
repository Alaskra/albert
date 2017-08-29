// albert - a simple application launcher for linux
// Copyright (C) 2014-2017 Manuel Schneider
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "albert.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTime>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>
#include <csignal>
#include <functional>
#include "extensionmanager.h"
#include "frontend.h"
#include "globalshortcut/hotkeymanager.h"
#include "frontendmanager.h"
#include "pluginspec.h"
#include "querymanager.h"
#include "settingswidget.h"
#include "trayicon.h"
#include "xdg/iconlookup.h"
using namespace Core;

static void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &message);
static void dispatchMessage();

// Core components
static QApplication     *app;
static ExtensionManager *extensionManager;
static FrontendManager  *frontendManager;
static QueryManager     *queryManager;
static HotkeyManager    *hotkeyManager;
static SettingsWidget   *settingsWidget;
static TrayIcon         *trayIcon;
static QMenu            *trayIconMenu;
static QLocalServer     *localServer;


int Core::AlbertApp::run(int argc, char **argv) {

    {
        bool showSettingsWhenInitialized = false;

        /*
         *  INITIALIZE APPLICATION
         */

        qInstallMessageHandler(myMessageOutput);

        qDebug() << "Initializing application";
        app = new QApplication(argc, argv);
        app->setApplicationName("albert");
        app->setApplicationDisplayName("Albert");
        app->setApplicationVersion("v0.12.0");
        app->setQuitOnLastWindowClosed(false);
        QString icon = XDG::IconLookup::iconPath("albert");
        if ( icon.isEmpty() ) icon = ":app_icon";
        app->setWindowIcon(QIcon(icon));


        /*
         *  PARSE COMMANDLINE
         */

        qDebug() << "Parsing commandline";
        QCommandLineParser parser;
        parser.setApplicationDescription("Albert is still in alpha. These options may change in future versions.");
        parser.addHelpOption();
        parser.addVersionOption();
        parser.addOption(QCommandLineOption({"k", "hotkey"}, "Overwrite the hotkey to use.", "hotkey"));
        parser.addOption(QCommandLineOption({"p", "plugin-dirs"}, "Set the plugin dirs to use. Comma separated.", "directory"));
        parser.addPositionalArgument("command", "Command to send to a running instance, if any. (show, hide, toggle)", "[command]");
        parser.process(*app);


        /*
         *  IPC/SINGLETON MECHANISM
         */

        qDebug() << "Checking for other instances";
        QString socketPath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/socket";
        const QStringList args = parser.positionalArguments();
        QLocalSocket socket;
        socket.connectToServer(socketPath);
        if ( socket.waitForConnected(500) ) {
            // If there is a command send it
            if ( args.count() == 1 ){
                socket.write(args.at(0).toLocal8Bit());
                socket.flush();
                socket.waitForReadyRead(500);
                if (socket.bytesAvailable())
                    qInfo() << socket.readAll();
            }
            else
                qInfo("There is another instance of albert running.");
            socket.close();
            ::exit(EXIT_SUCCESS);
        } else if ( args.count() == 1 ) {
            qInfo("There is no other instance of albert running.");
            ::exit(EXIT_FAILURE);
        }

        // Remove pipes potentially leftover after crash
        QLocalServer::removeServer(socketPath);

        // Create server and handle messages
        qDebug() << "Creating IPC server";
        localServer = new QLocalServer;
        if ( !localServer->listen(socketPath) )
            qWarning() << "Local server could not be created. IPC will not work! Reason:"
                       << localServer->errorString();

        // Handle incoming messages
        QObject::connect(localServer, &QLocalServer::newConnection, dispatchMessage);


        /*
         *  INITIALIZE PATHS
         */

        // Make sure data, cache and config dir exists
        qDebug() << "Initializing mandatory paths";
        QString dataLocation = QStandardPaths::writableLocation(QStandardPaths::DataLocation);
        QString cacheLocation = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QString configLocation = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        for ( const QString &location : {dataLocation, cacheLocation, configLocation} )
            if (!QDir(location).mkpath("."))
                qFatal("Could not create dir: %s",  qPrintable(location));

        // Move old config for user convenience TODO drop somewhen
        QSettings::setPath(QSettings::defaultFormat(), QSettings::UserScope,
                           QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
        QFileInfo oldcfg(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/albert.conf");
        QFileInfo newcfg(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/albert/albert.conf");
        if (oldcfg.exists()){
            if (newcfg.exists())
                QFile::remove(newcfg.filePath());
            QFile::rename(oldcfg.filePath(), newcfg.filePath());
        }


        /*
         * DETECT FIRST RUN AND VERSION CHANGE
         */

        // If there is a file in .cache, move it
        if ( QFile::exists(QString("%1/firstrun").arg(cacheLocation)) ){
            qDebug() << "Moving 'firstrun' to new path";
            QFile::rename(QString("%1/firstrun").arg(cacheLocation),
                          QString("%1/firstrun").arg(dataLocation));
        }

        // If there is a firstRun file, rename it to lastVersion (since v0.11)
        if ( QFile::exists(QString("%1/firstrun").arg(dataLocation)) )
            qDebug() << "Renaming 'firstrun' to 'last_used_version'";
            QFile::rename(QString("%1/firstrun").arg(dataLocation),
                          QString("%1/last_used_version").arg(dataLocation));

        qDebug() << "Checking last used version";
        QFile file(QString("%1/last_used_version").arg(dataLocation));
        if ( file.exists() ) {
            // Read last used version
            if ( file.open(QIODevice::ReadOnly|QIODevice::Text) ) {
                QString lastUsedVersion;
                QTextStream(&file) >> lastUsedVersion;
                file.close();

                // Show newsbox in case of major version change
                if ( app->applicationVersion().section('.', 1, 1) != lastUsedVersion.section('.', 1, 1) ){
                    // Do whatever is neccessary on first run
                    QMessageBox(QMessageBox::Information, "Major version changed",
                                QString("You are now using Albert %1. Albert is still in the alpha "
                                        "stage. This means things may change unexpectedly. Check "
                                        "the <a href=\"https://albertlauncher.github.io/news/\">"
                                        "news</a> to read about the things that changed.")
                                .arg(app->applicationVersion())
                                ).exec();
                }
            }
            else
                qCritical() << qPrintable(QString("Could not open file %1: %2,. Config migration may fail.")
                                          .arg(file.fileName(), file.errorString()));
        } else {
            // Do whatever is neccessary on first run
            if ( QMessageBox(QMessageBox::Information, "First run",
                             "Seems like this is the first time you run Albert. "
                             "Most probably you want to set a hotkey to show "
                             "Albert. Do you want to open the settings dialog?",
                             QMessageBox::No|QMessageBox::Yes).exec() == QMessageBox::Yes )
                showSettingsWhenInitialized = true;
        }

        // Write the current version into the file
        if ( file.open(QIODevice::WriteOnly|QIODevice::Text) ) {
            QTextStream out(&file);
            out << app->applicationVersion();
            file.close();
        } else
            qCritical() << qPrintable(QString("Could not open file %1: %2").arg(file.fileName(), file.errorString()));


        /*
         * INITIALIZE DATABASE
         */

        qDebug() << "Initializing database";
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
        if ( !db.isValid() )
            qFatal("No sqlite available");

        if (!db.driver()->hasFeature(QSqlDriver::Transactions))
            qFatal("QSqlDriver::Transactions not available.");

        db.setDatabaseName(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("core.db"));
        if (!db.open())
            qFatal("Unable to establish a database connection.");

        db.transaction();

        // Create tables
        QSqlQuery q;
        if (!q.exec("CREATE TABLE IF NOT EXISTS usages ( "
                    "  input TEXT NOT NULL, "
                    "  itemId TEXT, "
                    "  timestamp DATETIME DEFAULT CURRENT_TIMESTAMP "
                    ");"))
            qFatal("Unable to create table 'usages': %s", q.lastError().text().toUtf8().constData());

        if (!q.exec("CREATE TABLE IF NOT EXISTS runtimes ( "
                    "  extensionId TEXT NOT NULL, "
                    "  runtime INTEGER NOT NULL, "
                    "  timestamp DATETIME DEFAULT CURRENT_TIMESTAMP "
                    ");"))
            qFatal("Unable to create table 'runtimes': %s", q.lastError().text().toUtf8().constData());

        // Do regular cleanup
        if (!q.exec("DELETE FROM usages WHERE julianday('now')-julianday(timestamp)>90;"))
            qWarning("Unable to cleanup usages table.");

        if (!q.exec("DELETE FROM runtimes WHERE julianday('now')-julianday(timestamp)>7;"))
            qWarning("Unable to cleanup runtimes table.");

        db.commit();


        /*
         *  INITIALIZE APPLICATION COMPONENTS
         */

        qDebug() << "Initializing core components";


        // Define plugindirs
        QStringList pluginDirs;
        if ( parser.isSet("plugin-dirs") )
            pluginDirs = parser.value("plugin-dirs").split(',');
        else {
#if defined __linux__
            QStringList dirs = {
                "/usr/lib/", "/usr/local/lib/", "/usr/lib64/", "/usr/local/lib64/",
                QDir::home().filePath(".local/lib/"),
                QDir::home().filePath(".local/lib64/")
            };

            for ( const QString& dir : dirs ) {
                QFileInfo fileInfo = QFileInfo(QDir(dir).filePath("albert/plugins"));
                if ( fileInfo.isDir() )
                    pluginDirs.push_back(fileInfo.canonicalFilePath());
            }
#elif defined __APPLE__
        throw "Not implemented";
#elif defined _WIN32
        throw "Not implemented";
#endif
        }

        frontendManager = new FrontendManager(pluginDirs);
        extensionManager = new ExtensionManager(pluginDirs);

        extensionManager->reloadExtensions();

        hotkeyManager = new HotkeyManager;
        queryManager  = new QueryManager(extensionManager);


        /*
         * Build Tray Icon
         */

        qDebug() << "Initializing tray icon";
        trayIcon      = new TrayIcon;
        trayIconMenu  = new QMenu;
        QAction* showAction     = new QAction("Show", trayIconMenu);
        QAction* settingsAction = new QAction("Settings", trayIconMenu);
        QAction* quitAction     = new QAction("Quit", trayIconMenu);

        showAction->setIcon(app->style()->standardIcon(QStyle::SP_TitleBarMaxButton));
        settingsAction->setIcon(app->style()->standardIcon(QStyle::SP_FileDialogDetailedView));
        quitAction->setIcon(app->style()->standardIcon(QStyle::SP_TitleBarCloseButton));

        trayIconMenu->addAction(showAction);
        trayIconMenu->addAction(settingsAction);
        trayIconMenu->addSeparator();
        trayIconMenu->addAction(quitAction);

        trayIcon->setContextMenu(trayIconMenu);


        /*
         *  Standalone note
         */

        QSettings settings(qApp->applicationName());
        bool standsalone_note_shown = settings.value("standsalone_note_shown", false).toBool();
        if ( !standsalone_note_shown ) {
            QMessageBox(QMessageBox::Information, "Note",
                        "This is standalone free and open source software. Albert is not "
                        "related or affiliated to any other projects or corporations.").exec();
            settings.setValue("standsalone_note_shown", true);
        }


        /*
         *  Hotkey
         */

        qDebug() << "Setting up hotkey";
        // Check for a command line override
        QString hotkey;
        if ( parser.isSet("hotkey") ) {
            hotkey = parser.value("hotkey");
            if ( !hotkeyManager->registerHotkey(hotkey) )
                qFatal("Failed to set hotkey to %s.", hotkey.toLocal8Bit().constData());

        // Check if the settings contains a hotkey entry
        } else if ( settings.contains("hotkey") ) {
            hotkey = settings.value("hotkey").toString();
            if ( !hotkeyManager->registerHotkey(hotkey) ){
                if ( QMessageBox(QMessageBox::Critical, "Error",
                                 QString("Failed to set hotkey: '%1'. Do you want to open the settings?").arg(hotkey),
                                 QMessageBox::No|QMessageBox::Yes).exec() == QMessageBox::Yes )
                    showSettingsWhenInitialized = true;
            }
        }


        /*
         *  MISC
         */

        // Quit gracefully on unix signals
        qDebug() << "Setup signal handlers";
        for ( int sig : { SIGINT, SIGTERM, SIGHUP, SIGPIPE } ) {
            signal(sig, [](int){
                QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
            });
        }

        // Print a message if the app was not terminated graciously
        qDebug() << "Creating running indicator file";
        QString filePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/running";
        if (QFile::exists(filePath)){
            qWarning() << "Application has not been terminated graciously.";
        } else {
            // Create the running indicator file
            QFile file(filePath);
            if (!file.open(QIODevice::WriteOnly))
                qWarning() << "Could not create file:" << filePath;
            file.close();
        }

        // Application is initialized create the settings widget
        qDebug() << "Creating settings widget";
        settingsWidget = new SettingsWidget(extensionManager,
                                            frontendManager,
                                            hotkeyManager,
                                            trayIcon);

        // If somebody requested the settings dialog open it
        if ( showSettingsWhenInitialized )
            settingsWidget->show();


        /*
         * SIGNALING
         */

        qDebug() << "Setting up signals";

        // Connect tray menu (except for frontend stuff…)
        QObject::connect(settingsAction, &QAction::triggered,
                         settingsWidget, &SettingsWidget::show);

        QObject::connect(settingsAction, &QAction::triggered,
                         settingsWidget, &SettingsWidget::raise);

        QObject::connect(quitAction, &QAction::triggered,
                         app, &QApplication::quit);


        // Define a lambda that connects a new frontend
        auto connectFrontend = [&](Frontend *f){

            QObject::connect(hotkeyManager, &HotkeyManager::hotKeyPressed,
                             f, &Frontend::toggleVisibility);

            QObject::connect(queryManager, &QueryManager::resultsReady,
                             f, &Frontend::setModel);

            QObject::connect(showAction, &QAction::triggered,
                             f, &Frontend::setVisible);

            QObject::connect(trayIcon, &TrayIcon::activated, [=](QSystemTrayIcon::ActivationReason reason){
                if( reason == QSystemTrayIcon::ActivationReason::Trigger)
                    f->toggleVisibility();
            });

            QObject::connect(f, &Frontend::settingsWidgetRequested,
                             std::bind(&SettingsWidget::setVisible, settingsWidget, true));

            QObject::connect(f, &Frontend::settingsWidgetRequested,
                             settingsWidget, &SettingsWidget::raise);

            QObject::connect(f, &Frontend::widgetShown,
                             queryManager, &QueryManager::setupSession);

            QObject::connect(f, &Frontend::widgetHidden,
                             queryManager, &QueryManager::teardownSession);

            QObject::connect(f, &Frontend::inputChanged,
                             queryManager, &QueryManager::startQuery);
        };

        // Connect the current frontend
        connectFrontend(frontendManager->currentFrontend());

        // Connect new frontends
        QObject::connect(frontendManager, &FrontendManager::frontendChanged, [&](Frontend *f) {
            connectFrontend(f);
        });
    }


    /*
     * ENTER EVENTLOOP
     */

    qDebug() << "Entering eventloop";
    int retval = app->exec();


    /*
     *  FINALIZE APPLICATION
     */

    qDebug() << "Cleaning up core components";
    delete settingsWidget;
    delete trayIconMenu;
    delete trayIcon;
    delete queryManager;
    delete hotkeyManager;
    delete extensionManager;
    delete frontendManager;

    qDebug() << "Shutting down IPC server";
    localServer->close();

    // Delete the running indicator file
    qDebug() << "Deleting running indicator file";
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/running");

    qDebug() << "Quit";
    return retval;
}


/** ***************************************************************************/
void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    switch (type) {
    case QtDebugMsg:
       fprintf(stdout, "[%s] \x1b[3m[DEBG] %s\x1b[0m\n",
               QTime::currentTime().toString().toLocal8Bit().constData(),
               message.toLocal8Bit().constData());
       fflush(stdout);
       break;
    case QtInfoMsg:
        fprintf(stdout, "[%s] [INFO] %s\n",
                QTime::currentTime().toString().toLocal8Bit().constData(),
                message.toLocal8Bit().constData());
        fflush(stdout);
        break;
    case QtWarningMsg:
        fprintf(stderr, "[%s] \x1b[33;1m[WARN]\x1b[0;1m %s  --  [%s]\x1b[0m\n",
                QTime::currentTime().toString().toLocal8Bit().constData(),
                message.toLocal8Bit().constData(),
                context.function);
        break;
    case QtCriticalMsg:
        fprintf(stderr, "[%s] \x1b[31;1m[CRIT]\x1b[0;1m %s  --  [%s]\x1b[0m\n",
                QTime::currentTime().toString().toLocal8Bit().constData(),
                message.toLocal8Bit().constData(),
                context.function);
        break;
    case QtFatalMsg:
        fprintf(stderr, "[%s] \x1b[41;30;4m[FATAL]\x1b[0;1m %s  --  [%s]\x1b[0m\n",
                QTime::currentTime().toString().toLocal8Bit().constData(),
                message.toLocal8Bit().constData(),
                context.function);
        exit(1);
    }
}


/** ***************************************************************************/
void dispatchMessage() {
    QLocalSocket* socket = localServer->nextPendingConnection(); // Should be safe
    socket->waitForReadyRead(500);
    if (socket->bytesAvailable()) {
        QString msg = QString::fromLocal8Bit(socket->readAll());
        if ( msg == "show") {
            frontendManager->currentFrontend()->setVisible(true);
            socket->write("Application set visible.");
        } else if ( msg == "hide") {
            frontendManager->currentFrontend()->setVisible(false);
            socket->write("Application set invisible.");
        } else if ( msg == "toggle") {
            frontendManager->currentFrontend()->toggleVisibility();
            socket->write("Visibility toggled.");
        } else
            socket->write("Command not supported.");
    }
    socket->flush();
    socket->close();
    socket->deleteLater();
}

