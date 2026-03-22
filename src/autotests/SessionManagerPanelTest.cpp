/*
    SPDX-FileCopyrightText: 2025 Struktured Labs

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Own
#include "SessionManagerPanelTest.h"

// Qt
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTreeWidget>

// Konsolai
#include "../claude/ClaudeSession.h"
#include "../claude/SessionManagerPanel.h"

using namespace Konsolai;

// Helper macro: construct SessionManagerPanel and process events so that
// the deferred init (QTimer::singleShot(0, ...)) fires before tests inspect state.
#define SessionManagerPanel_INIT(varName)                                                                                                                      \
    SessionManagerPanel varName;                                                                                                                               \
    QCoreApplication::processEvents()

static QString sessionsFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/sessions.json");
}

static void writeTestSessions(const QJsonArray &sessions)
{
    QString path = sessionsFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(sessions).toJson());
    file.close();
}

static QJsonObject makeSession(const QString &id, const QString &name, bool pinned = false, bool archived = false, bool expired = false)
{
    QJsonObject obj;
    obj[QStringLiteral("sessionId")] = id;
    obj[QStringLiteral("sessionName")] = name;
    obj[QStringLiteral("profileName")] = QStringLiteral("Test");
    obj[QStringLiteral("workingDirectory")] = QStringLiteral("/home/user/project");
    obj[QStringLiteral("isPinned")] = pinned;
    obj[QStringLiteral("isArchived")] = archived;
    obj[QStringLiteral("isExpired")] = expired;
    obj[QStringLiteral("lastAccessed")] = QStringLiteral("2025-06-01T12:00:00");
    obj[QStringLiteral("createdAt")] = QStringLiteral("2025-06-01T10:00:00");
    return obj;
}

void SessionManagerPanelTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void SessionManagerPanelTest::cleanupTestCase()
{
}

static QString foldersFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/session_folders.json");
}

void SessionManagerPanelTest::cleanup()
{
    QFile::remove(sessionsFilePath());
    QFile::remove(foldersFilePath());
}

// ============================================================
// Metadata filtering
// ============================================================

void SessionManagerPanelTest::testAllSessionsEmpty()
{
    SessionManagerPanel_INIT(panel);
    // With no sessions file, should have no sessions
    QCOMPARE(panel.allSessions().size(), 0);
}

void SessionManagerPanelTest::testAllSessionsLoaded()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("aaa11111"), QStringLiteral("konsolai-test-aaa11111")));
    sessions.append(makeSession(QStringLiteral("bbb22222"), QStringLiteral("konsolai-test-bbb22222")));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.allSessions().size(), 2);
}

void SessionManagerPanelTest::testPinnedSessionsFilter()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("pin11111"), QStringLiteral("konsolai-test-pin11111"), true));
    sessions.append(makeSession(QStringLiteral("nop22222"), QStringLiteral("konsolai-test-nop22222"), false));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QList<SessionMetadata> pinned = panel.pinnedSessions();
    QCOMPARE(pinned.size(), 1);
    QCOMPARE(pinned[0].sessionId, QStringLiteral("pin11111"));
}

void SessionManagerPanelTest::testArchivedSessionsFilter()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("arc11111"), QStringLiteral("konsolai-test-arc11111"), false, true));
    sessions.append(makeSession(QStringLiteral("act22222"), QStringLiteral("konsolai-test-act22222"), false, false));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QList<SessionMetadata> archived = panel.archivedSessions();
    QCOMPARE(archived.size(), 1);
    QCOMPARE(archived[0].sessionId, QStringLiteral("arc11111"));
}

void SessionManagerPanelTest::testPinnedExcludesArchived()
{
    // A session that is both pinned and archived should NOT appear in pinnedSessions
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("both1111"), QStringLiteral("konsolai-test-both1111"), true, true));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.pinnedSessions().size(), 0);
    QCOMPARE(panel.archivedSessions().size(), 1);
}

// ============================================================
// Pin/Unpin
// ============================================================

void SessionManagerPanelTest::testPinSession()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("pin11111"), QStringLiteral("konsolai-test-pin11111"), false));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.pinnedSessions().size(), 0);

    panel.pinSession(QStringLiteral("pin11111"));
    QCOMPARE(panel.pinnedSessions().size(), 1);
}

void SessionManagerPanelTest::testUnpinSession()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("unp11111"), QStringLiteral("konsolai-test-unp11111"), true));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.pinnedSessions().size(), 1);

    panel.unpinSession(QStringLiteral("unp11111"));
    QCOMPARE(panel.pinnedSessions().size(), 0);
}

void SessionManagerPanelTest::testPinNonexistentSession()
{
    SessionManagerPanel_INIT(panel);
    // Should be a no-op, not crash
    panel.pinSession(QStringLiteral("nonexistent"));
    QCOMPARE(panel.pinnedSessions().size(), 0);
}

// ============================================================
// Archive
// ============================================================

void SessionManagerPanelTest::testArchiveSession()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("arc11111"), QStringLiteral("konsolai-test-arc11111"), false, false));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.archivedSessions().size(), 0);

    panel.archiveSession(QStringLiteral("arc11111"));
    QCOMPARE(panel.archivedSessions().size(), 1);
    QVERIFY(panel.archivedSessions()[0].isArchived);
}

void SessionManagerPanelTest::testArchiveNonexistentSession()
{
    SessionManagerPanel_INIT(panel);
    // Should be a no-op, not crash
    panel.archiveSession(QStringLiteral("nonexistent"));
    QCOMPARE(panel.archivedSessions().size(), 0);
}

// ============================================================
// Close (new feature)
// ============================================================

void SessionManagerPanelTest::testCloseSessionNotArchived()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("cls11111"), QStringLiteral("konsolai-test-cls11111"), false, false));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    panel.closeSession(QStringLiteral("cls11111"));

    // Should NOT be archived
    QCOMPARE(panel.archivedSessions().size(), 0);

    // Should still be in allSessions
    QCOMPARE(panel.allSessions().size(), 1);

    // Verify lastAccessed was updated (should be recent)
    QList<SessionMetadata> all = panel.allSessions();
    QVERIFY(all[0].lastAccessed.secsTo(QDateTime::currentDateTime()) < 5);
}

void SessionManagerPanelTest::testCloseNonexistentSession()
{
    SessionManagerPanel_INIT(panel);
    // Should be a no-op, not crash
    panel.closeSession(QStringLiteral("nonexistent"));
    QCOMPARE(panel.allSessions().size(), 0);
}

// ============================================================
// Mark expired
// ============================================================

void SessionManagerPanelTest::testMarkExpired()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("exp11111"), QStringLiteral("konsolai-test-exp11111"), false, false, false));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    panel.markExpired(QStringLiteral("konsolai-test-exp11111"));

    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 1);
    QVERIFY(all[0].isExpired);
    QVERIFY(!all[0].isArchived); // expired != archived
}

void SessionManagerPanelTest::testMarkExpiredUnknownSession()
{
    SessionManagerPanel_INIT(panel);
    // Should be a no-op, not crash
    panel.markExpired(QStringLiteral("konsolai-nonexistent-12345678"));
}

// ============================================================
// Collapsed state
// ============================================================

void SessionManagerPanelTest::testCollapsedToggle()
{
    SessionManagerPanel_INIT(panel);
    QVERIFY(!panel.isCollapsed());

    panel.setCollapsed(true);
    QVERIFY(panel.isCollapsed());

    panel.setCollapsed(false);
    QVERIFY(!panel.isCollapsed());
}

void SessionManagerPanelTest::testCollapsedSignal()
{
    SessionManagerPanel_INIT(panel);
    QSignalSpy spy(&panel, &SessionManagerPanel::collapsedChanged);

    panel.setCollapsed(true);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), true);

    panel.setCollapsed(false);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
}

void SessionManagerPanelTest::testCollapsedIdempotent()
{
    SessionManagerPanel_INIT(panel);
    QSignalSpy spy(&panel, &SessionManagerPanel::collapsedChanged);

    panel.setCollapsed(false); // Already not collapsed
    QCOMPARE(spy.count(), 0); // No signal emitted
}

// ============================================================
// Metadata persistence round-trip
// ============================================================

void SessionManagerPanelTest::testMetadataPersistence()
{
    // Write, modify, save, re-read
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("per11111"), QStringLiteral("konsolai-test-per11111"), false));
    writeTestSessions(sessions);

    {
        SessionManagerPanel_INIT(panel);
        panel.pinSession(QStringLiteral("per11111"));
        // Destructor calls saveMetadata()
    }

    // New panel should load the saved state
    SessionManagerPanel_INIT(panel2);
    QCOMPARE(panel2.pinnedSessions().size(), 1);
    QCOMPARE(panel2.pinnedSessions()[0].sessionId, QStringLiteral("per11111"));
}

void SessionManagerPanelTest::testMetadataYoloPersistence()
{
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("yol11111"), QStringLiteral("konsolai-test-yol11111"));
    s[QStringLiteral("yoloMode")] = true;
    s[QStringLiteral("doubleYoloMode")] = true;
    s[QStringLiteral("tripleYoloMode")] = false;
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 1);
    QVERIFY(all[0].yoloMode);
    QVERIFY(all[0].doubleYoloMode);
    QVERIFY(!all[0].tripleYoloMode);
}

void SessionManagerPanelTest::testMetadataSshFields()
{
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("ssh11111"), QStringLiteral("konsolai-test-ssh11111"));
    s[QStringLiteral("isRemote")] = true;
    s[QStringLiteral("sshHost")] = QStringLiteral("dev.example.com");
    s[QStringLiteral("sshUsername")] = QStringLiteral("user");
    s[QStringLiteral("sshPort")] = 2222;
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 1);
    QVERIFY(all[0].isRemote);
    QCOMPARE(all[0].sshHost, QStringLiteral("dev.example.com"));
    QCOMPARE(all[0].sshUsername, QStringLiteral("user"));
    QCOMPARE(all[0].sshPort, 2222);
}

// ============================================================
// Dismiss / Restore / Purge lifecycle
// ============================================================

void SessionManagerPanelTest::testDismissSession()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("dis11111"), QStringLiteral("konsolai-test-dis11111"), false, true));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.archivedSessions().size(), 1);

    panel.dismissSession(QStringLiteral("dis11111"));

    // Still isArchived=true, but now also isDismissed=true
    // archivedSessions() returns all isArchived regardless of isDismissed
    QCOMPARE(panel.archivedSessions().size(), 1);
    QCOMPARE(panel.allSessions().size(), 1);
    QVERIFY(panel.allSessions()[0].isDismissed);
    QVERIFY(panel.allSessions()[0].isArchived);
}

void SessionManagerPanelTest::testDismissNonexistentSession()
{
    SessionManagerPanel_INIT(panel);
    panel.dismissSession(QStringLiteral("nonexistent"));
    QCOMPARE(panel.allSessions().size(), 0);
}

void SessionManagerPanelTest::testRestoreSession()
{
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("rst11111"), QStringLiteral("konsolai-test-rst11111"), false, true);
    s[QStringLiteral("isDismissed")] = true;
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    // archivedSessions() includes dismissed (isArchived is still true)
    QCOMPARE(panel.archivedSessions().size(), 1);
    QVERIFY(panel.allSessions()[0].isDismissed);

    panel.restoreSession(QStringLiteral("rst11111"));

    // Should no longer be dismissed, still archived
    QCOMPARE(panel.archivedSessions().size(), 1);
    QVERIFY(!panel.archivedSessions()[0].isDismissed);
    QVERIFY(panel.archivedSessions()[0].isArchived);
}

void SessionManagerPanelTest::testRestoreNonexistentSession()
{
    SessionManagerPanel_INIT(panel);
    panel.restoreSession(QStringLiteral("nonexistent"));
    QCOMPARE(panel.allSessions().size(), 0);
}

void SessionManagerPanelTest::testPurgeSession()
{
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("prg11111"), QStringLiteral("konsolai-test-prg11111"), false, true);
    s[QStringLiteral("isDismissed")] = true;
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.allSessions().size(), 1);

    panel.purgeSession(QStringLiteral("prg11111"));
    QCOMPARE(panel.allSessions().size(), 0);
}

void SessionManagerPanelTest::testPurgeNonexistentSession()
{
    SessionManagerPanel_INIT(panel);
    panel.purgeSession(QStringLiteral("nonexistent"));
    QCOMPARE(panel.allSessions().size(), 0);
}

void SessionManagerPanelTest::testPurgeDismissed()
{
    QJsonArray sessions;
    QJsonObject s1 = makeSession(QStringLiteral("pd111111"), QStringLiteral("konsolai-test-pd111111"), false, true);
    s1[QStringLiteral("isDismissed")] = true;
    QJsonObject s2 = makeSession(QStringLiteral("pd222222"), QStringLiteral("konsolai-test-pd222222"), false, true);
    s2[QStringLiteral("isDismissed")] = true;
    QJsonObject s3 = makeSession(QStringLiteral("pd333333"), QStringLiteral("konsolai-test-pd333333"), false, true);
    // s3 is archived but NOT dismissed
    sessions.append(s1);
    sessions.append(s2);
    sessions.append(s3);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.allSessions().size(), 3);

    panel.purgeDismissed();

    // Only the non-dismissed one should remain
    QCOMPARE(panel.allSessions().size(), 1);
    QCOMPARE(panel.allSessions()[0].sessionId, QStringLiteral("pd333333"));
}

void SessionManagerPanelTest::testDismissRestorePurgeRoundTrip()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("rnd11111"), QStringLiteral("konsolai-test-rnd11111"), false, true));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);

    // Start: archived
    QCOMPARE(panel.archivedSessions().size(), 1);

    // Dismiss — still in archivedSessions() since isArchived stays true
    panel.dismissSession(QStringLiteral("rnd11111"));
    QCOMPARE(panel.archivedSessions().size(), 1);
    QVERIFY(panel.allSessions()[0].isDismissed);

    // Restore
    panel.restoreSession(QStringLiteral("rnd11111"));
    QCOMPARE(panel.archivedSessions().size(), 1);
    QVERIFY(!panel.allSessions()[0].isDismissed);

    // Dismiss again and purge
    panel.dismissSession(QStringLiteral("rnd11111"));
    panel.purgeSession(QStringLiteral("rnd11111"));
    QCOMPARE(panel.allSessions().size(), 0);
}

// ============================================================
// Additional metadata persistence tests
// ============================================================

void SessionManagerPanelTest::testMetadataBudgetPersistence()
{
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("bgt11111"), QStringLiteral("konsolai-test-bgt11111"));
    s[QStringLiteral("budgetTimeLimitMinutes")] = 60;
    s[QStringLiteral("budgetCostCeilingUSD")] = 5.50;
    s[QStringLiteral("budgetTokenCeiling")] = 100000;
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all[0].budgetTimeLimitMinutes, 60);
    QCOMPARE(all[0].budgetCostCeilingUSD, 5.50);
    QCOMPARE(all[0].budgetTokenCeiling, static_cast<quint64>(100000));
}

void SessionManagerPanelTest::testMetadataCorruptedJson()
{
    // Write garbage to the sessions file
    QString path = sessionsFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("this is not valid json {{{");
    file.close();

    // Panel should handle gracefully — no sessions, no crash
    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.allSessions().size(), 0);
}

void SessionManagerPanelTest::testMetadataMissingFields()
{
    // Session with minimal fields (missing many optional ones)
    QJsonArray sessions;
    QJsonObject s;
    s[QStringLiteral("sessionId")] = QStringLiteral("min11111");
    s[QStringLiteral("sessionName")] = QStringLiteral("konsolai-test-min11111");
    // No workingDirectory, no isPinned, no isArchived, etc.
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all[0].sessionId, QStringLiteral("min11111"));
    // Defaults should be applied
    QVERIFY(!all[0].isPinned);
    QVERIFY(!all[0].isArchived);
    QVERIFY(!all[0].isDismissed);
    QVERIFY(!all[0].isRemote);
    QVERIFY(!all[0].yoloMode);
    QCOMPARE(all[0].budgetTimeLimitMinutes, 0);
    QCOMPARE(all[0].budgetCostCeilingUSD, 0.0);
    QCOMPARE(all[0].budgetTokenCeiling, static_cast<quint64>(0));
}

void SessionManagerPanelTest::testMetadataApprovalCountPersistence()
{
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("apv11111"), QStringLiteral("konsolai-test-apv11111"));
    s[QStringLiteral("yoloApprovalCount")] = 42;
    s[QStringLiteral("doubleYoloApprovalCount")] = 7;
    s[QStringLiteral("tripleYoloApprovalCount")] = 3;
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all[0].yoloApprovalCount, 42);
    QCOMPARE(all[0].doubleYoloApprovalCount, 7);
    QCOMPARE(all[0].tripleYoloApprovalCount, 3);
}

// ============================================================
// Full round-trip with ALL fields
// ============================================================

void SessionManagerPanelTest::testMetadataAllFieldsRoundTrip()
{
    // Create a session with EVERY field populated
    QJsonArray sessions;
    QJsonObject s;
    s[QStringLiteral("sessionId")] = QStringLiteral("all11111");
    s[QStringLiteral("sessionName")] = QStringLiteral("konsolai-test-all11111");
    s[QStringLiteral("profileName")] = QStringLiteral("FullProfile");
    s[QStringLiteral("workingDirectory")] = QStringLiteral("/home/user/big-project");
    s[QStringLiteral("isPinned")] = true;
    s[QStringLiteral("isArchived")] = false;
    s[QStringLiteral("isExpired")] = false;
    s[QStringLiteral("isDismissed")] = false;
    s[QStringLiteral("lastAccessed")] = QStringLiteral("2025-12-25T23:59:59");
    s[QStringLiteral("createdAt")] = QStringLiteral("2025-01-01T00:00:00");
    // SSH fields
    s[QStringLiteral("isRemote")] = true;
    s[QStringLiteral("sshHost")] = QStringLiteral("prod.example.com");
    s[QStringLiteral("sshUsername")] = QStringLiteral("deployer");
    s[QStringLiteral("sshPort")] = 2222;
    // Yolo fields
    s[QStringLiteral("yoloMode")] = true;
    s[QStringLiteral("doubleYoloMode")] = true;
    s[QStringLiteral("tripleYoloMode")] = true;
    // Approval counts
    s[QStringLiteral("yoloApprovalCount")] = 100;
    s[QStringLiteral("doubleYoloApprovalCount")] = 50;
    s[QStringLiteral("tripleYoloApprovalCount")] = 25;
    // Budget fields
    s[QStringLiteral("budgetTimeLimitMinutes")] = 120;
    s[QStringLiteral("budgetCostCeilingUSD")] = 10.99;
    s[QStringLiteral("budgetTokenCeiling")] = 500000;
    sessions.append(s);
    writeTestSessions(sessions);

    // Load, modify, save, and reload
    {
        SessionManagerPanel_INIT(panel);
        QList<SessionMetadata> all = panel.allSessions();
        QCOMPARE(all.size(), 1);

        const auto &m = all[0];
        // Verify all fields loaded correctly
        QCOMPARE(m.sessionId, QStringLiteral("all11111"));
        QCOMPARE(m.sessionName, QStringLiteral("konsolai-test-all11111"));
        QCOMPARE(m.profileName, QStringLiteral("FullProfile"));
        QCOMPARE(m.workingDirectory, QStringLiteral("/home/user/big-project"));
        QVERIFY(m.isPinned);
        QVERIFY(!m.isArchived);
        QVERIFY(!m.isExpired);
        QVERIFY(!m.isDismissed);
        QVERIFY(m.isRemote);
        QCOMPARE(m.sshHost, QStringLiteral("prod.example.com"));
        QCOMPARE(m.sshUsername, QStringLiteral("deployer"));
        QCOMPARE(m.sshPort, 2222);
        QVERIFY(m.yoloMode);
        QVERIFY(m.doubleYoloMode);
        QVERIFY(m.tripleYoloMode);
        QCOMPARE(m.yoloApprovalCount, 100);
        QCOMPARE(m.doubleYoloApprovalCount, 50);
        QCOMPARE(m.tripleYoloApprovalCount, 25);
        QCOMPARE(m.budgetTimeLimitMinutes, 120);
        QCOMPARE(m.budgetCostCeilingUSD, 10.99);
        QCOMPARE(m.budgetTokenCeiling, static_cast<quint64>(500000));

        // Panel destructor saves metadata
    }

    // Reload and verify persistence
    {
        SessionManagerPanel_INIT(panel2);
        QList<SessionMetadata> all = panel2.allSessions();
        QCOMPARE(all.size(), 1);

        const auto &m = all[0];
        QCOMPARE(m.sessionId, QStringLiteral("all11111"));
        QVERIFY(m.isPinned);
        QVERIFY(m.isRemote);
        QCOMPARE(m.sshHost, QStringLiteral("prod.example.com"));
        QCOMPARE(m.sshPort, 2222);
        QVERIFY(m.yoloMode);
        QVERIFY(m.doubleYoloMode);
        QVERIFY(m.tripleYoloMode);
        QCOMPARE(m.yoloApprovalCount, 100);
        QCOMPARE(m.doubleYoloApprovalCount, 50);
        QCOMPARE(m.tripleYoloApprovalCount, 25);
        QCOMPARE(m.budgetTimeLimitMinutes, 120);
        QCOMPARE(m.budgetCostCeilingUSD, 10.99);
        QCOMPARE(m.budgetTokenCeiling, static_cast<quint64>(500000));
    }
}

void SessionManagerPanelTest::testMetadataApprovalLogRoundTrip()
{
    // Create session with approval log entries
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("log11111"), QStringLiteral("konsolai-test-log11111"));

    QJsonArray logEntries;
    {
        QJsonObject entry;
        entry[QStringLiteral("time")] = QStringLiteral("2025-06-15T10:00:00");
        entry[QStringLiteral("tool")] = QStringLiteral("Bash");
        entry[QStringLiteral("action")] = QStringLiteral("auto-approved");
        entry[QStringLiteral("level")] = 1;
        entry[QStringLiteral("tokens")] = 5000;
        entry[QStringLiteral("cost")] = 0.05;
        logEntries.append(entry);
    }
    {
        QJsonObject entry;
        entry[QStringLiteral("time")] = QStringLiteral("2025-06-15T10:01:00");
        entry[QStringLiteral("tool")] = QStringLiteral("suggestion");
        entry[QStringLiteral("action")] = QStringLiteral("auto-accepted");
        entry[QStringLiteral("level")] = 2;
        entry[QStringLiteral("tokens")] = 10000;
        entry[QStringLiteral("cost")] = 0.10;
        logEntries.append(entry);
    }

    s[QStringLiteral("approvalLog")] = logEntries;
    s[QStringLiteral("yoloApprovalCount")] = 1;
    s[QStringLiteral("doubleYoloApprovalCount")] = 1;
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all[0].yoloApprovalCount, 1);
    QCOMPARE(all[0].doubleYoloApprovalCount, 1);
    QCOMPARE(all[0].approvalLog.size(), 2);

    // Verify first entry
    const auto &e1 = all[0].approvalLog[0];
    QCOMPARE(e1.toolName, QStringLiteral("Bash"));
    QCOMPARE(e1.action, QStringLiteral("auto-approved"));
    QCOMPARE(e1.yoloLevel, 1);
    QCOMPARE(e1.totalTokens, quint64(5000));
    QCOMPARE(e1.estimatedCostUSD, 0.05);

    // Verify second entry
    const auto &e2 = all[0].approvalLog[1];
    QCOMPARE(e2.toolName, QStringLiteral("suggestion"));
    QCOMPARE(e2.yoloLevel, 2);
}

void SessionManagerPanelTest::testMetadataMultipleSessionsRoundTrip()
{
    // Test that multiple sessions with different field combinations all persist correctly
    QJsonArray sessions;

    // Session 1: pinned, local, no yolo
    QJsonObject s1 = makeSession(QStringLiteral("mul11111"), QStringLiteral("konsolai-test-mul11111"), true, false);

    // Session 2: archived, remote, yolo enabled
    QJsonObject s2 = makeSession(QStringLiteral("mul22222"), QStringLiteral("konsolai-test-mul22222"), false, true);
    s2[QStringLiteral("isRemote")] = true;
    s2[QStringLiteral("sshHost")] = QStringLiteral("dev.example.com");
    s2[QStringLiteral("yoloMode")] = true;

    // Session 3: dismissed, with budget
    QJsonObject s3 = makeSession(QStringLiteral("mul33333"), QStringLiteral("konsolai-test-mul33333"), false, true);
    s3[QStringLiteral("isDismissed")] = true;
    s3[QStringLiteral("budgetTimeLimitMinutes")] = 30;
    s3[QStringLiteral("budgetCostCeilingUSD")] = 2.50;

    sessions.append(s1);
    sessions.append(s2);
    sessions.append(s3);
    writeTestSessions(sessions);

    {
        SessionManagerPanel_INIT(panel);
        QList<SessionMetadata> all = panel.allSessions();
        QCOMPARE(all.size(), 3);
        // Panel destructor saves
    }

    // Reload
    SessionManagerPanel_INIT(panel2);
    QList<SessionMetadata> all = panel2.allSessions();
    QCOMPARE(all.size(), 3);

    // Find each session and verify
    SessionMetadata *m1 = nullptr;
    SessionMetadata *m2 = nullptr;
    SessionMetadata *m3 = nullptr;
    for (auto &m : all) {
        if (m.sessionId == QStringLiteral("mul11111"))
            m1 = &m;
        else if (m.sessionId == QStringLiteral("mul22222"))
            m2 = &m;
        else if (m.sessionId == QStringLiteral("mul33333"))
            m3 = &m;
    }

    QVERIFY(m1);
    QVERIFY(m1->isPinned);
    QVERIFY(!m1->isRemote);

    QVERIFY(m2);
    QVERIFY(m2->isArchived);
    QVERIFY(m2->isRemote);
    QCOMPARE(m2->sshHost, QStringLiteral("dev.example.com"));
    QVERIFY(m2->yoloMode);

    QVERIFY(m3);
    QVERIFY(m3->isDismissed);
    QCOMPARE(m3->budgetTimeLimitMinutes, 30);
    QCOMPARE(m3->budgetCostCeilingUSD, 2.50);
}

void SessionManagerPanelTest::testMetadataSaveLoadIdempotent()
{
    // Save, load, save again — result should be identical
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("idem1111"), QStringLiteral("konsolai-test-idem1111"), true, false);
    s[QStringLiteral("yoloMode")] = true;
    s[QStringLiteral("budgetTimeLimitMinutes")] = 45;
    sessions.append(s);
    writeTestSessions(sessions);

    // First load & save
    {
        SessionManagerPanel_INIT(panel);
        QCOMPARE(panel.allSessions().size(), 1);
    }

    // Read file after first save
    QFile file1(sessionsFilePath());
    QVERIFY(file1.open(QIODevice::ReadOnly));
    QByteArray data1 = file1.readAll();
    file1.close();

    // Second load & save
    {
        SessionManagerPanel_INIT(panel);
        QCOMPARE(panel.allSessions().size(), 1);
    }

    // Read file after second save
    QFile file2(sessionsFilePath());
    QVERIFY(file2.open(QIODevice::ReadOnly));
    QByteArray data2 = file2.readAll();
    file2.close();

    // Parse both and compare session count and key fields (exact byte comparison
    // may differ due to JSON key ordering or whitespace)
    QJsonDocument doc1 = QJsonDocument::fromJson(data1);
    QJsonDocument doc2 = QJsonDocument::fromJson(data2);
    QVERIFY(!doc1.isNull());
    QVERIFY(!doc2.isNull());

    QJsonArray arr1 = doc1.array();
    QJsonArray arr2 = doc2.array();
    QCOMPARE(arr1.size(), arr2.size());
    QCOMPARE(arr1.size(), 1);

    QJsonObject o1 = arr1[0].toObject();
    QJsonObject o2 = arr2[0].toObject();
    QCOMPARE(o1.value(QStringLiteral("sessionId")).toString(), o2.value(QStringLiteral("sessionId")).toString());
    QCOMPARE(o1.value(QStringLiteral("isPinned")).toBool(), o2.value(QStringLiteral("isPinned")).toBool());
    QCOMPARE(o1.value(QStringLiteral("yoloMode")).toBool(), o2.value(QStringLiteral("yoloMode")).toBool());
    QCOMPARE(o1.value(QStringLiteral("budgetTimeLimitMinutes")).toInt(), o2.value(QStringLiteral("budgetTimeLimitMinutes")).toInt());
}

// ============================================================
// Subagent/subprocess metadata persistence
// ============================================================

void SessionManagerPanelTest::testMetadataSubagentPersistence()
{
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("sag11111"), QStringLiteral("konsolai-test-sag11111"));

    // Add subagents array
    QJsonArray agentArray;
    {
        QJsonObject a;
        a[QStringLiteral("agentId")] = QStringLiteral("agent-abc");
        a[QStringLiteral("agentType")] = QStringLiteral("Explore");
        a[QStringLiteral("teammateName")] = QStringLiteral("researcher");
        a[QStringLiteral("state")] = 3; // NotRunning
        a[QStringLiteral("taskDescription")] = QStringLiteral("Find auth bugs");
        a[QStringLiteral("currentTaskSubject")] = QStringLiteral("Fix login");
        a[QStringLiteral("promptGroupId")] = 1;
        agentArray.append(a);
    }
    {
        QJsonObject a;
        a[QStringLiteral("agentId")] = QStringLiteral("agent-def");
        a[QStringLiteral("agentType")] = QStringLiteral("Bash");
        a[QStringLiteral("state")] = 3;
        a[QStringLiteral("promptGroupId")] = 2;
        agentArray.append(a);
    }
    s[QStringLiteral("subagents")] = agentArray;
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all[0].subagents.size(), 2);
    QCOMPARE(all[0].subagents[0].agentId, QStringLiteral("agent-abc"));
    QCOMPARE(all[0].subagents[0].agentType, QStringLiteral("Explore"));
    QCOMPARE(all[0].subagents[0].teammateName, QStringLiteral("researcher"));
    QCOMPARE(all[0].subagents[0].taskDescription, QStringLiteral("Find auth bugs"));
    QCOMPARE(all[0].subagents[0].currentTaskSubject, QStringLiteral("Fix login"));
    QCOMPARE(all[0].subagents[0].promptGroupId, 1);
    QCOMPARE(all[0].subagents[1].agentId, QStringLiteral("agent-def"));
    QCOMPARE(all[0].subagents[1].agentType, QStringLiteral("Bash"));
    QCOMPARE(all[0].subagents[1].promptGroupId, 2);
}

void SessionManagerPanelTest::testMetadataSubprocessPersistence()
{
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("spr11111"), QStringLiteral("konsolai-test-spr11111"));

    QJsonArray procArray;
    {
        QJsonObject p;
        p[QStringLiteral("id")] = QStringLiteral("proc-001");
        p[QStringLiteral("command")] = QStringLiteral("ninja -j4");
        p[QStringLiteral("fullCommand")] = QStringLiteral("ninja -j4 -C /build");
        p[QStringLiteral("status")] = 1; // Completed
        p[QStringLiteral("exitCode")] = 0;
        p[QStringLiteral("pid")] = 12345.0;
        p[QStringLiteral("promptGroupId")] = 1;
        p[QStringLiteral("isBackground")] = true;
        QJsonObject res;
        res[QStringLiteral("cpu")] = 50.5;
        res[QStringLiteral("rss")] = 2097152.0;
        p[QStringLiteral("resourceUsage")] = res;
        procArray.append(p);
    }
    s[QStringLiteral("subprocesses")] = procArray;
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all[0].subprocesses.size(), 1);

    const auto &proc = all[0].subprocesses[0];
    QCOMPARE(proc.id, QStringLiteral("proc-001"));
    QCOMPARE(proc.command, QStringLiteral("ninja -j4"));
    QCOMPARE(proc.fullCommand, QStringLiteral("ninja -j4 -C /build"));
    QCOMPARE(static_cast<int>(proc.status), static_cast<int>(SubprocessInfo::Completed));
    QCOMPARE(proc.exitCode, 0);
    QCOMPARE(proc.pid, qint64(12345));
    QCOMPARE(proc.promptGroupId, 1);
    QVERIFY(proc.isBackground);
    QCOMPARE(proc.resourceUsage.cpuPercent, 50.5);
    QCOMPARE(proc.resourceUsage.rssBytes, quint64(2097152));
}

void SessionManagerPanelTest::testMetadataPromptLabelsPersistence()
{
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("plb11111"), QStringLiteral("konsolai-test-plb11111"));

    QJsonObject labels;
    labels[QStringLiteral("0")] = QStringLiteral("Fix the bug");
    labels[QStringLiteral("1")] = QStringLiteral("Add tests");
    labels[QStringLiteral("2")] = QStringLiteral("Refactor auth");
    s[QStringLiteral("promptLabels")] = labels;
    s[QStringLiteral("promptRound")] = 2;
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all[0].promptGroupLabels.size(), 3);
    QCOMPARE(all[0].promptGroupLabels[0], QStringLiteral("Fix the bug"));
    QCOMPARE(all[0].promptGroupLabels[1], QStringLiteral("Add tests"));
    QCOMPARE(all[0].promptGroupLabels[2], QStringLiteral("Refactor auth"));
    QCOMPARE(all[0].currentPromptRound, 2);
}

void SessionManagerPanelTest::testMetadataSubagentEmptyNotSerialized()
{
    // Session with no subagents/subprocesses — JSON should not contain those keys
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("emp11111"), QStringLiteral("konsolai-test-emp11111")));
    writeTestSessions(sessions);

    {
        SessionManagerPanel_INIT(panel);
        QList<SessionMetadata> all = panel.allSessions();
        QCOMPARE(all.size(), 1);
        QVERIFY(all[0].subagents.isEmpty());
        QVERIFY(all[0].subprocesses.isEmpty());
        QVERIFY(all[0].promptGroupLabels.isEmpty());
        QCOMPARE(all[0].currentPromptRound, 0);
        // Panel destructor saves
    }

    // Read raw JSON and verify no subagent/subprocess keys
    QFile file(sessionsFilePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    QJsonObject obj = doc.array()[0].toObject();
    QVERIFY(!obj.contains(QStringLiteral("subagents")));
    QVERIFY(!obj.contains(QStringLiteral("subprocesses")));
    QVERIFY(!obj.contains(QStringLiteral("promptLabels")));
    QVERIFY(!obj.contains(QStringLiteral("promptRound")));
}

void SessionManagerPanelTest::testMetadataSubagentRoundTrip()
{
    // Write session with subagents → save → reload → verify
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("art11111"), QStringLiteral("konsolai-test-art11111"));

    QJsonArray agentArray;
    QJsonObject a;
    a[QStringLiteral("agentId")] = QStringLiteral("rt-agent");
    a[QStringLiteral("agentType")] = QStringLiteral("general-purpose");
    a[QStringLiteral("teammateName")] = QStringLiteral("developer");
    a[QStringLiteral("state")] = 3;
    a[QStringLiteral("promptGroupId")] = 1;
    agentArray.append(a);
    s[QStringLiteral("subagents")] = agentArray;

    QJsonObject labels;
    labels[QStringLiteral("0")] = QStringLiteral("Initial prompt");
    labels[QStringLiteral("1")] = QStringLiteral("Follow up");
    s[QStringLiteral("promptLabels")] = labels;
    s[QStringLiteral("promptRound")] = 1;

    sessions.append(s);
    writeTestSessions(sessions);

    // First load & save
    {
        SessionManagerPanel_INIT(panel);
        QCOMPARE(panel.allSessions().size(), 1);
        QCOMPARE(panel.allSessions()[0].subagents.size(), 1);
    }

    // Second load — verify persistence
    {
        SessionManagerPanel_INIT(panel2);
        QList<SessionMetadata> all = panel2.allSessions();
        QCOMPARE(all.size(), 1);
        QCOMPARE(all[0].subagents.size(), 1);
        QCOMPARE(all[0].subagents[0].agentId, QStringLiteral("rt-agent"));
        QCOMPARE(all[0].subagents[0].agentType, QStringLiteral("general-purpose"));
        QCOMPARE(all[0].subagents[0].teammateName, QStringLiteral("developer"));
        QCOMPARE(all[0].promptGroupLabels.size(), 2);
        QCOMPARE(all[0].promptGroupLabels[0], QStringLiteral("Initial prompt"));
        QCOMPARE(all[0].promptGroupLabels[1], QStringLiteral("Follow up"));
        QCOMPARE(all[0].currentPromptRound, 1);
    }
}

// ============================================================
// Remote session registration and restoration
// ============================================================

void SessionManagerPanelTest::testRegisterSessionCapturesRemoteFields()
{
    // registerSession() should capture isRemote/sshHost/sshUsername/sshPort
    // from the ClaudeSession object into metadata
    SessionManagerPanel_INIT(panel);

    auto *session = new ClaudeSession(QStringLiteral("Claude"), QStringLiteral("/home/struktured/projects/fluxit"), this);
    session->setIsRemote(true);
    session->setSshHost(QStringLiteral("blackmage.io"));
    session->setSshUsername(QStringLiteral("struktured"));
    session->setSshPort(22);

    panel.registerSession(session);

    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 1);
    QVERIFY(all[0].isRemote);
    QCOMPARE(all[0].sshHost, QStringLiteral("blackmage.io"));
    QCOMPARE(all[0].sshUsername, QStringLiteral("struktured"));
    QCOMPARE(all[0].sshPort, 22);

    delete session;
}

void SessionManagerPanelTest::testUnarchiveEmitsRemoteFields()
{
    // unarchiveSession() should emit signal with SSH metadata for remote sessions
    QJsonArray sessions;
    QJsonObject s = makeSession(QStringLiteral("una11111"), QStringLiteral("konsolai-test-una11111"));
    s[QStringLiteral("isRemote")] = true;
    s[QStringLiteral("sshHost")] = QStringLiteral("blackmage.io");
    s[QStringLiteral("sshUsername")] = QStringLiteral("struktured");
    s[QStringLiteral("sshPort")] = 2222;
    s[QStringLiteral("isArchived")] = false;
    sessions.append(s);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QSignalSpy spy(&panel, &SessionManagerPanel::unarchiveRequested);

    panel.unarchiveSession(QStringLiteral("una11111"));

    QCOMPARE(spy.count(), 1);
    QList<QVariant> args = spy.at(0);
    QCOMPARE(args.at(0).toString(), QStringLiteral("una11111")); // sessionId
    QCOMPARE(args.at(1).toString(), QStringLiteral("/home/user/project")); // workingDirectory
    QCOMPARE(args.at(2).toBool(), true); // isRemote
    QCOMPARE(args.at(3).toString(), QStringLiteral("blackmage.io")); // sshHost
    QCOMPARE(args.at(4).toString(), QStringLiteral("struktured")); // sshUsername
    QCOMPARE(args.at(5).toInt(), 2222); // sshPort
}

void SessionManagerPanelTest::testUnarchiveLocalSessionEmitsNoRemoteFields()
{
    // unarchiveSession() for a local session should emit isRemote=false
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("loc11111"), QStringLiteral("konsolai-test-loc11111")));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QSignalSpy spy(&panel, &SessionManagerPanel::unarchiveRequested);

    panel.unarchiveSession(QStringLiteral("loc11111"));

    QCOMPARE(spy.count(), 1);
    QList<QVariant> args = spy.at(0);
    QCOMPARE(args.at(2).toBool(), false); // isRemote
    QVERIFY(args.at(3).toString().isEmpty()); // sshHost empty
}

void SessionManagerPanelTest::testRegisterRemoteSessionRoundTrip()
{
    // Register a remote session → save → reload → verify remote fields persisted
    {
        SessionManagerPanel_INIT(panel);

        // Parent to test, not panel, to avoid SSH cleanup on panel destruct
        auto *session = new ClaudeSession(QStringLiteral("Claude"), QStringLiteral("/home/struktured/projects/fluxit"), this);
        session->setIsRemote(true);
        session->setSshHost(QStringLiteral("dev.example.com"));
        session->setSshUsername(QStringLiteral("deployer"));
        session->setSshPort(2222);

        panel.registerSession(session);

        // Verify in-memory
        QList<SessionMetadata> all = panel.allSessions();
        QCOMPARE(all.size(), 1);
        QVERIFY(all[0].isRemote);
        QCOMPARE(all[0].sshHost, QStringLiteral("dev.example.com"));

        // Delete session before panel to avoid SSH cleanup hanging
        delete session;

        // Panel destructor saves metadata
    }

    // Reload from disk
    SessionManagerPanel_INIT(panel2);
    QList<SessionMetadata> all = panel2.allSessions();
    QCOMPARE(all.size(), 1);
    QVERIFY(all[0].isRemote);
    QCOMPARE(all[0].sshHost, QStringLiteral("dev.example.com"));
    QCOMPARE(all[0].sshUsername, QStringLiteral("deployer"));
    QCOMPARE(all[0].sshPort, 2222);
    QCOMPARE(all[0].workingDirectory, QStringLiteral("/home/struktured/projects/fluxit"));
}

// ============================================================
// Bulk operations
// ============================================================

void SessionManagerPanelTest::testBulkArchiveMultipleSessions()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("bulk01"), QStringLiteral("konsolai-bulk01")));
    sessions.append(makeSession(QStringLiteral("bulk02"), QStringLiteral("konsolai-bulk02")));
    sessions.append(makeSession(QStringLiteral("bulk03"), QStringLiteral("konsolai-bulk03")));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.allSessions().size(), 3);
    QCOMPARE(panel.archivedSessions().size(), 0);

    // Archive all three
    panel.archiveSession(QStringLiteral("bulk01"));
    panel.archiveSession(QStringLiteral("bulk02"));
    panel.archiveSession(QStringLiteral("bulk03"));

    QCOMPARE(panel.archivedSessions().size(), 3);

    // Verify each is archived
    for (const auto &meta : panel.allSessions()) {
        QVERIFY(meta.isArchived);
    }
}

void SessionManagerPanelTest::testBulkDismissMultipleSessions()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("bdis01"), QStringLiteral("konsolai-bdis01"), false, true));
    sessions.append(makeSession(QStringLiteral("bdis02"), QStringLiteral("konsolai-bdis02"), false, true));
    sessions.append(makeSession(QStringLiteral("bdis03"), QStringLiteral("konsolai-bdis03"), false, true));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.archivedSessions().size(), 3);

    // Dismiss all three
    panel.dismissSession(QStringLiteral("bdis01"));
    panel.dismissSession(QStringLiteral("bdis02"));
    panel.dismissSession(QStringLiteral("bdis03"));

    // All still exist with isDismissed flag
    // (archivedSessions() still includes them since isArchived remains true)
    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 3);
    int dismissedCount = 0;
    for (const auto &meta : all) {
        if (meta.isDismissed) {
            dismissedCount++;
        }
    }
    QCOMPARE(dismissedCount, 3);
}

void SessionManagerPanelTest::testBulkDismissOlderThan()
{
    QJsonArray sessions;

    // Old session (2 months ago)
    QJsonObject oldSession = makeSession(QStringLiteral("age01"), QStringLiteral("konsolai-age01"), false, true);
    oldSession[QStringLiteral("lastAccessed")] = QDateTime::currentDateTime().addDays(-60).toString(Qt::ISODate);
    sessions.append(oldSession);

    // Recent session (yesterday)
    QJsonObject recentSession = makeSession(QStringLiteral("age02"), QStringLiteral("konsolai-age02"), false, true);
    recentSession[QStringLiteral("lastAccessed")] = QDateTime::currentDateTime().addDays(-1).toString(Qt::ISODate);
    sessions.append(recentSession);

    // Medium-age session (2 weeks ago)
    QJsonObject mediumSession = makeSession(QStringLiteral("age03"), QStringLiteral("konsolai-age03"), false, true);
    mediumSession[QStringLiteral("lastAccessed")] = QDateTime::currentDateTime().addDays(-14).toString(Qt::ISODate);
    sessions.append(mediumSession);

    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.archivedSessions().size(), 3);

    // Dismiss sessions older than 1 month — should only get age01
    QDateTime cutoff30 = QDateTime::currentDateTime().addDays(-30);
    int dismissedCount = 0;
    for (const auto &meta : panel.allSessions()) {
        if (meta.isArchived && !meta.isDismissed && meta.lastAccessed.isValid() && meta.lastAccessed < cutoff30) {
            panel.dismissSession(meta.sessionId);
            dismissedCount++;
        }
    }
    QCOMPARE(dismissedCount, 1);

    // Count non-dismissed archived sessions
    auto countActive = [&panel]() {
        int count = 0;
        for (const auto &m : panel.allSessions()) {
            if (m.isArchived && !m.isDismissed) count++;
        }
        return count;
    };
    QCOMPARE(countActive(), 2);

    // Dismiss sessions older than 1 week — should get age03 (14 days old)
    QDateTime cutoff7 = QDateTime::currentDateTime().addDays(-7);
    for (const auto &meta : panel.allSessions()) {
        if (meta.isArchived && !meta.isDismissed && meta.lastAccessed.isValid() && meta.lastAccessed < cutoff7) {
            panel.dismissSession(meta.sessionId);
        }
    }
    QCOMPARE(countActive(), 1);

    // Remaining non-dismissed should be the recent session
    for (const auto &meta : panel.allSessions()) {
        if (meta.isArchived && !meta.isDismissed) {
            QCOMPARE(meta.sessionId, QStringLiteral("age02"));
        }
    }
}

void SessionManagerPanelTest::testBulkCloseMultipleSessions()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("bcls01"), QStringLiteral("konsolai-bcls01")));
    sessions.append(makeSession(QStringLiteral("bcls02"), QStringLiteral("konsolai-bcls02")));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);

    // Close both — should NOT mark as archived
    panel.closeSession(QStringLiteral("bcls01"));
    panel.closeSession(QStringLiteral("bcls02"));

    // Sessions should still exist, not be archived
    QList<SessionMetadata> all = panel.allSessions();
    QCOMPARE(all.size(), 2);
    for (const auto &meta : all) {
        QVERIFY(!meta.isArchived);
    }
}

// ============================================================
// Tree widget rendering — subagent/team subnodes
// ============================================================

// Helper: find the QTreeWidget inside a SessionManagerPanel (m_treeWidget is private)
static QTreeWidget *findTree(SessionManagerPanel &panel)
{
    auto trees = panel.findChildren<QTreeWidget *>();
    return trees.isEmpty() ? nullptr : trees.first();
}

// Helper: force a synchronous tree rebuild.
// updateTreeWidget() uses an async tmux query whose QProcess finished signal
// is unreliable in QTEST_MAIN environments. rebuildTreeSync() bypasses that.
static void forceTreeRebuild(SessionManagerPanel &panel)
{
    panel.rebuildTreeSync();
}

// Helper: recursively collect all tree items matching a predicate
static void collectItems(QTreeWidgetItem *root, std::function<bool(QTreeWidgetItem *)> pred, QList<QTreeWidgetItem *> &out)
{
    if (pred(root)) {
        out.append(root);
    }
    for (int i = 0; i < root->childCount(); ++i) {
        collectItems(root->child(i), pred, out);
    }
}

static QList<QTreeWidgetItem *> findItemsByRole(QTreeWidget *tree, int role, const QVariant &value)
{
    QList<QTreeWidgetItem *> result;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        collectItems(
            tree->topLevelItem(i),
            [&](QTreeWidgetItem *item) {
                return item->data(0, role) == value;
            },
            result);
    }
    return result;
}

// Helper: build a session JSON with subagents attached
static QJsonObject makeSessionWithAgents(const QString &id,
                                         const QString &name,
                                         const QJsonArray &agents,
                                         const QJsonArray &procs = {},
                                         const QJsonObject &labels = {},
                                         int promptRound = 0,
                                         bool archived = true)
{
    QJsonObject s = makeSession(id, name, false, archived);
    if (!agents.isEmpty()) {
        s[QStringLiteral("subagents")] = agents;
    }
    if (!procs.isEmpty()) {
        s[QStringLiteral("subprocesses")] = procs;
    }
    if (!labels.isEmpty()) {
        s[QStringLiteral("promptLabels")] = labels;
    }
    if (promptRound > 0) {
        s[QStringLiteral("promptRound")] = promptRound;
    }
    return s;
}

static QJsonObject makeAgent(const QString &agentId,
                             const QString &agentType,
                             int state = 3 /*NotRunning*/,
                             int promptGroupId = 0,
                             const QString &teammateName = {},
                             const QString &taskDesc = {},
                             const QString &taskSubject = {})
{
    QJsonObject a;
    a[QStringLiteral("agentId")] = agentId;
    a[QStringLiteral("agentType")] = agentType;
    a[QStringLiteral("state")] = state;
    a[QStringLiteral("promptGroupId")] = promptGroupId;
    if (!teammateName.isEmpty())
        a[QStringLiteral("teammateName")] = teammateName;
    if (!taskDesc.isEmpty())
        a[QStringLiteral("taskDescription")] = taskDesc;
    if (!taskSubject.isEmpty())
        a[QStringLiteral("currentTaskSubject")] = taskSubject;
    return a;
}

static QJsonObject makeProc(const QString &id, const QString &command, int status = 1 /*Completed*/, int promptGroupId = 0, int exitCode = 0)
{
    QJsonObject p;
    p[QStringLiteral("id")] = id;
    p[QStringLiteral("command")] = command;
    p[QStringLiteral("fullCommand")] = command;
    p[QStringLiteral("status")] = status;
    p[QStringLiteral("exitCode")] = exitCode;
    p[QStringLiteral("promptGroupId")] = promptGroupId;
    // Give subprocesses a 5-second duration so they aren't filtered as instant commands
    QDateTime start = QDateTime::currentDateTime().addSecs(-10);
    p[QStringLiteral("startedAt")] = start.toString(Qt::ISODate);
    p[QStringLiteral("finishedAt")] = start.addSecs(5).toString(Qt::ISODate);
    return p;
}

void SessionManagerPanelTest::testTreeSubagentItemsRendered()
{
    // Verify subagent items appear in the tree under their session
    QJsonArray agents;
    agents.append(makeAgent(QStringLiteral("a1"), QStringLiteral("Explore"), 3, 0, QStringLiteral("researcher")));
    agents.append(makeAgent(QStringLiteral("a2"), QStringLiteral("Bash"), 3, 0));

    QJsonArray sessions;
    sessions.append(makeSessionWithAgents(QStringLiteral("tree01"), QStringLiteral("konsolai-test-tree01"), agents));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QTreeWidget *tree = findTree(panel);
    QVERIFY(tree);
    forceTreeRebuild(panel);

    // Find items with agentId stored in UserRole
    auto a1Items = findItemsByRole(tree, Qt::UserRole, QStringLiteral("a1"));
    auto a2Items = findItemsByRole(tree, Qt::UserRole, QStringLiteral("a2"));
    QCOMPARE(a1Items.size(), 1);
    QCOMPARE(a2Items.size(), 1);

    // Verify display text includes agent type and teammate name
    QVERIFY(a1Items.first()->text(0).contains(QStringLiteral("Explore")));
    QVERIFY(a1Items.first()->text(0).contains(QStringLiteral("researcher")));
    QVERIFY(a2Items.first()->text(0).contains(QStringLiteral("Bash")));
}

void SessionManagerPanelTest::testTreeSubprocessItemsRendered()
{
    // Verify subprocess items appear in the tree
    QJsonArray procs;
    procs.append(makeProc(QStringLiteral("p1"), QStringLiteral("ninja -j4")));

    QJsonArray sessions;
    sessions.append(makeSessionWithAgents(QStringLiteral("tree02"), QStringLiteral("konsolai-test-tree02"), {}, procs));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QTreeWidget *tree = findTree(panel);
    QVERIFY(tree);
    forceTreeRebuild(panel);

    // Find subprocess items by UserRole+4 (subprocess ID)
    auto pItems = findItemsByRole(tree, Qt::UserRole + 4, QStringLiteral("p1"));
    QCOMPARE(pItems.size(), 1);
    QVERIFY(pItems.first()->text(0).contains(QStringLiteral("ninja")));
}

void SessionManagerPanelTest::testTreeMultiRoundPromptGroups()
{
    // Two prompt rounds → two "Prompt #N" group nodes
    QJsonArray agents;
    agents.append(makeAgent(QStringLiteral("r0a"), QStringLiteral("Explore"), 3, 0));
    agents.append(makeAgent(QStringLiteral("r1a"), QStringLiteral("Plan"), 3, 1));

    QJsonObject labels;
    labels[QStringLiteral("0")] = QStringLiteral("Fix the bug");
    labels[QStringLiteral("1")] = QStringLiteral("Add tests");

    QJsonArray sessions;
    sessions.append(makeSessionWithAgents(QStringLiteral("tree03"), QStringLiteral("konsolai-test-tree03"), agents, {}, labels, 1));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QTreeWidget *tree = findTree(panel);
    QVERIFY(tree);
    forceTreeRebuild(panel);

    // Prompt group nodes identified by UserRole+3 (prompt round ID)
    auto pg0 = findItemsByRole(tree, Qt::UserRole + 3, 0);
    auto pg1 = findItemsByRole(tree, Qt::UserRole + 3, 1);
    QCOMPARE(pg0.size(), 1);
    QCOMPARE(pg1.size(), 1);

    // Verify custom labels are used
    QVERIFY(pg0.first()->text(0).contains(QStringLiteral("Fix the bug")));
    QVERIFY(pg1.first()->text(0).contains(QStringLiteral("Add tests")));

    // Each prompt group should have a "Subtasks" child
    auto subtasks = findItemsByRole(tree, Qt::UserRole + 5, QStringLiteral("subtasks"));
    QCOMPARE(subtasks.size(), 2); // one per round
}

void SessionManagerPanelTest::testTreeTaskGrouping()
{
    // Multiple agents with same taskDescription → grouped under task group node
    QJsonArray agents;
    agents.append(makeAgent(QStringLiteral("tg1"), QStringLiteral("Explore"), 3, 0, QStringLiteral("scout"), QStringLiteral("Search codebase")));
    agents.append(makeAgent(QStringLiteral("tg2"), QStringLiteral("Bash"), 3, 0, QStringLiteral("runner"), QStringLiteral("Search codebase")));
    agents.append(makeAgent(QStringLiteral("tg3"), QStringLiteral("Plan"), 3, 0, {}, QStringLiteral("Different task")));

    QJsonArray sessions;
    sessions.append(makeSessionWithAgents(QStringLiteral("tree04"), QStringLiteral("konsolai-test-tree04"), agents));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QTreeWidget *tree = findTree(panel);
    QVERIFY(tree);
    forceTreeRebuild(panel);

    // Task group node identified by UserRole+2 = task description
    auto taskGroups = findItemsByRole(tree, Qt::UserRole + 2, QStringLiteral("Search codebase"));
    QCOMPARE(taskGroups.size(), 1);

    // Task group should contain 2 agents
    QTreeWidgetItem *group = taskGroups.first();
    QVERIFY(group->text(0).contains(QStringLiteral("2 agents")));

    // The third agent ("Different task") should NOT be in a task group (single agent → inline)
    auto diffItems = findItemsByRole(tree, Qt::UserRole, QStringLiteral("tg3"));
    QCOMPARE(diffItems.size(), 1);
    // Single-agent tasks get the task description prepended inline
    QVERIFY(diffItems.first()->text(0).contains(QStringLiteral("Different task")));
}

void SessionManagerPanelTest::testTreeHideCompletedAgents()
{
    // Archived/persisted sessions force all agents to NotRunning.
    // hideCompletedAgents only applies to active sessions, not persisted trees.
    // So for persisted trees, all agents should always be visible.
    QJsonArray agents;
    agents.append(makeAgent(QStringLiteral("hc1"), QStringLiteral("Explore"), 3, 0)); // NotRunning
    agents.append(makeAgent(QStringLiteral("hc2"), QStringLiteral("Bash"), 3, 0)); // NotRunning

    QJsonArray sessions;
    sessions.append(makeSessionWithAgents(QStringLiteral("tree05"), QStringLiteral("konsolai-test-tree05"), agents));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QTreeWidget *tree = findTree(panel);
    QVERIFY(tree);
    forceTreeRebuild(panel);

    // Both agents should be visible even though they are NotRunning,
    // because persisted trees don't hide completed items
    auto hc1 = findItemsByRole(tree, Qt::UserRole, QStringLiteral("hc1"));
    auto hc2 = findItemsByRole(tree, Qt::UserRole, QStringLiteral("hc2"));
    QCOMPARE(hc1.size(), 1);
    QCOMPARE(hc2.size(), 1);
}

void SessionManagerPanelTest::testTreeSubagentStateIcons()
{
    // Verify that different states produce items with different foreground colors.
    // Persisted agents are forced to NotRunning, so we can only test that state here.
    QJsonArray agents;
    agents.append(makeAgent(QStringLiteral("si1"), QStringLiteral("Explore"), 3, 0)); // NotRunning

    QJsonArray sessions;
    sessions.append(makeSessionWithAgents(QStringLiteral("tree06"), QStringLiteral("konsolai-test-tree06"), agents));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QTreeWidget *tree = findTree(panel);
    QVERIFY(tree);
    forceTreeRebuild(panel);

    auto items = findItemsByRole(tree, Qt::UserRole, QStringLiteral("si1"));
    QCOMPARE(items.size(), 1);

    // NotRunning agents should have gray-ish foreground (QColor(140,140,140))
    QColor fg = items.first()->foreground(0).color();
    QVERIFY(fg.red() > 100 && fg.green() > 100 && fg.blue() > 100); // gray range
    QVERIFY(!items.first()->icon(0).isNull());
}

void SessionManagerPanelTest::testTreePersistedAgentsForcedNotRunning()
{
    // Even if persisted JSON says Working/Idle, tree rendering forces NotRunning
    QJsonArray agents;
    // State 1 = Working in ClaudeProcess::State enum
    agents.append(makeAgent(QStringLiteral("pf1"), QStringLiteral("Explore"), 1, 0));
    // State 2 = Idle
    agents.append(makeAgent(QStringLiteral("pf2"), QStringLiteral("Bash"), 2, 0));

    QJsonArray sessions;
    sessions.append(makeSessionWithAgents(QStringLiteral("tree07"), QStringLiteral("konsolai-test-tree07"), agents));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QTreeWidget *tree = findTree(panel);
    QVERIFY(tree);
    forceTreeRebuild(panel);

    // Both items should be rendered (not filtered as completed, since persisted trees
    // don't use hideCompleted)
    auto pf1 = findItemsByRole(tree, Qt::UserRole, QStringLiteral("pf1"));
    auto pf2 = findItemsByRole(tree, Qt::UserRole, QStringLiteral("pf2"));
    QCOMPARE(pf1.size(), 1);
    QCOMPARE(pf2.size(), 1);

    // Both should have the gray "completed" foreground (forced NotRunning),
    // NOT the green (Working) or gray (Idle) they were stored as
    QColor fg1 = pf1.first()->foreground(0).color();
    QColor fg2 = pf2.first()->foreground(0).color();
    // QColor(140,140,140) — the "completed" color
    QCOMPARE(fg1, QColor(140, 140, 140));
    QCOMPARE(fg2, QColor(140, 140, 140));
}

// ============================================================
// Pin immediate tree update
// ============================================================

void SessionManagerPanelTest::testPinSession_ImmediateTreeUpdate()
{
    // Verify that pinSession() moves the item into the Pinned category immediately
    // (no deferred timer, no processEvents needed)
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("imm11111"), QStringLiteral("konsolai-test-imm11111"), false));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QTreeWidget *tree = findTree(panel);
    QVERIFY(tree);
    forceTreeRebuild(panel);

    // Find the "Pinned" category
    QTreeWidgetItem *pinnedCat = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (tree->topLevelItem(i)->text(0).contains(QStringLiteral("Pinned"))) {
            pinnedCat = tree->topLevelItem(i);
            break;
        }
    }
    QVERIFY(pinnedCat);
    QCOMPARE(pinnedCat->childCount(), 0);

    // Pin without processEvents — should update tree immediately
    panel.pinSession(QStringLiteral("imm11111"));

    // Re-find pinned category (tree was rebuilt)
    pinnedCat = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (tree->topLevelItem(i)->text(0).contains(QStringLiteral("Pinned"))) {
            pinnedCat = tree->topLevelItem(i);
            break;
        }
    }
    QVERIFY(pinnedCat);
    QVERIFY(pinnedCat->childCount() > 0);
}

// ============================================================
// Timer pause/resume (window activation)
// ============================================================

void SessionManagerPanelTest::testPauseResumeIdempotent()
{
    SessionManagerPanel_INIT(panel);

    // Pause twice — should not crash or change behavior
    panel.pauseBackgroundTimers();
    panel.pauseBackgroundTimers();

    // Resume twice — should not crash
    panel.resumeBackgroundTimers();
    panel.resumeBackgroundTimers();

    // Process events to ensure debounced timers fire
    QCoreApplication::processEvents();
}

void SessionManagerPanelTest::testPauseSuppressesTreeUpdates()
{
    SessionManagerPanel_INIT(panel);

    // Register a session first
    ClaudeSession session(QStringLiteral("TestProfile"), QStringLiteral("/home/user/project"));
    panel.registerSession(&session);
    QCoreApplication::processEvents();

    // Pause timers
    panel.pauseBackgroundTimers();

    // Register another session while paused — the tree update will be deferred
    ClaudeSession session2(QStringLiteral("TestProfile2"), QStringLiteral("/home/user/project2"));
    panel.registerSession(&session2);
    QCoreApplication::processEvents();

    // Resume — deferred tree update should flush without crash
    panel.resumeBackgroundTimers();
    QCoreApplication::processEvents();

    panel.unregisterSession(&session);
    panel.unregisterSession(&session2);
}

void SessionManagerPanelTest::testPauseSuppressesMetadataSaves()
{
    SessionManagerPanel_INIT(panel);

    // Register and let initial save happen
    ClaudeSession session(QStringLiteral("TestProfile"), QStringLiteral("/home/user/project"));
    panel.registerSession(&session);
    QCoreApplication::processEvents();
    QTest::qWait(1200); // let initial debounced save fire
    QCoreApplication::processEvents();

    // Pause timers
    panel.pauseBackgroundTimers();

    // Pin session — this triggers scheduleMetadataSave internally
    panel.pinSession(session.sessionId());
    QCoreApplication::processEvents();

    // Resume — deferred save should flush
    panel.resumeBackgroundTimers();
    QCoreApplication::processEvents();
    QTest::qWait(1200); // let debounced save fire
    QCoreApplication::processEvents();

    panel.unregisterSession(&session);
}

void SessionManagerPanelTest::testResumeFlushesDeferred()
{
    SessionManagerPanel_INIT(panel);

    // Register a session, let it settle
    ClaudeSession session(QStringLiteral("TestProfile"), QStringLiteral("/home/user/project"));
    panel.registerSession(&session);
    QCoreApplication::processEvents();
    QTest::qWait(1200);
    QCoreApplication::processEvents();

    // Pause
    panel.pauseBackgroundTimers();

    // Make changes while paused
    panel.pinSession(session.sessionId());
    QCoreApplication::processEvents();

    // Resume — should flush deferred operations
    panel.resumeBackgroundTimers();
    QCoreApplication::processEvents();
    QTest::qWait(1200);
    QCoreApplication::processEvents();

    // Verify session is still tracked and pinned
    QVERIFY(panel.allSessions().size() > 0);
    bool foundPinned = false;
    for (const auto &meta : panel.pinnedSessions()) {
        if (meta.sessionId == session.sessionId()) {
            foundPinned = true;
            break;
        }
    }
    QVERIFY(foundPinned);

    panel.unregisterSession(&session);
}

// ============================================================
// Register fast-path (tab switch)
// ============================================================

void SessionManagerPanelTest::testRegisterSessionFastPath()
{
    // Registering the same session twice should hit the fast path and
    // NOT duplicate metadata or tree entries
    SessionManagerPanel_INIT(panel);

    ClaudeSession session(QStringLiteral("TestProfile"), QStringLiteral("/home/user/project"));
    panel.registerSession(&session);
    QCoreApplication::processEvents(); // let debounced save/update fire

    int countAfterFirst = panel.allSessions().size();

    // Register again (simulates tab switch)
    panel.registerSession(&session);
    QCoreApplication::processEvents();

    QCOMPARE(panel.allSessions().size(), countAfterFirst);

    // Metadata should still exist and lastAccessed should be updated
    const SessionMetadata *meta = panel.sessionMetadata(session.sessionId());
    QVERIFY(meta);
    QVERIFY(meta->lastAccessed.secsTo(QDateTime::currentDateTime()) < 5);
}

// ============================================================
// Auto-archive old closed sessions
// ============================================================

void SessionManagerPanelTest::testAutoArchiveClosedSessions()
{
    // Create a closed (expired) session with lastAccessed 8 days ago
    QJsonArray sessions;
    QJsonObject old = makeSession(QStringLiteral("old11111"), QStringLiteral("konsolai-test-old11111"), false, false, true);
    old[QStringLiteral("lastAccessed")] = QDateTime::currentDateTime().addDays(-8).toString(Qt::ISODate);
    sessions.append(old);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.allSessions().size(), 1);

    // Verify it's expired but not archived
    const SessionMetadata *meta = panel.sessionMetadata(QStringLiteral("old11111"));
    QVERIFY(meta);
    QVERIFY(meta->isExpired);
    QVERIFY(!meta->isArchived);

    // Trigger auto-archive (normally runs on timer)
    panel.autoArchiveOldClosedSessions();

    // Should now be archived
    meta = panel.sessionMetadata(QStringLiteral("old11111"));
    QVERIFY(meta);
    QVERIFY(meta->isArchived);
}

void SessionManagerPanelTest::testAutoArchiveSkipsPinned()
{
    // Create a closed (expired) pinned session with lastAccessed 8 days ago
    QJsonArray sessions;
    QJsonObject pinned = makeSession(QStringLiteral("pin11111"), QStringLiteral("konsolai-test-pin11111"), true, false, true);
    pinned[QStringLiteral("lastAccessed")] = QDateTime::currentDateTime().addDays(-8).toString(Qt::ISODate);
    sessions.append(pinned);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    panel.autoArchiveOldClosedSessions();

    // Should NOT be archived because it's pinned
    const SessionMetadata *meta = panel.sessionMetadata(QStringLiteral("pin11111"));
    QVERIFY(meta);
    QVERIFY(!meta->isArchived);
}

void SessionManagerPanelTest::testAutoArchiveSkipsRecent()
{
    // Create a closed (expired) session with lastAccessed 3 days ago (under threshold)
    QJsonArray sessions;
    QJsonObject recent = makeSession(QStringLiteral("rec11111"), QStringLiteral("konsolai-test-rec11111"), false, false, true);
    recent[QStringLiteral("lastAccessed")] = QDateTime::currentDateTime().addDays(-3).toString(Qt::ISODate);
    sessions.append(recent);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    panel.autoArchiveOldClosedSessions();

    // Should NOT be archived because it's only 3 days old
    const SessionMetadata *meta = panel.sessionMetadata(QStringLiteral("rec11111"));
    QVERIFY(meta);
    QVERIFY(!meta->isArchived);
}

// ============================================================
// Session folders
// ============================================================

void SessionManagerPanelTest::testCreateFolder()
{
    SessionManagerPanel_INIT(panel);
    QCOMPARE(panel.allFolders().size(), 0);

    panel.createFolder(QStringLiteral("My Fleet"), QColor(Qt::red));
    QCOMPARE(panel.allFolders().size(), 1);
    QCOMPARE(panel.allFolders().first().name, QStringLiteral("My Fleet"));
    QVERIFY(panel.allFolders().first().color == QColor(Qt::red));
    QVERIFY(!panel.allFolders().first().folderId.isEmpty());
}

void SessionManagerPanelTest::testRenameFolderEmpty()
{
    SessionManagerPanel_INIT(panel);
    panel.createFolder(QStringLiteral("Test"), QColor());
    QString fid = panel.allFolders().first().folderId;

    // Empty name should be rejected
    panel.renameFolder(fid, QString());
    QCOMPARE(panel.allFolders().first().name, QStringLiteral("Test"));

    // Valid rename
    panel.renameFolder(fid, QStringLiteral("Renamed"));
    QCOMPARE(panel.allFolders().first().name, QStringLiteral("Renamed"));
}

void SessionManagerPanelTest::testDeleteFolderUngroupsSessions()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("s1"), QStringLiteral("konsolai-Test-s1")));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    panel.createFolder(QStringLiteral("Fleet"), QColor());
    QString fid = panel.allFolders().first().folderId;

    panel.moveSessionToFolder(QStringLiteral("s1"), fid);
    QCOMPARE(panel.sessionMetadata(QStringLiteral("s1"))->folderId, fid);

    panel.deleteFolder(fid);
    QCOMPARE(panel.allFolders().size(), 0);
    QVERIFY(panel.sessionMetadata(QStringLiteral("s1"))->folderId.isEmpty());
}

void SessionManagerPanelTest::testMoveSessionToFolder()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("s1"), QStringLiteral("konsolai-Test-s1")));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    panel.createFolder(QStringLiteral("A"), QColor());
    panel.createFolder(QStringLiteral("B"), QColor());
    auto folders = panel.allFolders();
    QCOMPARE(folders.size(), 2);

    QString fidA = folders[0].name == QStringLiteral("A") ? folders[0].folderId : folders[1].folderId;
    QString fidB = folders[0].name == QStringLiteral("B") ? folders[0].folderId : folders[1].folderId;

    panel.moveSessionToFolder(QStringLiteral("s1"), fidA);
    QCOMPARE(panel.sessionMetadata(QStringLiteral("s1"))->folderId, fidA);

    // Move to different folder
    panel.moveSessionToFolder(QStringLiteral("s1"), fidB);
    QCOMPARE(panel.sessionMetadata(QStringLiteral("s1"))->folderId, fidB);
}

void SessionManagerPanelTest::testRemoveSessionFromFolder()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("s1"), QStringLiteral("konsolai-Test-s1")));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    panel.createFolder(QStringLiteral("F"), QColor());
    QString fid = panel.allFolders().first().folderId;

    panel.moveSessionToFolder(QStringLiteral("s1"), fid);
    QVERIFY(!panel.sessionMetadata(QStringLiteral("s1"))->folderId.isEmpty());

    panel.removeSessionFromFolder(QStringLiteral("s1"));
    QVERIFY(panel.sessionMetadata(QStringLiteral("s1"))->folderId.isEmpty());
}

void SessionManagerPanelTest::testFolderIdPersistence()
{
    // Write sessions with folderId pre-set
    QJsonArray sessions;
    QJsonObject s1 = makeSession(QStringLiteral("s1"), QStringLiteral("konsolai-Test-s1"));
    s1[QStringLiteral("folderId")] = QStringLiteral("folder-abc");
    sessions.append(s1);
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    const SessionMetadata *meta = panel.sessionMetadata(QStringLiteral("s1"));
    QVERIFY(meta);
    QCOMPARE(meta->folderId, QStringLiteral("folder-abc"));
}

void SessionManagerPanelTest::testFoldersPersistence()
{
    // Create a folder, verify it persists to disk
    {
        SessionManagerPanel_INIT(panel);
        panel.createFolder(QStringLiteral("Persisted"), QColor(0x11, 0x22, 0x33));
        QCoreApplication::processEvents(); // flush async write
        QTest::qWait(100); // allow QtConcurrent::run to complete
    }

    // Re-create panel — should load the folder
    {
        SessionManagerPanel_INIT(panel2);
        QCOMPARE(panel2.allFolders().size(), 1);
        QCOMPARE(panel2.allFolders().first().name, QStringLiteral("Persisted"));
    }
}

void SessionManagerPanelTest::testTreeRoutesGroupedSessionsToFolder()
{
    QJsonArray sessions;
    sessions.append(makeSession(QStringLiteral("s1"), QStringLiteral("konsolai-Test-s1")));
    sessions.append(makeSession(QStringLiteral("s2"), QStringLiteral("konsolai-Test-s2")));
    writeTestSessions(sessions);

    SessionManagerPanel_INIT(panel);
    panel.createFolder(QStringLiteral("MyGroup"), QColor());
    QString fid = panel.allFolders().first().folderId;

    // Move s1 into the folder
    panel.moveSessionToFolder(QStringLiteral("s1"), fid);
    panel.rebuildTreeSync();

    // Find the tree widget
    auto *tree = panel.findChild<QTreeWidget *>();
    QVERIFY(tree);

    // Find the folder top-level item
    QTreeWidgetItem *folderItem = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto *topItem = tree->topLevelItem(i);
        if (topItem->data(0, Qt::UserRole + 7).toString() == fid) {
            folderItem = topItem;
            break;
        }
    }
    QVERIFY(folderItem);
    QCOMPARE(folderItem->childCount(), 1); // s1 is in the folder

    // Verify s1 is under the folder, not under a category
    QString childId = folderItem->child(0)->data(0, Qt::UserRole).toString();
    QCOMPARE(childId, QStringLiteral("s1"));
}

QTEST_MAIN(SessionManagerPanelTest)

#include "moc_SessionManagerPanelTest.cpp"
