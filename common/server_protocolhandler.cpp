#include "server_protocolhandler.h"

#include "debug_pb_message.h"
#include "featureset.h"
#include "get_pb_extension.h"
#include "pb/commands.pb.h"
#include "pb/event_game_joined.pb.h"
#include "pb/event_list_rooms.pb.h"
#include "pb/event_notify_user.pb.h"
#include "pb/event_room_say.pb.h"
#include "pb/event_server_message.pb.h"
#include "pb/event_user_message.pb.h"
#include "pb/response.pb.h"
#include "pb/response_get_games_of_user.pb.h"
#include "pb/response_get_user_info.pb.h"
#include "pb/response_join_room.pb.h"
#include "pb/response_list_users.pb.h"
#include "pb/response_login.pb.h"
#include "pb/serverinfo_user.pb.h"
#include "server_database_interface.h"
#include "server_game.h"
#include "server_player.h"
#include "server_room.h"
#include "trice_limits.h"

#include <QDateTime>
#include <QDebug>
#include <QtMath>
#include <google/protobuf/descriptor.h>

Server_ProtocolHandler::Server_ProtocolHandler(Server *_server,
                                               Server_DatabaseInterface *_databaseInterface,
                                               QObject *parent)
    : QObject(parent), Server_AbstractUserInterface(_server), deleted(false), databaseInterface(_databaseInterface),
      authState(NotLoggedIn), usingRealPassword(false), acceptsUserListChanges(false), acceptsRoomListChanges(false),
      idleClientWarningSent(false), timeRunning(0), lastDataReceived(0), lastActionReceived(0)

{
    connect(server, &Server::pingClockTimeout, this, &Server_ProtocolHandler::pingClockTimeout);
}

Server_ProtocolHandler::~Server_ProtocolHandler()
{
}

// This function must only be called from the thread this object lives in.
// Except when the server is shutting down.
// The thread must not hold any server locks when calling this (e.g. clientsLock, roomsLock).
void Server_ProtocolHandler::prepareDestroy()
{
    if (deleted)
        return;
    deleted = true;

    for (auto *room : rooms.values()) {
        room->removeClient(this);
    }

    QMap<int, QPair<int, int>> tempGames(getGames());

    server->roomsLock.lockForRead();
    QMapIterator<int, QPair<int, int>> gameIterator(tempGames);
    while (gameIterator.hasNext()) {
        gameIterator.next();

        Server_Room *room = server->getRooms().value(gameIterator.value().first);
        if (!room)
            continue;
        room->gamesLock.lockForRead();
        Server_Game *game = room->getGames().value(gameIterator.key());
        if (!game) {
            room->gamesLock.unlock();
            continue;
        }
        game->gameMutex.lock();
        Server_Player *p = game->getPlayers().value(gameIterator.value().second);
        if (!p) {
            game->gameMutex.unlock();
            room->gamesLock.unlock();
            continue;
        }

        p->disconnectClient();

        game->gameMutex.unlock();
        room->gamesLock.unlock();
    }
    server->roomsLock.unlock();

    server->removeClient(this);

    deleteLater();
}

void Server_ProtocolHandler::sendProtocolItem(const Response &item)
{
    ServerMessage msg;
    msg.mutable_response()->CopyFrom(item);
    msg.set_message_type(ServerMessage::RESPONSE);

    transmitProtocolItem(msg);
}

void Server_ProtocolHandler::sendProtocolItem(const SessionEvent &item)
{
    ServerMessage msg;
    msg.mutable_session_event()->CopyFrom(item);
    msg.set_message_type(ServerMessage::SESSION_EVENT);

    transmitProtocolItem(msg);
}

void Server_ProtocolHandler::sendProtocolItem(const GameEventContainer &item)
{
    ServerMessage msg;
    msg.mutable_game_event_container()->CopyFrom(item);
    msg.set_message_type(ServerMessage::GAME_EVENT_CONTAINER);

    transmitProtocolItem(msg);
}

void Server_ProtocolHandler::sendProtocolItem(const RoomEvent &item)
{
    ServerMessage msg;
    msg.mutable_room_event()->CopyFrom(item);
    msg.set_message_type(ServerMessage::ROOM_EVENT);

    transmitProtocolItem(msg);
}

Response::ResponseCode Server_ProtocolHandler::processSessionCommandContainer(const CommandContainer &cont,
                                                                              ResponseContainer &rc)
{
    Response::ResponseCode finalResponseCode = Response::RespOk;
    for (int i = cont.session_command_size() - 1; i >= 0; --i) {
        Response::ResponseCode resp = Response::RespInvalidCommand;
        const SessionCommand &sc = cont.session_command(i);
        const int num = getPbExtension(sc);
        if (num != SessionCommand::PING) { // don't log ping commands
            logDebugMessage(getSafeDebugString(sc));
        }
        switch ((SessionCommand::SessionCommandType)num) {
            case SessionCommand::PING:
                resp = cmdPing(sc.GetExtension(Command_Ping::ext), rc);
                break;
            case SessionCommand::LOGIN:
                resp = cmdLogin(sc.GetExtension(Command_Login::ext), rc);
                break;
            case SessionCommand::MESSAGE:
                resp = cmdMessage(sc.GetExtension(Command_Message::ext), rc);
                break;
            case SessionCommand::GET_GAMES_OF_USER:
                resp = cmdGetGamesOfUser(sc.GetExtension(Command_GetGamesOfUser::ext), rc);
                break;
            case SessionCommand::GET_USER_INFO:
                resp = cmdGetUserInfo(sc.GetExtension(Command_GetUserInfo::ext), rc);
                break;
            case SessionCommand::LIST_ROOMS:
                resp = cmdListRooms(sc.GetExtension(Command_ListRooms::ext), rc);
                break;
            case SessionCommand::JOIN_ROOM:
                resp = cmdJoinRoom(sc.GetExtension(Command_JoinRoom::ext), rc);
                break;
            case SessionCommand::LIST_USERS:
                resp = cmdListUsers(sc.GetExtension(Command_ListUsers::ext), rc);
                break;
            default:
                resp = processExtendedSessionCommand(num, sc, rc);
        }
        if (resp != Response::RespOk)
            finalResponseCode = resp;
    }
    return finalResponseCode;
}

Response::ResponseCode Server_ProtocolHandler::processRoomCommandContainer(const CommandContainer &cont,
                                                                           ResponseContainer &rc)
{
    if (authState == NotLoggedIn)
        return Response::RespLoginNeeded;

    QReadLocker locker(&server->roomsLock);
    Server_Room *room = rooms.value(cont.room_id(), 0);
    if (!room)
        return Response::RespNotInRoom;

    resetIdleTimer();

    Response::ResponseCode finalResponseCode = Response::RespOk;
    for (int i = cont.room_command_size() - 1; i >= 0; --i) {
        Response::ResponseCode resp = Response::RespInvalidCommand;
        const RoomCommand &sc = cont.room_command(i);
        const int num = getPbExtension(sc);
        logDebugMessage(getSafeDebugString(sc));
        switch ((RoomCommand::RoomCommandType)num) {
            case RoomCommand::LEAVE_ROOM:
                resp = cmdLeaveRoom(sc.GetExtension(Command_LeaveRoom::ext), room, rc);
                break;
            case RoomCommand::ROOM_SAY:
                resp = cmdRoomSay(sc.GetExtension(Command_RoomSay::ext), room, rc);
                break;
            case RoomCommand::CREATE_GAME:
                resp = cmdCreateGame(sc.GetExtension(Command_CreateGame::ext), room, rc);
                break;
            case RoomCommand::JOIN_GAME:
                resp = cmdJoinGame(sc.GetExtension(Command_JoinGame::ext), room, rc);
                break;
        }
        if (resp != Response::RespOk)
            finalResponseCode = resp;
    }
    return finalResponseCode;
}

Response::ResponseCode Server_ProtocolHandler::processGameCommandContainer(const CommandContainer &cont,
                                                                           ResponseContainer &rc)
{
    static QList<GameCommand::GameCommandType> antifloodCommandsWhiteList =
        QList<GameCommand::GameCommandType>()
        // draw/undo card draw (example: drawing 10 cards one by one from the deck)
        << GameCommand::DRAW_CARDS
        << GameCommand::UNDO_DRAW
        // create, delete arrows (example: targeting with 10 cards during an attack)
        << GameCommand::CREATE_ARROW
        << GameCommand::DELETE_ARROW
        // set card attributes (example: tapping 10 cards at once)
        << GameCommand::SET_CARD_ATTR
        // increment / decrement counter (example: -10 life points one by one)
        << GameCommand::INC_COUNTER
        // mulling lots of hands in a row
        << GameCommand::MULLIGAN
        // allows a user to sideboard without receiving flooding message
        << GameCommand::MOVE_CARD;

    if (authState == NotLoggedIn)
        return Response::RespLoginNeeded;

    QMap<int, QPair<int, int>> gameMap = getGames();
    if (!gameMap.contains(cont.game_id()))
        return Response::RespNotInRoom;
    const QPair<int, int> roomIdAndPlayerId = gameMap.value(cont.game_id());

    QReadLocker roomsLocker(&server->roomsLock);
    Server_Room *room = server->getRooms().value(roomIdAndPlayerId.first);
    if (!room)
        return Response::RespNotInRoom;

    QReadLocker roomGamesLocker(&room->gamesLock);
    Server_Game *game = room->getGames().value(cont.game_id());
    if (!game) {
        if (room->getExternalGames().contains(cont.game_id())) {
            server->sendIsl_GameCommand(cont, room->getExternalGames().value(cont.game_id()).server_id(),
                                        userInfo->session_id(), roomIdAndPlayerId.first, roomIdAndPlayerId.second);
            return Response::RespNothing;
        }
        return Response::RespNotInRoom;
    }

    QMutexLocker gameLocker(&game->gameMutex);
    Server_Player *player = game->getPlayers().value(roomIdAndPlayerId.second);
    if (!player)
        return Response::RespNotInRoom;

    resetIdleTimer();

    int commandCountingInterval = server->getCommandCountingInterval();
    int maxCommandCountPerInterval = server->getMaxCommandCountPerInterval();
    GameEventStorage ges;
    Response::ResponseCode finalResponseCode = Response::RespOk;
    for (int i = cont.game_command_size() - 1; i >= 0; --i) {
        const GameCommand &sc = cont.game_command(i);
        logDebugMessage(QString("game %1 player %2: ").arg(cont.game_id()).arg(roomIdAndPlayerId.second) +
                        getSafeDebugString(sc));

        if (commandCountingInterval > 0) {
            int totalCount = 0;
            if (commandCountOverTime.isEmpty())
                commandCountOverTime.prepend(0);

            if (!antifloodCommandsWhiteList.contains((GameCommand::GameCommandType)getPbExtension(sc)))
                ++commandCountOverTime[0];

            for (int count : commandCountOverTime) {
                totalCount += count;
            }

            if (maxCommandCountPerInterval > 0 && totalCount > maxCommandCountPerInterval) {
                return Response::RespChatFlood;
            }
        }

        Response::ResponseCode resp = player->processGameCommand(sc, rc, ges);

        if (resp != Response::RespOk)
            finalResponseCode = resp;
    }
    ges.sendToGame(game);

    return finalResponseCode;
}

Response::ResponseCode Server_ProtocolHandler::processModeratorCommandContainer(const CommandContainer &cont,
                                                                                ResponseContainer &rc)
{
    if (!userInfo)
        return Response::RespLoginNeeded;
    if (!(userInfo->user_level() & ServerInfo_User::IsModerator))
        return Response::RespLoginNeeded;

    resetIdleTimer();

    Response::ResponseCode finalResponseCode = Response::RespOk;
    for (int i = cont.moderator_command_size() - 1; i >= 0; --i) {
        Response::ResponseCode resp = Response::RespInvalidCommand;
        const ModeratorCommand &sc = cont.moderator_command(i);
        const int num = getPbExtension(sc);
        logDebugMessage(getSafeDebugString(sc));

        resp = processExtendedModeratorCommand(num, sc, rc);
        if (resp != Response::RespOk)
            finalResponseCode = resp;
    }
    return finalResponseCode;
}

Response::ResponseCode Server_ProtocolHandler::processAdminCommandContainer(const CommandContainer &cont,
                                                                            ResponseContainer &rc)
{
    if (!userInfo)
        return Response::RespLoginNeeded;
    if (!(userInfo->user_level() & ServerInfo_User::IsAdmin))
        return Response::RespLoginNeeded;

    resetIdleTimer();

    Response::ResponseCode finalResponseCode = Response::RespOk;
    for (int i = cont.admin_command_size() - 1; i >= 0; --i) {
        Response::ResponseCode resp = Response::RespInvalidCommand;
        const AdminCommand &sc = cont.admin_command(i);
        const int num = getPbExtension(sc);
        logDebugMessage(getSafeDebugString(sc));

        resp = processExtendedAdminCommand(num, sc, rc);
        if (resp != Response::RespOk)
            finalResponseCode = resp;
    }
    return finalResponseCode;
}

void Server_ProtocolHandler::processCommandContainer(const CommandContainer &cont)
{
    // Command processing must be disabled after prepareDestroy() has been called.
    if (deleted)
        return;

    lastDataReceived = timeRunning;

    ResponseContainer responseContainer(cont.has_cmd_id() ? cont.cmd_id() : -1);
    Response::ResponseCode finalResponseCode;

    if (cont.game_command_size())
        finalResponseCode = processGameCommandContainer(cont, responseContainer);
    else if (cont.room_command_size())
        finalResponseCode = processRoomCommandContainer(cont, responseContainer);
    else if (cont.session_command_size())
        finalResponseCode = processSessionCommandContainer(cont, responseContainer);
    else if (cont.moderator_command_size())
        finalResponseCode = processModeratorCommandContainer(cont, responseContainer);
    else if (cont.admin_command_size())
        finalResponseCode = processAdminCommandContainer(cont, responseContainer);
    else
        finalResponseCode = Response::RespInvalidCommand;

    if ((finalResponseCode != Response::RespNothing))
        sendResponseContainer(responseContainer, finalResponseCode);
}

void Server_ProtocolHandler::pingClockTimeout()
{

    int cmdcountinterval = server->getCommandCountingInterval();
    int msgcountinterval = server->getMessageCountingInterval();
    int pingclockinterval = server->getClientKeepAlive();

    int interval = server->getMessageCountingInterval();
    if (interval > 0) {
        if (pingclockinterval > 0) {
            messageSizeOverTime.prepend(0);
            if (messageSizeOverTime.size() > (msgcountinterval / pingclockinterval))
                messageSizeOverTime.removeLast();
            messageCountOverTime.prepend(0);
            if (messageCountOverTime.size() > (msgcountinterval / pingclockinterval))
                messageCountOverTime.removeLast();
        }
    }

    interval = server->getCommandCountingInterval();
    if (interval > 0) {
        if (pingclockinterval > 0) {
            commandCountOverTime.prepend(0);
            if (commandCountOverTime.size() > (cmdcountinterval / pingclockinterval))
                commandCountOverTime.removeLast();
        }
    }

    if (timeRunning - lastDataReceived > server->getMaxPlayerInactivityTime())
        prepareDestroy();

    // PrivLevel users, Moderators, and Admins are not subject to the server idle timeout policy
    const bool hasPrivLevel = userInfo && QString::fromStdString(userInfo->privlevel()).toLower() != "none";
    const bool isModOrAdmin =
        userInfo && (userInfo->user_level() & (ServerInfo_User::IsModerator | ServerInfo_User::IsAdmin));
    if (!hasPrivLevel && !isModOrAdmin) {
        if ((server->getIdleClientTimeout() > 0) && (idleClientWarningSent)) {
            if (timeRunning - lastActionReceived > server->getIdleClientTimeout()) {
                prepareDestroy();
            }
        }

        if (((timeRunning - lastActionReceived) >= qCeil(server->getIdleClientTimeout() * .9)) &&
            (!idleClientWarningSent) && (server->getIdleClientTimeout() > 0)) {
            Event_NotifyUser event;
            event.set_type(Event_NotifyUser::IDLEWARNING);
            SessionEvent *se = prepareSessionEvent(event);
            sendProtocolItem(*se);
            delete se;
            idleClientWarningSent = true;
        }
    }

    ++timeRunning;
}

Response::ResponseCode Server_ProtocolHandler::cmdPing(const Command_Ping & /*cmd*/, ResponseContainer & /*rc*/)
{
    return Response::RespOk;
}

Response::ResponseCode Server_ProtocolHandler::cmdLogin(const Command_Login &cmd, ResponseContainer &rc)
{
    QString userName = nameFromStdString(cmd.user_name()).simplified();
    QString clientId = nameFromStdString(cmd.clientid()).simplified();
    QString clientVersion = nameFromStdString(cmd.clientver()).simplified();
    QString password;
    bool needsHash = false;
    if (cmd.has_password()) {
        if (cmd.password().length() > MAX_NAME_LENGTH)
            return Response::RespWrongPassword;
        password = QString::fromStdString(cmd.password());
        needsHash = true;
    } else if (cmd.hashed_password().length() > MAX_NAME_LENGTH) {
        return Response::RespContextError;
    } else {
        password = nameFromStdString(cmd.hashed_password());
    }

    if (userInfo != 0) {
        return Response::RespContextError;
    }

    // check client feature set against server feature set
    FeatureSet features;
    QMap<QString, bool> receivedClientFeatures;
    QMap<QString, bool> missingClientFeatures;

    int featureCount = qMin(cmd.clientfeatures().size(), MAX_NAME_LENGTH);
    for (int i = 0; i < featureCount; ++i) {
        receivedClientFeatures.insert(nameFromStdString(cmd.clientfeatures(i)).simplified(), false);
    }

    missingClientFeatures =
        features.identifyMissingFeatures(receivedClientFeatures, server->getServerRequiredFeatureList());

    if (!missingClientFeatures.isEmpty()) {
        if (features.isRequiredFeaturesMissing(missingClientFeatures, server->getServerRequiredFeatureList())) {
            Response_Login *re = new Response_Login;
            re->set_denied_reason_str("Client upgrade required");
            QMap<QString, bool>::iterator i;
            for (i = missingClientFeatures.begin(); i != missingClientFeatures.end(); ++i) {
                re->add_missing_features(i.key().toStdString().c_str());
            }
            rc.setResponseExtension(re);
            return Response::RespClientUpdateRequired;
        }
    }

    QString reasonStr;
    int banSecondsLeft = 0;
    QString connectionType = getConnectionType();
    AuthenticationResult res = server->loginUser(this, userName, password, needsHash, reasonStr, banSecondsLeft,
                                                 clientId, clientVersion, connectionType);
    switch (res) {
        case UserIsBanned: {
            Response_Login *re = new Response_Login;
            re->set_denied_reason_str(reasonStr.toStdString());
            if (banSecondsLeft != 0)
                re->set_denied_end_time(QDateTime::currentDateTime().addSecs(banSecondsLeft).toSecsSinceEpoch());
            rc.setResponseExtension(re);
            return Response::RespUserIsBanned;
        }
        case NotLoggedIn:
            return Response::RespWrongPassword;
        case WouldOverwriteOldSession:
            return Response::RespWouldOverwriteOldSession;
        case UsernameInvalid: {
            Response_Login *re = new Response_Login;
            re->set_denied_reason_str(reasonStr.toStdString());
            rc.setResponseExtension(re);
            return Response::RespUsernameInvalid;
        }
        case RegistrationRequired:
            return Response::RespRegistrationRequired;
        case ClientIdRequired:
            return Response::RespClientIdRequired;
        case UserIsInactive:
            return Response::RespAccountNotActivated;
        default:
            authState = res;
            usingRealPassword = needsHash;
    }

    // limit the number of non-privileged users that can connect to the server based on configuration settings
    if (!userInfo || QString::fromStdString(userInfo->privlevel()).toLower() == "none") {
        if (server->getMaxUserLimitEnabled()) {
            if (server->getUsersCount() > server->getMaxUserTotal()) {
                qDebug() << "Max Users Total Limit Reached, please increase the max_users_total setting.";
                return Response::RespServerFull;
            }
        }
    }

    userName = QString::fromStdString(userInfo->name());
    Event_ServerMessage event;
    event.set_message(server->getLoginMessage().toStdString());
    rc.enqueuePostResponseItem(ServerMessage::SESSION_EVENT, prepareSessionEvent(event));

    Response_Login *re = new Response_Login;
    re->mutable_user_info()->CopyFrom(copyUserInfo(true));

    if (authState == PasswordRight) {
        QMapIterator<QString, ServerInfo_User> buddyIterator(databaseInterface->getBuddyList(userName));
        while (buddyIterator.hasNext())
            re->add_buddy_list()->CopyFrom(buddyIterator.next().value());

        QMapIterator<QString, ServerInfo_User> ignoreIterator(databaseInterface->getIgnoreList(userName));
        while (ignoreIterator.hasNext())
            re->add_ignore_list()->CopyFrom(ignoreIterator.next().value());
    }

    // return to client any missing features the server has that the client does not
    if (!missingClientFeatures.isEmpty()) {
        QMap<QString, bool>::iterator i;
        for (i = missingClientFeatures.begin(); i != missingClientFeatures.end(); ++i)
            re->add_missing_features(i.key().toStdString().c_str());
    }

    joinPersistentGames(rc);
    databaseInterface->removeForgotPassword(userName);
    rc.setResponseExtension(re);
    return Response::RespOk;
}

Response::ResponseCode Server_ProtocolHandler::cmdMessage(const Command_Message &cmd, ResponseContainer &rc)
{
    if (authState == NotLoggedIn)
        return Response::RespLoginNeeded;

    QReadLocker locker(&server->clientsLock);

    QString receiver = nameFromStdString(cmd.user_name());
    Server_AbstractUserInterface *userInterface = server->findUser(receiver);
    if (!userInterface) {
        return Response::RespNameNotFound;
    }
    if (databaseInterface->isInIgnoreList(receiver, QString::fromStdString(userInfo->name()))) {
        return Response::RespInIgnoreList;
    }
    if (!addSaidMessageSize(static_cast<int>(cmd.message().size()))) {
        return Response::RespChatFlood;
    }

    Event_UserMessage event;
    event.set_sender_name(userInfo->name());
    event.set_receiver_name(receiver.toStdString());
    event.set_message(cmd.message());

    SessionEvent *se = prepareSessionEvent(event);
    userInterface->sendProtocolItem(*se);
    rc.enqueuePreResponseItem(ServerMessage::SESSION_EVENT, se);

    databaseInterface->logMessage(userInfo->id(), QString::fromStdString(userInfo->name()),
                                  QString::fromStdString(userInfo->address()), QString::fromStdString(cmd.message()),
                                  Server_DatabaseInterface::MessageTargetChat, userInterface->getUserInfo()->id(),
                                  receiver);
    resetIdleTimer();
    return Response::RespOk;
}

Response::ResponseCode Server_ProtocolHandler::cmdGetGamesOfUser(const Command_GetGamesOfUser &cmd,
                                                                 ResponseContainer &rc)
{
    if (authState == NotLoggedIn)
        return Response::RespLoginNeeded;

    // We don't need to check whether the user is logged in; persistent games should also work.
    // The client needs to deal with an empty result list.

    Response_GetGamesOfUser *re = new Response_GetGamesOfUser;
    server->roomsLock.lockForRead();
    QMapIterator<int, Server_Room *> roomIterator(server->getRooms());
    while (roomIterator.hasNext()) {
        Server_Room *room = roomIterator.next().value();
        room->gamesLock.lockForRead();
        room->getInfo(*re->add_room_list(), false, true);
        QListIterator<ServerInfo_Game> gameIterator(room->getGamesOfUser(nameFromStdString(cmd.user_name())));
        while (gameIterator.hasNext())
            re->add_game_list()->CopyFrom(gameIterator.next());
        room->gamesLock.unlock();
    }
    server->roomsLock.unlock();

    rc.setResponseExtension(re);
    return Response::RespOk;
}

Response::ResponseCode Server_ProtocolHandler::cmdGetUserInfo(const Command_GetUserInfo &cmd, ResponseContainer &rc)
{
    if (authState == NotLoggedIn)
        return Response::RespLoginNeeded;

    QString userName = nameFromStdString(cmd.user_name());
    Response_GetUserInfo *re = new Response_GetUserInfo;
    if (userName.isEmpty())
        re->mutable_user_info()->CopyFrom(*userInfo);
    else {

        QReadLocker locker(&server->clientsLock);

        ServerInfo_User_Container *infoSource = server->findUser(userName);
        if (!infoSource) {
            re->mutable_user_info()->CopyFrom(databaseInterface->getUserData(userName, true));
        } else {
            re->mutable_user_info()->CopyFrom(
                infoSource->copyUserInfo(true, false, userInfo->user_level() & ServerInfo_User::IsModerator));
        }
    }

    rc.setResponseExtension(re);
    return Response::RespOk;
}

Response::ResponseCode Server_ProtocolHandler::cmdListRooms(const Command_ListRooms & /*cmd*/, ResponseContainer &rc)
{
    if (authState == NotLoggedIn)
        return Response::RespLoginNeeded;

    Event_ListRooms event;
    QMapIterator<int, Server_Room *> roomIterator(server->getRooms());
    while (roomIterator.hasNext())
        roomIterator.next().value()->getInfo(*event.add_room_list(), false);
    rc.enqueuePreResponseItem(ServerMessage::SESSION_EVENT, prepareSessionEvent(event));

    acceptsRoomListChanges = true;
    return Response::RespOk;
}

Response::ResponseCode Server_ProtocolHandler::cmdJoinRoom(const Command_JoinRoom &cmd, ResponseContainer &rc)
{
    if (authState == NotLoggedIn)
        return Response::RespLoginNeeded;

    if (rooms.contains(cmd.room_id()))
        return Response::RespContextError;

    QReadLocker serverLocker(&server->roomsLock);
    Server_Room *room = server->getRooms().value(cmd.room_id(), 0);
    if (!room)
        return Response::RespNameNotFound;

    if (!(userInfo->user_level() & ServerInfo_User::IsModerator))
        if (!(room->userMayJoin(*userInfo)))
            return Response::RespUserLevelTooLow;

    room->addClient(this);
    rooms.insert(room->getId(), room);

    QReadLocker chatHistoryLocker(&room->historyLock);
    QList<ServerInfo_ChatMessage> chatHistory = room->getChatHistory();
    ServerInfo_ChatMessage chatMessage;
    for (int i = 0; i < chatHistory.size(); ++i) {
        chatMessage = chatHistory.at(i);
        Event_RoomSay roomChatHistory;
        roomChatHistory.set_message(chatMessage.sender_name() + ": " + chatMessage.message());
        roomChatHistory.set_message_type(Event_RoomSay::ChatHistory);
        roomChatHistory.set_time_of(
            QDateTime::fromString(QString::fromStdString(chatMessage.time())).toMSecsSinceEpoch());
        rc.enqueuePostResponseItem(ServerMessage::ROOM_EVENT, room->prepareRoomEvent(roomChatHistory));
    }

    Event_RoomSay joinMessageEvent;
    joinMessageEvent.set_message(room->getJoinMessage().toStdString());
    joinMessageEvent.set_message_type(Event_RoomSay::Welcome);
    rc.enqueuePostResponseItem(ServerMessage::ROOM_EVENT, room->prepareRoomEvent(joinMessageEvent));

    Response_JoinRoom *re = new Response_JoinRoom;
    room->getInfo(*re->mutable_room_info(), true);

    rc.setResponseExtension(re);
    return Response::RespOk;
}

Response::ResponseCode Server_ProtocolHandler::cmdListUsers(const Command_ListUsers & /*cmd*/, ResponseContainer &rc)
{
    if (authState == NotLoggedIn)
        return Response::RespLoginNeeded;

    Response_ListUsers *re = new Response_ListUsers;
    server->clientsLock.lockForRead();
    QMapIterator<QString, Server_ProtocolHandler *> userIterator = server->getUsers();
    while (userIterator.hasNext())
        re->add_user_list()->CopyFrom(userIterator.next().value()->copyUserInfo(false));
    QMapIterator<QString, Server_AbstractUserInterface *> extIterator = server->getExternalUsers();
    while (extIterator.hasNext())
        re->add_user_list()->CopyFrom(extIterator.next().value()->copyUserInfo(false));

    acceptsUserListChanges = true;
    server->clientsLock.unlock();

    rc.setResponseExtension(re);
    return Response::RespOk;
}

Response::ResponseCode
Server_ProtocolHandler::cmdLeaveRoom(const Command_LeaveRoom & /*cmd*/, Server_Room *room, ResponseContainer & /*rc*/)
{
    rooms.remove(room->getId());
    room->removeClient(this);
    return Response::RespOk;
}

bool Server_ProtocolHandler::addSaidMessageSize(int size)
{
    if (server->getMessageCountingInterval() <= 0) {
        return true;
    }

    int totalSize = 0, totalCount = 0;
    if (messageSizeOverTime.isEmpty()) {
        messageSizeOverTime.prepend(0);
    }

    messageSizeOverTime[0] += size;
    for (int messageSize : messageSizeOverTime) {
        totalSize += messageSize;
    }

    if (messageCountOverTime.isEmpty()) {
        messageCountOverTime.prepend(0);
    }

    messageCountOverTime[0] += 1;
    for (int messageCount : messageCountOverTime) {
        totalCount += messageCount;
    }

    return totalSize <= server->getMaxMessageSizePerInterval() && totalCount <= server->getMaxMessageCountPerInterval();
}

Response::ResponseCode
Server_ProtocolHandler::cmdRoomSay(const Command_RoomSay &cmd, Server_Room *room, ResponseContainer & /*rc*/)
{
    if (!addSaidMessageSize(static_cast<int>(cmd.message().size()))) {
        return Response::RespChatFlood;
    }
    QString msg = QString::fromStdString(cmd.message());

    msg.replace(QChar('\n'), QChar(' '));

    room->say(QString::fromStdString(userInfo->name()), msg);

    databaseInterface->logMessage(userInfo->id(), QString::fromStdString(userInfo->name()),
                                  QString::fromStdString(userInfo->address()), msg,
                                  Server_DatabaseInterface::MessageTargetRoom, room->getId(), room->getName());

    return Response::RespOk;
}

Response::ResponseCode
Server_ProtocolHandler::cmdCreateGame(const Command_CreateGame &cmd, Server_Room *room, ResponseContainer &rc)
{
    if (authState == NotLoggedIn)
        return Response::RespLoginNeeded;
    if (cmd.password().length() > MAX_NAME_LENGTH)
        return Response::RespContextError;

    auto level = userInfo->user_level();
    bool isJudge = level & ServerInfo_User::IsJudge;
    int maxGames = server->getMaxGamesPerUser();
    bool asJudge = cmd.join_as_judge();
    bool asSpectator = cmd.join_as_spectator();
    // allow judges to open games as spectator without limit to facilitate bots etc, -1 means no limit
    if (!(isJudge && asJudge && asSpectator) && maxGames >= 0 &&
        room->getGamesCreatedByUser(QString::fromStdString(userInfo->name())) >= maxGames) {
        return Response::RespContextError;
    }

    // if a non judge user tries to create a game as judge while not permitted, instead create a normal game
    if (asJudge && !(server->permitCreateGameAsJudge() || isJudge)) {
        asJudge = false;
    }

    QList<int> gameTypes;
    int gameTypeCount = qMin(cmd.game_type_ids().size(), MAX_NAME_LENGTH);
    for (int i = 0; i < gameTypeCount; ++i) { // the client actually only sends one of these
        gameTypes.append(cmd.game_type_ids(i));
    }

    QString description = nameFromStdString(cmd.description());
    int startingLifeTotal = cmd.has_starting_life_total() ? cmd.starting_life_total() : 20;

    const int gameId = databaseInterface->getNextGameId();
    if (gameId == -1) {
        return Response::RespInternalError;
    }

    // When server doesn't permit registered users to exist, do not honor only-reg setting
    bool onlyRegisteredUsers = cmd.only_registered() && (server->permitUnregisteredUsers());
    Server_Game *game = new Server_Game(
        copyUserInfo(false), gameId, description, QString::fromStdString(cmd.password()), cmd.max_players(), gameTypes,
        cmd.only_buddies(), onlyRegisteredUsers, cmd.spectators_allowed(), cmd.spectators_need_password(),
        cmd.spectators_can_talk(), cmd.spectators_see_everything(), startingLifeTotal, room);

    game->addPlayer(this, rc, asSpectator, asJudge, false);
    room->addGame(game);

    return Response::RespOk;
}

Response::ResponseCode
Server_ProtocolHandler::cmdJoinGame(const Command_JoinGame &cmd, Server_Room *room, ResponseContainer &rc)
{
    if (authState == NotLoggedIn)
        return Response::RespLoginNeeded;

    return room->processJoinGameCommand(cmd, rc, this);
}

void Server_ProtocolHandler::resetIdleTimer()
{
    lastActionReceived = timeRunning;
    idleClientWarningSent = false;
}
