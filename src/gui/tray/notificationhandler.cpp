#include "notificationhandler.h"

#include "accountstate.h"
#include "capabilities.h"
#include "networkjobs.h"

#include "iconjob.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace OCC {

Q_LOGGING_CATEGORY(lcServerNotification, "nextcloud.gui.servernotification", QtInfoMsg)

const QString notificationsPath = QLatin1String("ocs/v2.php/apps/notifications/api/v2/notifications");
const char propertyAccountStateC[] = "oc_account_state";
const int successStatusCode = 200;
const int notModifiedStatusCode = 304;

ServerNotificationHandler::ServerNotificationHandler(AccountState *accountState, QObject *parent)
    : QObject(parent)
    , _accountState(accountState)
{
}

void ServerNotificationHandler::slotFetchNotifications()
{
    // check connectivity and credentials
    if (!(_accountState && _accountState->isConnected() && _accountState->account() && _accountState->account()->credentials() && _accountState->account()->credentials()->ready())) {
        deleteLater();
        return;
    }
    // check if the account has notifications enabled. If the capabilities are
    // not yet valid, its assumed that notifications are available.
    if (_accountState->account()->capabilities().isValid()) {
        if (!_accountState->account()->capabilities().notificationsAvailable()) {
            qCInfo(lcServerNotification) << "Account" << _accountState->account()->displayName() << "does not have notifications enabled.";
            deleteLater();
            return;
        }
    }

    // if the previous notification job has finished, start next.
    _notificationJob = new JsonApiJob(_accountState->account(), notificationsPath, this);
    QObject::connect(_notificationJob.data(), &JsonApiJob::jsonReceived,
        this, &ServerNotificationHandler::slotNotificationsReceived);
    QObject::connect(_notificationJob.data(), &JsonApiJob::etagResponseHeaderReceived,
        this, &ServerNotificationHandler::slotEtagResponseHeaderReceived);
    QObject::connect(_notificationJob.data(), &JsonApiJob::allowDesktopNotificationsChanged,
            this, &ServerNotificationHandler::slotAllowDesktopNotificationsChanged);
    _notificationJob->setProperty(propertyAccountStateC, QVariant::fromValue<AccountState *>(_accountState));
    _notificationJob->addRawHeader("If-None-Match", _accountState->notificationsEtagResponseHeader());
    _notificationJob->start();
}

void ServerNotificationHandler::slotEtagResponseHeaderReceived(const QByteArray &value, int statusCode)
{
    if (statusCode == successStatusCode) {
        qCWarning(lcServerNotification) << "New Notification ETag Response Header received " << value;
        auto *account = qvariant_cast<AccountState *>(sender()->property(propertyAccountStateC));
        account->setNotificationsEtagResponseHeader(value);
    }
}

void ServerNotificationHandler::slotAllowDesktopNotificationsChanged(bool isAllowed)
{
    auto *account = qvariant_cast<AccountState *>(sender()->property(propertyAccountStateC));
    if (account != nullptr) {
       account->setDesktopNotificationsAllowed(isAllowed);
    }
}

void ServerNotificationHandler::slotNotificationsReceived(const QJsonDocument &json, int statusCode)
{
    if (statusCode != successStatusCode && statusCode != notModifiedStatusCode) {
        qCWarning(lcServerNotification) << "Notifications failed with status code " << statusCode;
        deleteLater();
        return;
    }

    if (statusCode == notModifiedStatusCode) {
        qCWarning(lcServerNotification) << "Status code " << statusCode << " Not Modified - No new notifications.";
        deleteLater();
        return;
    }

    auto notifies = json.object().value("ocs").toObject().value("data").toArray();

    auto *ai = qvariant_cast<AccountState *>(sender()->property(propertyAccountStateC));

    ActivityList list;

    foreach (auto element, notifies) {
        Activity a;
        auto json = element.toObject();
        a._type = Activity::NotificationType;
        a._accName = ai->account()->displayName();
        a._id = json.value("notification_id").toInt();

        //need to know, specially for remote_share
        a._objectType = json.value("object_type").toString();

        // 2 cases to consider:
        // - server == 24 & has Talk: notification type chat/call contains conversationToken/messageId in object_type
        // - server < 24 & has Talk: notification type chat/call contains _only_ the conversationToken in object_type
        if (a._objectType == "chat" || a._objectType == "call") {
            const auto objectId = json.value("object_id").toString();
            const auto objectIdData = objectId.split("/");
            a._talkNotificationData.conversationToken = objectIdData.first();
            if (a._objectType == "chat" && objectIdData.size() > 1) {
                a._talkNotificationData.messageId = objectIdData.last();
            } else {
                qCInfo(lcServerNotification) << "Replying directly to Talk conversation" << a._talkNotificationData.conversationToken << "will not be possible because the notification doesn't contain the message ID.";
            }
        } 

        a._status = 0;

        a._subject = json.value("subject").toString();
        a._message = json.value("message").toString();
        a._icon = json.value("icon").toString();

        QUrl link(json.value("link").toString());
        if (!link.isEmpty()) {
            if (link.host().isEmpty()) {
                link.setScheme(ai->account()->url().scheme());
                link.setHost(ai->account()->url().host());
            }
            if (link.port() == -1) {
                link.setPort(ai->account()->url().port());
            }
        }
        a._link = link;
        a._dateTime = QDateTime::fromString(json.value("datetime").toString(), Qt::ISODate);

        auto actions = json.value("actions").toArray();
        foreach (auto action, actions) {
            a._links.append(ActivityLink::createFomJsonObject(action.toObject()));
        }

        // Add another action to dismiss notification on server
        // https://github.com/owncloud/notifications/blob/master/docs/ocs-endpoint-v1.md#deleting-a-notification-for-a-user
        ActivityLink al;
        al._label = tr("Dismiss");
        al._link = Utility::concatUrlPath(ai->account()->url(), notificationsPath + "/" + QString::number(a._id)).toString();
        al._verb = "DELETE";
        al._primary = false;
        a._links.append(al);

        list.append(a);
    }
    emit newNotificationList(list);

    deleteLater();
}
}
