#include "widget.h"
#include "ui_widget.h"
#include "character.h"
#include "message.h"
#include "utils.h"

#ifdef WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

Widget::Widget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Widget),
    cmdPeer(new Player())
{
    tcpServer = new QTcpServer(this);
    udpSocket = new QUdpSocket(this);
    tcpReceivedDatas = new QByteArray();
    ui->setupUi(this);

    pingTimer = new QTimer(this);
}

void Widget::logStatusMessage(QString msg)
{
    ui->log->appendPlainText(msg);
    ui->status->setText(msg);
    ui->log->repaint();
    ui->status->repaint();
}

void Widget::logMessage(QString msg)
{
    if (!logInfos)
        return;
    ui->log->appendPlainText(msg);
    ui->log->repaint();
}

void Widget::startServer()
{
    logStatusMessage("Private server v0.4.2");
    lastNetviewId=0;
    lastId=0;

    /// Read config
    logStatusMessage("Reading config file ...");
    QSettings config(CONFIGFILEPATH, QSettings::IniFormat);
    loginPort = config.value("loginPort", 1031).toInt();
    gamePort = config.value("gamePort", 1039).toInt();
    maxConnected = config.value("maxConnected",128).toInt();
    maxRegistered = config.value("maxRegistered",2048).toInt();
    pingTimeout = config.value("pingTimeout", 15).toInt();
    pingCheckInterval = config.value("pingCheckInterval", 5000).toInt();
    logInfos = config.value("logInfosMessages", true).toBool();
    saltPassword = config.value("saltPassword", "Change Me").toString();
    enableLoginServer = config.value("enableLoginServer", true).toBool();
    enableGameServer = config.value("enableGameServer", true).toBool();
    enableMultiplayer = config.value("enableMultiplayer", true).toBool();
    syncInterval = config.value("syncInterval",DEFAULT_SYNC_INTERVAL).toInt();
    remoteLoginIP = config.value("remoteLoginIP", "127.0.0.1").toString();
    remoteLoginPort = config.value("remoteLoginPort", 1031).toInt();
    remoteLoginTimeout = config.value("remoteLoginTimeout", 5000).toInt();
    useRemoteLogin = config.value("useRemoteLogin", false).toBool();

    /// Init servers
    tcpClientsList.clear();
#ifdef WIN32
    startTimestamp = GetTickCount();
#else
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    startTimestamp = tp.tv_sec*1000 + tp.tv_nsec/1000/1000;
#endif
    // Read vortex DB
    if (enableGameServer)
    {
        bool corrupted=false;
        QDir vortexDir("data/vortex/");
        QStringList files = vortexDir.entryList(QDir::Files);
        int nVortex=0;
        for (int i=0; i<files.size(); i++) // For each vortex file
        {
            Scene scene(files[i].split('.')[0]);

            QFile file("data/vortex/"+files[i]);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                logStatusMessage("Error reading vortex DB");
                return;
            }
            QByteArray data = file.readAll();
            data.replace('\r', "");
            QList<QByteArray> lines = data.split('\n');
            for (int j=0; j<lines.size(); j++)
            {
                if (lines[j].size() == 0) // Skip empty lines
                    continue;
                nVortex++;
                Vortex vortex;
                bool ok1, ok2, ok3, ok4;
                QList<QByteArray> elems = lines[j].split(' ');
                if (elems.size() < 5)
                {
                    logStatusMessage("Vortex DB is corrupted. Incorrect line ("
                                    +QString().setNum(elems.size())+" elems), file " + files[i]);
                    corrupted=true;
                    break;
                }
                vortex.id = elems[0].toInt(&ok1, 16);
                vortex.destName = elems[1];
                for (int j=2; j<elems.size() - 3;j++) // Concatenate the string between id and poss
                    vortex.destName += " "+elems[j];
                vortex.destPos.x = elems[elems.size()-3].toFloat(&ok2);
                vortex.destPos.y = elems[elems.size()-2].toFloat(&ok3);
                vortex.destPos.z = elems[elems.size()-1].toFloat(&ok4);
                if (!(ok1&&ok2&&ok3&&ok4))
                {
                    logStatusMessage("Vortex DB is corrupted. Conversion failed, file " + files[i]);
                    corrupted=true;
                    break;
                }
                scene.vortexes << vortex;
                //win.logMessage("Add vortex "+QString().setNum(vortex.id)+" to "+vortex.destName+" "
                //               +QString().setNum(vortex.destPos.x)+" "
                //               +QString().setNum(vortex.destPos.y)+" "
                //               +QString().setNum(vortex.destPos.z));
            }
            scenes << scene;
        }

        if (corrupted)
        {
            stopServer();
            return;
        }

        logMessage("Loaded " + QString().setNum(nVortex) + " vortex in " + QString().setNum(scenes.size()) + " scenes");
    }

    // Read NPC/Quests DB
    if (enableGameServer)
    {
        try
        {
            unsigned nQuests = 0;
            QDir npcsDir("data/npcs/");
            QStringList files = npcsDir.entryList(QDir::Files);
            for (int i=0; i<files.size(); i++, nQuests++) // For each vortex file
            {
                Quest *quest = new Quest("data/npcs/"+files[i]);
                quests << *quest;
                npcs << quest->npc;
            }
            logMessage("Loaded "+QString().setNum(nQuests)+" quests/npcs.");
        }
        catch (QString e)
        {
            enableGameServer = false;
        }
    }

    if (enableLoginServer)
    {
//      logStatusMessage("Loading players database ...");
        tcpPlayers = Player::loadPlayers();
    }

    // TCP server
    if (enableLoginServer)
    {
        logStatusMessage("Starting TCP login server on port "+QString().setNum(loginPort)+" ...");
        if (!tcpServer->listen(QHostAddress::Any,loginPort))
        {
            logStatusMessage("TCP: Unable to start server on port "+QString().setNum(loginPort)+": "+tcpServer->errorString());
            stopServer();
            return;
        }

        // If we use a remote login server, try to open a connection preventively.
        if (useRemoteLogin)
            remoteLoginSock.connectToHost(remoteLoginIP, remoteLoginPort);
    }

    // UDP server
    if (enableGameServer)
    {
        logStatusMessage("Starting UDP game server on port "+QString().setNum(gamePort)+" ...");
        if (!udpSocket->bind(gamePort, QUdpSocket::ReuseAddressHint|QUdpSocket::ShareAddress))
        {
            logStatusMessage("UDP: Unable to start server on port "+QString().setNum(gamePort));
            stopServer();
            return;
        }
    }

    if (enableGameServer)
    {
        // Start ping timeout timer
        pingTimer->start(pingCheckInterval);
    }

    if (enableMultiplayer)
        sync.startSync();

    if (enableLoginServer || enableGameServer)
        logStatusMessage("Server started");

    connect(ui->sendButton, SIGNAL(clicked()), this, SLOT(sendCmdLine()));
    if (enableLoginServer)
        connect(tcpServer, SIGNAL(newConnection()), this, SLOT(tcpConnectClient()));
    if (enableGameServer)
    {
        connect(udpSocket, SIGNAL(readyRead()),this, SLOT(udpProcessPendingDatagrams()));
        connect(pingTimer, SIGNAL(timeout()), this, SLOT(checkPingTimeouts()));
    }
}

void Widget::stopServer()
{
    logStatusMessage("Stopping all server operations");
    pingTimer->stop();
    tcpServer->close();
    for (int i=0;i<tcpClientsList.size();i++)
        tcpClientsList[i]->close();
    udpSocket->close();

    sync.stopSync();

    disconnect(ui->sendButton, SIGNAL(clicked()), this, SLOT(sendCmdLine()));
    disconnect(udpSocket, SIGNAL(readyRead()),this, SLOT(udpProcessPendingDatagrams()));
    disconnect(tcpServer, SIGNAL(newConnection()), this, SLOT(tcpConnectClient()));
    disconnect(pingTimer, SIGNAL(timeout()), this, SLOT(checkPingTimeouts()));
    disconnect(this);
}

// Disconnect players, free the sockets, and exit quickly
// Does NOT run the atexits
Widget::~Widget()
{
    logMessage(QString("UDP: Disconnecting all players"));
    for (;udpPlayers.size();)
    {
        Player* player = udpPlayers[0];
        sendMessage(player, MsgDisconnect, "Connection closed by the server admin");

        // Save the pony
        QList<Pony> ponies = Player::loadPonies(player);
        for (int i=0; i<ponies.size(); i++)
            if (ponies[i].ponyData == player->pony.ponyData)
                ponies[i] = player->pony;
        Player::savePonies(player, ponies);

        // Free
        delete player;
        udpPlayers.removeFirst();
    }

    stopServer();
    delete tcpServer;
    delete tcpReceivedDatas;
    delete udpSocket;
    delete pingTimer;
    delete cmdPeer;

    delete ui;

    // We freed everything that was important, so don't waste time in atexits
#ifdef WIN32
    _exit(EXIT_SUCCESS);
#else
    quick_exit(EXIT_SUCCESS);
#endif
}
