/*
    SPDX-FileCopyrightText: 2025 Struktured Labs
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "SessionManagerPanel.h"
#include "ClaudeConversationPicker.h"
#include "ClaudeSession.h"
#include "ClaudeSessionRegistry.h"
#include "KonsolaiSettings.h"
#include "NotificationManager.h"
#include "TmuxManager.h"

#include <limits>

#include <KLocalizedString>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QToolBar>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace Konsolai
{

// Result of background cache refresh (discoverSessions + readClaudeConversations)
struct CacheRefreshResult {
    QList<ClaudeSessionState> discovered;
    QHash<QString, QList<ClaudeConversation>> conversations;
};

static const QString SETTINGS_GROUP = QStringLiteral("SessionManager");

static QString formatElapsed(const QDateTime &start)
{
    if (!start.isValid()) {
        return {};
    }
    qint64 secs = start.secsTo(QDateTime::currentDateTime());
    if (secs < 0) {
        secs = 0;
    }
    if (secs < 60) {
        return QStringLiteral("%1s").arg(secs);
    }
    qint64 mins = secs / 60;
    qint64 remSecs = secs % 60;
    if (mins < 60) {
        return QStringLiteral("%1m %2s").arg(mins).arg(remSecs);
    }
    qint64 hours = mins / 60;
    qint64 remMins = mins % 60;
    return QStringLiteral("%1h %2m").arg(hours).arg(remMins);
}

SessionManagerPanel::SessionManagerPanel(QWidget *parent)
    : QWidget(parent)
    , m_registry(ClaudeSessionRegistry::instance())
{
    setupUi();
    showLoadingState();
    // Defer heavy init (metadata parse, tmux queries, timers) so the event loop
    // can paint the window first, making startup feel snappy.
    QTimer::singleShot(0, this, &SessionManagerPanel::deferredInit);
}

void SessionManagerPanel::deferredInit()
{
    loadMetadata();
    loadFolders();
    cleanupStaleSockets(); // async — returns immediately
    refreshRemoteTmuxSessions(); // async SSH query for remote session liveness
    refresh(); // async — returns immediately

    // Periodically refresh remote tmux session liveness (every 60s)
    m_remoteTmuxTimer = new QTimer(this);
    m_remoteTmuxTimer->setInterval(60000);
    connect(m_remoteTmuxTimer, &QTimer::timeout, this, &SessionManagerPanel::refreshRemoteTmuxSessions);
    m_remoteTmuxTimer->start();

    // TTL-based cache invalidation timers
    m_gitCacheTimer = new QTimer(this);
    m_gitCacheTimer->setInterval(60000); // 60s TTL for git branch cache
    connect(m_gitCacheTimer, &QTimer::timeout, this, [this]() {
        m_gitBranchCache.clear();
    });
    m_gitCacheTimer->start();

    m_convCacheTimer = new QTimer(this);
    m_convCacheTimer->setInterval(120000); // 120s — refresh caches in background (no UI freeze)
    connect(m_convCacheTimer, &QTimer::timeout, this, [this]() {
        m_gsdBadgeCache.clear(); // cheap, no I/O
        refreshCachesAsync(); // heavy I/O runs on thread pool
    });
    m_convCacheTimer->start();

    // Pre-populate caches on startup
    refreshCachesAsync();

    // Auto-archive closed sessions every 5 minutes
    m_autoArchiveTimer = new QTimer(this);
    m_autoArchiveTimer->setInterval(300000); // 5 minutes
    connect(m_autoArchiveTimer, &QTimer::timeout, this, &SessionManagerPanel::autoArchiveOldClosedSessions);
    m_autoArchiveTimer->start();
    // Run once on startup after a short delay
    QTimer::singleShot(10000, this, &SessionManagerPanel::autoArchiveOldClosedSessions);

    // Process any sessions that registered before init completed
    m_initialized = true;
    for (const auto &session : std::as_const(m_pendingRegistrations)) {
        if (session) {
            registerSession(session.data());
        }
    }
    m_pendingRegistrations.clear();

    showReadyState();
}

SessionManagerPanel::~SessionManagerPanel()
{
    // Stop all timers to prevent callbacks during/after destruction
    if (m_updateDebounce) {
        m_updateDebounce->stop();
    }
    if (m_saveDebounce) {
        m_saveDebounce->stop();
    }
    if (m_durationTimer) {
        m_durationTimer->stop();
    }
    if (m_deferRetryTimer) {
        m_deferRetryTimer->stop();
    }
    if (m_remoteTmuxTimer) {
        m_remoteTmuxTimer->stop();
    }
    if (m_gitCacheTimer) {
        m_gitCacheTimer->stop();
    }
    if (m_convCacheTimer) {
        m_convCacheTimer->stop();
    }

    // Block signals during destruction — saveMetadata() emits usageAggregateChanged(),
    // and connected slots in MainWindow may dereference already-destroyed sibling widgets
    // (e.g. ClaudeStatusWidget destroyed before SessionManagerPanel in deleteChildren()).
    blockSignals(true);
    saveMetadata(/*sync=*/true);

    // Null out m_treeWidget before base QWidget destructor runs deleteChildren(),
    // which can trigger QProcess::finished → scheduleTreeUpdate → isTreeInteractionActive
    // accessing already-destroyed child widgets.
    m_treeWidget = nullptr;
}

void SessionManagerPanel::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    // Header with collapse button and new session button
    auto *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(4, 4, 4, 4);

    m_collapseButton = new QPushButton(this);
    m_collapseButton->setIcon(QIcon::fromTheme(QStringLiteral("sidebar-collapse-left")));
    m_collapseButton->setFlat(true);
    m_collapseButton->setFixedSize(24, 24);
    m_collapseButton->setToolTip(i18n("Toggle Session Panel"));
    connect(m_collapseButton, &QPushButton::clicked, this, [this]() {
        setCollapsed(!m_collapsed);
    });
    headerLayout->addWidget(m_collapseButton);

    // Note: Title "Sessions" is shown in dock widget title bar, no need for duplicate label here

    headerLayout->addStretch();

    // Notification settings button
    auto *notifyButton = new QPushButton(this);
    notifyButton->setIcon(QIcon::fromTheme(QStringLiteral("preferences-desktop-notification")));
    notifyButton->setFlat(true);
    notifyButton->setFixedSize(24, 24);
    notifyButton->setToolTip(i18n("Notification Settings"));
    connect(notifyButton, &QPushButton::clicked, this, [this]() {
        auto *mgr = NotificationManager::instance();
        if (!mgr) {
            return;
        }

        auto *dlg = new QDialog(this);
        dlg->setWindowTitle(i18n("Notification Settings"));
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        auto *vbox = new QVBoxLayout(dlg);

        auto *audioCheck = new QCheckBox(i18n("Sound alerts"), dlg);
        audioCheck->setChecked(mgr->isChannelEnabled(NotificationManager::Channel::Audio));
        vbox->addWidget(audioCheck);

        auto *volLayout = new QHBoxLayout();
        volLayout->addSpacing(20);
        auto *volLabel = new QLabel(i18n("Volume:"), dlg);
        volLayout->addWidget(volLabel);
        auto *volSlider = new QSlider(Qt::Horizontal, dlg);
        volSlider->setRange(0, 100);
        volSlider->setValue(static_cast<int>(mgr->audioVolume() * 100));
        volSlider->setEnabled(audioCheck->isChecked());
        volLayout->addWidget(volSlider);
        vbox->addLayout(volLayout);

        auto *desktopCheck = new QCheckBox(i18n("Desktop notifications"), dlg);
        desktopCheck->setChecked(mgr->isChannelEnabled(NotificationManager::Channel::Desktop));
        vbox->addWidget(desktopCheck);

        auto *terminalCheck = new QCheckBox(i18n("In-terminal overlay"), dlg);
        terminalCheck->setChecked(mgr->isChannelEnabled(NotificationManager::Channel::InTerminal));
        vbox->addWidget(terminalCheck);

        auto *trayCheck = new QCheckBox(i18n("System tray status"), dlg);
        trayCheck->setChecked(mgr->isChannelEnabled(NotificationManager::Channel::SystemTray));
        vbox->addWidget(trayCheck);

        auto *yoloCheck = new QCheckBox(i18n("Yolo approval sounds"), dlg);
        yoloCheck->setChecked(mgr->yoloNotificationsEnabled());
        vbox->addWidget(yoloCheck);

        connect(audioCheck, &QCheckBox::toggled, volSlider, &QSlider::setEnabled);

        auto *testSoundBtn = new QPushButton(i18n("Test Sound"), dlg);
        connect(testSoundBtn, &QPushButton::clicked, mgr, [mgr]() {
            mgr->playSound(NotificationManager::NotificationType::TaskComplete);
        });
        vbox->addWidget(testSoundBtn);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
        vbox->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, dlg, [=]() {
            mgr->enableChannel(NotificationManager::Channel::Audio, audioCheck->isChecked());
            mgr->enableChannel(NotificationManager::Channel::Desktop, desktopCheck->isChecked());
            mgr->enableChannel(NotificationManager::Channel::InTerminal, terminalCheck->isChecked());
            mgr->enableChannel(NotificationManager::Channel::SystemTray, trayCheck->isChecked());
            mgr->setAudioVolume(volSlider->value() / 100.0);
            mgr->setYoloNotificationsEnabled(yoloCheck->isChecked());
            mgr->saveSettings();
            dlg->accept();
        });

        dlg->resize(300, 250);
        dlg->show();
    });
    headerLayout->addWidget(notifyButton);

    m_newSessionButton = new QPushButton(this);
    m_newSessionButton->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
    m_newSessionButton->setFlat(true);
    m_newSessionButton->setFixedSize(24, 24);
    m_newSessionButton->setToolTip(i18n("New Claude Session"));
    connect(m_newSessionButton, &QPushButton::clicked, this, &SessionManagerPanel::onNewSessionClicked);
    headerLayout->addWidget(m_newSessionButton);

    layout->addLayout(headerLayout);

    // Search/filter bar
    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setObjectName(QStringLiteral("sessionFilter"));
    m_filterEdit->setPlaceholderText(i18n("Filter sessions..."));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->addAction(QIcon::fromTheme(QStringLiteral("edit-find")), QLineEdit::LeadingPosition);
    m_filterEdit->setContentsMargins(4, 0, 4, 0);
    connect(m_filterEdit, &QLineEdit::textChanged, this, &SessionManagerPanel::applyFilter);
    layout->addWidget(m_filterEdit);

    // Loading progress bar (shown during deferred init, hidden after)
    m_loadingBar = new QProgressBar(this);
    m_loadingBar->setRange(0, 0); // indeterminate
    m_loadingBar->setTextVisible(false);
    m_loadingBar->setFixedHeight(4);
    m_loadingBar->setVisible(false);
    layout->addWidget(m_loadingBar);

    // Tree widget for sessions
    m_treeWidget = new QTreeWidget(this);
    m_treeWidget->setObjectName(QStringLiteral("sessionTree"));
    m_treeWidget->setColumnCount(2);
    m_treeWidget->setHeaderHidden(true);
    m_treeWidget->setRootIsDecorated(true);
    m_treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    m_treeWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_treeWidget->setIndentation(12);
    // Column 0: session name (stretches), Column 1: indicators (fixed width, right-aligned)
    m_treeWidget->header()->setStretchLastSection(false);
    m_treeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_treeWidget->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    connect(m_treeWidget, &QTreeWidget::itemDoubleClicked, this, &SessionManagerPanel::onItemDoubleClicked);
    connect(m_treeWidget, &QTreeWidget::customContextMenuRequested, this, &SessionManagerPanel::onContextMenu);

    // Detect when user stops interacting with tree to flush deferred updates
    m_treeWidget->viewport()->installEventFilter(this);
    m_treeWidget->installEventFilter(this);

    layout->addWidget(m_treeWidget);

    // Empty state overlay
    m_emptyStateLabel = new QLabel(m_treeWidget);
    m_emptyStateLabel->setText(i18n("No Claude sessions yet\nOpen a Claude tab to get started"));
    m_emptyStateLabel->setAlignment(Qt::AlignCenter);
    m_emptyStateLabel->setStyleSheet(QStringLiteral("color: #757575; font-style: italic; padding: 40px;"));
    m_emptyStateLabel->setWordWrap(true);
    m_emptyStateLabel->setVisible(true);

    // Create category items
    m_pinnedCategory = new QTreeWidgetItem(m_treeWidget);
    m_pinnedCategory->setText(0, i18n("Pinned"));
    m_pinnedCategory->setIcon(0, QIcon::fromTheme(QStringLiteral("pin")));
    m_pinnedCategory->setFlags(Qt::ItemIsEnabled);
    m_pinnedCategory->setExpanded(true);

    m_activeCategory = new QTreeWidgetItem(m_treeWidget);
    m_activeCategory->setText(0, i18n("Active"));
    m_activeCategory->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-start")));
    m_activeCategory->setFlags(Qt::ItemIsEnabled);
    m_activeCategory->setExpanded(true);

    m_detachedCategory = new QTreeWidgetItem(m_treeWidget);
    m_detachedCategory->setText(0, i18n("Detached"));
    m_detachedCategory->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-pause")));
    m_detachedCategory->setFlags(Qt::ItemIsEnabled);
    m_detachedCategory->setExpanded(true);

    m_closedCategory = new QTreeWidgetItem(m_treeWidget);
    m_closedCategory->setText(0, i18n("Closed"));
    m_closedCategory->setIcon(0, QIcon::fromTheme(QStringLiteral("window-close")));
    m_closedCategory->setFlags(Qt::ItemIsEnabled);
    m_closedCategory->setExpanded(true);

    m_archivedCategory = new QTreeWidgetItem(m_treeWidget);
    m_archivedCategory->setText(0, i18n("Archived"));
    m_archivedCategory->setIcon(0, QIcon::fromTheme(QStringLiteral("archive-remove")));
    m_archivedCategory->setFlags(Qt::ItemIsEnabled);
    m_archivedCategory->setExpanded(false);

    m_dismissedCategory = new QTreeWidgetItem(m_treeWidget);
    m_dismissedCategory->setText(0, i18n("Dismissed"));
    m_dismissedCategory->setIcon(0, QIcon::fromTheme(QStringLiteral("edit-clear-history")));
    m_dismissedCategory->setFlags(Qt::ItemIsEnabled);
    m_dismissedCategory->setExpanded(false);

    m_discoveredCategory = new QTreeWidgetItem(m_treeWidget);
    m_discoveredCategory->setText(0, i18n("Discovered"));
    m_discoveredCategory->setIcon(0, QIcon::fromTheme(QStringLiteral("edit-find")));
    m_discoveredCategory->setFlags(Qt::ItemIsEnabled);
    m_discoveredCategory->setExpanded(false);

    setMinimumWidth(200);
    setMaximumWidth(350);
}

void SessionManagerPanel::showLoadingState()
{
    m_emptyStateLabel->setText(i18n("Loading sessions..."));
    m_emptyStateLabel->setVisible(true);
    m_loadingBar->setVisible(true);
    m_treeWidget->setVisible(false);
}

void SessionManagerPanel::showReadyState()
{
    m_loadingBar->setVisible(false);
    m_emptyStateLabel->setText(i18n("No Claude sessions yet\nOpen a Claude tab to get started"));
    // Tree visibility will be managed by updateTreeWidget — just ensure it's shown
    m_treeWidget->setVisible(true);
}

void SessionManagerPanel::setCollapsed(bool collapsed)
{
    if (m_collapsed == collapsed) {
        return;
    }

    m_collapsed = collapsed;

    if (collapsed) {
        m_collapseButton->setIcon(QIcon::fromTheme(QStringLiteral("sidebar-expand-left")));
        setMaximumWidth(32);
        m_treeWidget->hide();
        m_filterEdit->hide();
        m_newSessionButton->hide();
    } else {
        m_collapseButton->setIcon(QIcon::fromTheme(QStringLiteral("sidebar-collapse-left")));
        setMaximumWidth(350);
        m_treeWidget->show();
        m_filterEdit->show();
        m_newSessionButton->show();
    }

    Q_EMIT collapsedChanged(collapsed);
}

void SessionManagerPanel::ensureHooksConfigured(ClaudeSession *session)
{
    if (!session || session->isRemote()) {
        return; // Skip for null or remote sessions
    }

    QString workDir = session->workingDirectory();
    if (workDir.isEmpty()) {
        return;
    }

    QString settingsPath = workDir + QStringLiteral("/.claude/settings.local.json");
    QString sessionId = session->sessionId();
    QString socketPath = ClaudeHookHandler::sessionDataDir() + QStringLiteral("/sessions/") + sessionId + QStringLiteral(".sock");
    QString handlerPath = ClaudeHookHandler::hookHandlerPath();

    if (handlerPath.isEmpty()) {
        return; // No hook handler available
    }

    // Check if hooks are already configured for this session
    QFile file(settingsPath);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();

        if (doc.isObject()) {
            QJsonObject settings = doc.object();
            if (settings.contains(QStringLiteral("hooks"))) {
                QJsonObject hooks = settings[QStringLiteral("hooks")].toObject();
                // Check if any hook points to our socket
                QByteArray hooksBytes = QJsonDocument(hooks).toJson();
                if (hooksBytes.contains(socketPath.toUtf8())) {
                    return; // Hooks already configured correctly
                }
            }
        }
    }

    // Hooks missing or pointing to wrong socket - repair them
    qDebug() << "SessionManagerPanel: Repairing missing/stale hooks for session" << sessionId;

    // Read existing settings
    QJsonObject settings;
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            settings = doc.object();
        }
        file.close();
    }

    // Generate hooks config
    auto makeHookEntry = [&](const QString &eventType) -> QJsonArray {
        QString cmdStr = QStringLiteral("'%1' --socket '%2' --event '%3'").arg(handlerPath, socketPath, eventType);
        QJsonObject hookDef;
        hookDef[QStringLiteral("type")] = QStringLiteral("command");
        hookDef[QStringLiteral("command")] = cmdStr;

        QJsonObject entry;
        entry[QStringLiteral("matcher")] = QStringLiteral("*");
        entry[QStringLiteral("hooks")] = QJsonArray{hookDef};
        return QJsonArray{entry};
    };

    QJsonObject hooks;
    hooks[QStringLiteral("Notification")] = makeHookEntry(QStringLiteral("Notification"));
    hooks[QStringLiteral("Stop")] = makeHookEntry(QStringLiteral("Stop"));
    hooks[QStringLiteral("PreToolUse")] = makeHookEntry(QStringLiteral("PreToolUse"));
    hooks[QStringLiteral("PostToolUse")] = makeHookEntry(QStringLiteral("PostToolUse"));
    hooks[QStringLiteral("PermissionRequest")] = makeHookEntry(QStringLiteral("PermissionRequest"));
    hooks[QStringLiteral("SubagentStart")] = makeHookEntry(QStringLiteral("SubagentStart"));
    hooks[QStringLiteral("SubagentStop")] = makeHookEntry(QStringLiteral("SubagentStop"));
    hooks[QStringLiteral("TeammateIdle")] = makeHookEntry(QStringLiteral("TeammateIdle"));
    hooks[QStringLiteral("TaskCompleted")] = makeHookEntry(QStringLiteral("TaskCompleted"));

    settings[QStringLiteral("hooks")] = hooks;

    // Ensure .claude directory exists
    QDir().mkpath(workDir + QStringLiteral("/.claude"));

    // Write settings
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(QJsonDocument(settings).toJson(QJsonDocument::Indented));
        file.close();
        qDebug() << "SessionManagerPanel: Repaired hooks for session" << sessionId;
    }
}

void SessionManagerPanel::registerSession(ClaudeSession *session)
{
    if (!session) {
        return;
    }

    // Queue sessions arriving before deferred init completes
    if (!m_initialized) {
        m_pendingRegistrations.append(session);
        return;
    }

    QString sessionId = session->sessionId();

    // Fast path: session already registered (tab switch, not new registration).
    // Update lastAccessed in memory only — no save or tree rebuild needed.
    // The timestamp will be persisted on the next natural save (state change, approval, etc.).
    if (m_activeSessions.contains(sessionId) && m_activeSessions[sessionId] == session) {
        if (m_metadata.contains(sessionId)) {
            m_metadata[sessionId].lastAccessed = QDateTime::currentDateTime();
        }
        return;
    }

    m_activeSessions[sessionId] = session;

    // Ensure hooks are configured for this session's project
    ensureHooksConfigured(session);

    // Update or create metadata
    if (!m_metadata.contains(sessionId)) {
        SessionMetadata meta;
        meta.sessionId = sessionId;
        meta.sessionName = session->sessionName();
        meta.profileName = session->profileName();
        meta.workingDirectory = session->workingDirectory();
        meta.createdAt = QDateTime::currentDateTime();
        meta.lastAccessed = meta.createdAt;
        meta.isArchived = false;
        // New session - capture initial yolo mode from session (which comes from global settings)
        meta.yoloMode = session->yoloMode();
        meta.doubleYoloMode = session->doubleYoloMode();
        meta.tripleYoloMode = session->tripleYoloMode();
        // Capture remote SSH fields so session can be restored after restart
        meta.isRemote = session->isRemote();
        meta.sshHost = session->sshHost();
        meta.sshUsername = session->sshUsername();
        meta.sshPort = session->sshPort();
        m_metadata[sessionId] = meta;
    } else {
        // Session was archived/expired/closed, now reopened - clear stale flags
        m_metadata[sessionId].isArchived = false;
        m_metadata[sessionId].isExpired = false;
        m_metadata[sessionId].isDismissed = false;
        m_metadata[sessionId].sessionName = session->sessionName();
        m_metadata[sessionId].lastAccessed = QDateTime::currentDateTime();
        m_explicitlyClosed.remove(sessionId);

        // Restore per-session yolo mode settings from saved metadata
        session->setYoloMode(m_metadata[sessionId].yoloMode);
        session->setDoubleYoloMode(m_metadata[sessionId].doubleYoloMode);
        session->setTripleYoloMode(m_metadata[sessionId].tripleYoloMode);

        // Restore approval counts and log from saved metadata
        const auto &meta = m_metadata[sessionId];
        if (meta.yoloApprovalCount > 0 || meta.doubleYoloApprovalCount > 0 || meta.tripleYoloApprovalCount > 0) {
            session->restoreApprovalState(meta.yoloApprovalCount, meta.doubleYoloApprovalCount, meta.tripleYoloApprovalCount, meta.approvalLog);
        }

        qDebug() << "SessionManagerPanel: Restored yolo mode for" << sessionId << "- yolo:" << meta.yoloMode << "double:" << meta.doubleYoloMode
                 << "triple:" << meta.tripleYoloMode << "approvals:" << (meta.yoloApprovalCount + meta.doubleYoloApprovalCount + meta.tripleYoloApprovalCount);
    }

    scheduleMetadataSave();

    // Invalidate caches for this session's working directory and refresh in background
    const QString &workDir = session->workingDirectory();
    if (!workDir.isEmpty()) {
        m_conversationCache.remove(workDir);
        m_gitBranchCache.remove(workDir);
        m_gsdBadgeCache.remove(workDir);
    }
    m_discoveredCacheValid = false;
    refreshCachesAsync();

    scheduleTreeUpdate();

    // Signal connections below are idempotent — previous connections for this
    // session pointer are disconnected first (handles session ID reuse after unarchive).
    disconnect(session, nullptr, this, nullptr);

    // Connect to session finished (PTY died, e.g. tab closed) — fires immediately
    connect(session, &Konsole::Session::finished, this, [this, sessionId]() {
        if (m_activeSessions.contains(sessionId)) {
            m_activeSessions.remove(sessionId);
            scheduleTreeUpdate();
        }
    });

    // Connect to session destruction (backup, fires later via deleteLater)
    connect(session, &QObject::destroyed, this, [this, sessionId]() {
        if (m_activeSessions.contains(sessionId)) {
            m_activeSessions.remove(sessionId);
            scheduleTreeUpdate();
        }
    });

    // Connect to working directory changes (after run() gets real path from tmux)
    QPointer<ClaudeSession> safeSession(session);
    connect(session, &ClaudeSession::workingDirectoryChanged, this, [this, safeSession, sessionId](const QString &newPath) {
        if (!safeSession || !m_metadata.contains(sessionId) || newPath.isEmpty()) {
            return;
        }
        // Invalidate caches for old and new working directory
        const QString oldPath = m_metadata[sessionId].workingDirectory;
        if (!oldPath.isEmpty()) {
            m_conversationCache.remove(oldPath);
            m_gitBranchCache.remove(oldPath);
            m_gsdBadgeCache.remove(oldPath);
        }
        m_conversationCache.remove(newPath);
        m_gitBranchCache.remove(newPath);
        m_gsdBadgeCache.remove(newPath);

        m_metadata[sessionId].workingDirectory = newPath;
        // Re-run hook setup now that we have a valid working directory
        // (hooks require workDir and skip if empty at registerSession time)
        ensureHooksConfigured(safeSession);
        scheduleMetadataSave();
        updateTreeWidget();
        qDebug() << "SessionManagerPanel: Updated working directory for" << sessionId << "to" << newPath;
    });

    // Connect to approval count changes to update display (debounced, with smart filtering)
    connect(session, &ClaudeSession::approvalCountChanged, this, [this, sessionId]() {
        ClaudeSession *s = m_activeSessions.value(sessionId);
        int newCount = s ? s->totalApprovalCount() : 0;
        if (m_lastKnownApprovalCount.value(sessionId, -1) == newCount) {
            return; // No visible change, skip rebuild
        }
        m_lastKnownApprovalCount[sessionId] = newCount;
        scheduleTreeUpdate();
    });

    // Persist approval state on each new approval (debounced to avoid excessive I/O)
    connect(session, &ClaudeSession::approvalLogged, this, [this, sessionId](const ApprovalLogEntry &entry) {
        Q_UNUSED(entry);
        if (m_metadata.contains(sessionId)) {
            if (ClaudeSession *s = m_activeSessions.value(sessionId)) {
                m_metadata[sessionId].yoloApprovalCount = s->yoloApprovalCount();
                m_metadata[sessionId].doubleYoloApprovalCount = s->doubleYoloApprovalCount();
                m_metadata[sessionId].tripleYoloApprovalCount = s->tripleYoloApprovalCount();
                m_metadata[sessionId].approvalLog = s->approvalLog();
            }
        }
        scheduleMetadataSave();
    });

    // Connect to all yolo mode changes to update display and persist per-session settings
    connect(session, &ClaudeSession::yoloModeChanged, this, [this, sessionId](bool enabled) {
        if (m_metadata.contains(sessionId)) {
            m_metadata[sessionId].yoloMode = enabled;
            scheduleMetadataSave();
        }
        scheduleTreeUpdate();
    });
    connect(session, &ClaudeSession::doubleYoloModeChanged, this, [this, sessionId](bool enabled) {
        if (m_metadata.contains(sessionId)) {
            m_metadata[sessionId].doubleYoloMode = enabled;
            scheduleMetadataSave();
        }
        scheduleTreeUpdate();
    });
    connect(session, &ClaudeSession::tripleYoloModeChanged, this, [this, sessionId](bool enabled) {
        if (m_metadata.contains(sessionId)) {
            m_metadata[sessionId].tripleYoloMode = enabled;
            scheduleMetadataSave();
        }
        scheduleTreeUpdate();
    });

    // Connect to state changes (Working/Idle/etc.) to update live activity line (with smart filtering)
    connect(session, &ClaudeSession::stateChanged, this, [this, sessionId](ClaudeProcess::State newState) {
        if (m_lastKnownState.value(sessionId, static_cast<ClaudeProcess::State>(-1)) == newState) {
            return; // No visible change, skip rebuild
        }
        m_lastKnownState[sessionId] = newState;
        scheduleTreeUpdate();
    });

    // Connect to task description changes to update display
    connect(session, &ClaudeSession::taskDescriptionChanged, this, [this]() {
        scheduleTreeUpdate();
    });

    // Connect to subagent/team events to update tree with nested agents
    connect(session, &ClaudeSession::subagentStarted, this, [this]() {
        scheduleTreeUpdate();
    });
    connect(session, &ClaudeSession::subagentStopped, this, [this]() {
        scheduleTreeUpdate();
    });
    connect(session, &ClaudeSession::teamInfoChanged, this, [this]() {
        scheduleTreeUpdate();
    });

    // Connect to subprocess changes to update tree with running commands
    connect(session, &ClaudeSession::subprocessChanged, this, [this]() {
        scheduleTreeUpdate();
    });
}

void SessionManagerPanel::unregisterSession(ClaudeSession *session)
{
    if (!session) {
        return;
    }

    QString sessionId = session->sessionId();

    // Guard: skip if already unregistered (e.g., archiveSession already removed it)
    if (!m_activeSessions.contains(sessionId)) {
        return;
    }

    // Save yolo mode settings, approval state, resume ID, and subagent/subprocess snapshots
    if (m_metadata.contains(sessionId)) {
        m_metadata[sessionId].yoloMode = session->yoloMode();
        m_metadata[sessionId].doubleYoloMode = session->doubleYoloMode();
        m_metadata[sessionId].tripleYoloMode = session->tripleYoloMode();
        m_metadata[sessionId].yoloApprovalCount = session->yoloApprovalCount();
        m_metadata[sessionId].doubleYoloApprovalCount = session->doubleYoloApprovalCount();
        m_metadata[sessionId].tripleYoloApprovalCount = session->tripleYoloApprovalCount();
        m_metadata[sessionId].approvalLog = session->approvalLog();
        if (!session->resumeSessionId().isEmpty()) {
            m_metadata[sessionId].lastResumeSessionId = session->resumeSessionId();
        }
        if (!session->taskDescription().isEmpty()) {
            m_metadata[sessionId].description = session->taskDescription();
        }
        m_metadata[sessionId].subagents = session->subagents().values().toVector();
        m_metadata[sessionId].subprocesses = session->subprocesses().values().toVector();
        m_metadata[sessionId].promptGroupLabels = session->promptGroupLabels();
        m_metadata[sessionId].currentPromptRound = session->currentPromptRound();
        m_metadata[sessionId].lastAccessed = QDateTime::currentDateTime();
        scheduleMetadataSave();
    }

    m_activeSessions.remove(sessionId);

    // Clean up signal-filtering state maps
    m_lastKnownState.remove(sessionId);
    m_lastKnownApprovalCount.remove(sessionId);
    m_discoveredCacheValid = false;

    updateTreeWidget();
}

QList<SessionMetadata> SessionManagerPanel::allSessions() const
{
    return m_metadata.values();
}

const SessionMetadata *SessionManagerPanel::sessionMetadata(const QString &sessionId) const
{
    auto it = m_metadata.constFind(sessionId);
    return it != m_metadata.constEnd() ? &(*it) : nullptr;
}

bool SessionManagerPanel::isSessionActive(const QString &sessionId) const
{
    return m_activeSessions.contains(sessionId) && m_activeSessions[sessionId];
}

QList<SessionMetadata> SessionManagerPanel::pinnedSessions() const
{
    QList<SessionMetadata> result;
    for (const auto &meta : m_metadata) {
        if (meta.isPinned && !meta.isArchived) {
            result.append(meta);
        }
    }
    return result;
}

QList<SessionMetadata> SessionManagerPanel::archivedSessions() const
{
    QList<SessionMetadata> result;
    for (const auto &meta : m_metadata) {
        if (meta.isArchived) {
            result.append(meta);
        }
    }
    return result;
}

void SessionManagerPanel::cleanupStaleSockets()
{
    // Remove stale socket and yolo files from sessions that no longer have live tmux sessions.
    // Uses async tmux query to avoid blocking the GUI thread.
    QString sessionsDir = ClaudeHookHandler::sessionDataDir() + QStringLiteral("/sessions");
    QDir dir(sessionsDir);
    if (!dir.exists()) {
        return;
    }

    // Collect socket files before the async call (filesystem reads are fast)
    QStringList sockFiles = dir.entryList({QStringLiteral("*.sock")}, QDir::Files);
    if (sockFiles.isEmpty()) {
        return;
    }

    auto *tmux = new TmuxManager(nullptr);
    QPointer<SessionManagerPanel> guard(this);

    tmux->listKonsolaiSessionsAsync([guard, tmux, sessionsDir, sockFiles](const QList<TmuxManager::SessionInfo> &liveSessions) {
        tmux->deleteLater();

        if (!guard) {
            return;
        }

        QSet<QString> liveIds;
        for (const auto &info : liveSessions) {
            // Extract ID from session name: konsolai-{profile}-{id}
            QStringList parts = info.name.split(QLatin1Char('-'));
            if (parts.size() >= 3) {
                liveIds.insert(parts.last());
            }
        }

        // Clean up socket and yolo files for dead sessions
        QDir sessDir(sessionsDir);
        int cleaned = 0;
        for (const QString &sockFile : sockFiles) {
            QString id = sockFile.left(sockFile.length() - 5); // Remove .sock
            if (!liveIds.contains(id)) {
                QFile::remove(sessDir.filePath(sockFile));
                QString yoloFile = id + QStringLiteral(".yolo");
                if (sessDir.exists(yoloFile)) {
                    QFile::remove(sessDir.filePath(yoloFile));
                }
                QString teamYoloFile = id + QStringLiteral(".yolo-team");
                if (sessDir.exists(teamYoloFile)) {
                    QFile::remove(sessDir.filePath(teamYoloFile));
                }
                cleaned++;
            }
        }

        if (cleaned > 0) {
            qDebug() << "SessionManagerPanel: Cleaned up" << cleaned << "stale socket/yolo files";
        }
    });
}

void SessionManagerPanel::refresh()
{
    // Discover tmux sessions that aren't tracked, using async tmux query
    // to avoid blocking the GUI thread.
    if (!m_registry) {
        updateTreeWidget();
        return;
    }

    auto *tmux = new TmuxManager(nullptr);
    QPointer<SessionManagerPanel> guard(this);

    tmux->listKonsolaiSessionsAsync([guard, tmux](const QList<TmuxManager::SessionInfo> &liveSessions) {
        tmux->deleteLater();

        if (!guard) {
            return;
        }

        // Feed pre-fetched tmux data to registry (non-blocking)
        if (guard->m_registry) {
            guard->m_registry->refreshOrphanedSessions(liveSessions);
            for (const auto &state : guard->m_registry->orphanedSessions()) {
                if (!guard->m_metadata.contains(state.sessionId)) {
                    SessionMetadata meta;
                    meta.sessionId = state.sessionId;
                    meta.sessionName = state.sessionName;
                    meta.profileName = state.profileName;
                    meta.workingDirectory = state.workingDirectory;
                    meta.lastAccessed = state.lastAccessed;
                    meta.createdAt = state.created;
                    meta.isArchived = false;
                    meta.isPinned = false;
                    guard->m_metadata[state.sessionId] = meta;
                }
            }
        }

        // Repair stale createdAt timestamps for existing metadata entries.
        // A previous bug recorded the discovery time instead of tmux creation time,
        // causing multiple sessions to share the same timestamp and display name.
        static const QRegularExpression idPattern(QStringLiteral("^konsolai-.+-([a-f0-9]{8})$"));
        for (const TmuxManager::SessionInfo &info : liveSessions) {
            QRegularExpressionMatch idMatch = idPattern.match(info.name);
            if (!idMatch.hasMatch()) {
                continue;
            }
            QString sessionId = idMatch.captured(1);
            if (!guard->m_metadata.contains(sessionId)) {
                continue;
            }
            bool ok = false;
            qint64 epoch = info.created.toLongLong(&ok);
            if (!ok || epoch <= 0) {
                continue;
            }
            QDateTime tmuxCreated = QDateTime::fromSecsSinceEpoch(epoch);
            auto &meta = guard->m_metadata[sessionId];
            // If metadata createdAt is later than tmux creation, it was set from
            // discovery time — replace with the real tmux creation time.
            if (meta.createdAt > tmuxCreated) {
                meta.createdAt = tmuxCreated;
            }
        }

        guard->updateTreeWidget();
    });
}

void SessionManagerPanel::pinSession(const QString &sessionId)
{
    if (m_metadata.contains(sessionId)) {
        m_metadata[sessionId].isPinned = true;
        scheduleMetadataSave();
        rebuildTreeSync(); // Immediate sync rebuild — user explicitly requested this
    }
}

void SessionManagerPanel::unpinSession(const QString &sessionId)
{
    if (m_metadata.contains(sessionId)) {
        m_metadata[sessionId].isPinned = false;
        scheduleMetadataSave();
        rebuildTreeSync(); // Immediate sync rebuild — user explicitly requested this
    }
}

void SessionManagerPanel::archiveSession(const QString &sessionId)
{
    if (!m_metadata.contains(sessionId)) {
        return;
    }

    QString sessionName = m_metadata[sessionId].sessionName;

    // Snapshot live session data into metadata BEFORE removing from active map
    if (m_activeSessions.contains(sessionId)) {
        ClaudeSession *session = m_activeSessions[sessionId];
        if (session) {
            if (!session->resumeSessionId().isEmpty()) {
                m_metadata[sessionId].lastResumeSessionId = session->resumeSessionId();
            }
            m_metadata[sessionId].subagents = session->subagents().values().toVector();
            m_metadata[sessionId].subprocesses = session->subprocesses().values().toVector();
            m_metadata[sessionId].promptGroupLabels = session->promptGroupLabels();
            m_metadata[sessionId].currentPromptRound = session->currentPromptRound();
            disconnect(session, nullptr, this, nullptr);
        }
    }

    // Remove from active sessions
    m_activeSessions.remove(sessionId);

    // Mark as archived
    m_metadata[sessionId].isArchived = true;
    m_metadata[sessionId].lastAccessed = QDateTime::currentDateTime();
    scheduleMetadataSave();

    // Clean up stale socket, yolo, and yolo-team files
    QString socketPath = ClaudeHookHandler::sessionDataDir() + QStringLiteral("/sessions/") + sessionId + QStringLiteral(".sock");
    if (QFile::exists(socketPath)) {
        QFile::remove(socketPath);
    }
    QString yoloPath = ClaudeHookHandler::sessionDataDir() + QStringLiteral("/sessions/") + sessionId + QStringLiteral(".yolo");
    if (QFile::exists(yoloPath)) {
        QFile::remove(yoloPath);
    }
    QString teamYoloPath = ClaudeHookHandler::sessionDataDir() + QStringLiteral("/sessions/") + sessionId + QStringLiteral(".yolo-team");
    if (QFile::exists(teamYoloPath)) {
        QFile::remove(teamYoloPath);
    }

    // Kill the tmux session asynchronously, then update tree after kill completes
    if (sessionName.isEmpty()) {
        qDebug() << "SessionManagerPanel::archiveSession - no session name, skipping tmux kill";
        updateTreeWidget();
    } else {
        if (m_pendingAsyncKills++ == 0) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
        }
        auto *tmux = new TmuxManager(nullptr);
        QPointer<SessionManagerPanel> guard(this);
        tmux->sessionExistsAsync(sessionName, [tmux, sessionName, guard](bool exists) {
            if (exists) {
                tmux->killSessionAsync(sessionName, [tmux, guard](bool) {
                    tmux->deleteLater();
                    if (guard) {
                        if (--guard->m_pendingAsyncKills == 0) {
                            QApplication::restoreOverrideCursor();
                        }
                        guard->updateTreeWidget();
                    } else {
                        QApplication::restoreOverrideCursor();
                    }
                });
            } else {
                tmux->deleteLater();
                if (guard) {
                    if (--guard->m_pendingAsyncKills == 0) {
                        QApplication::restoreOverrideCursor();
                    }
                    guard->updateTreeWidget();
                } else {
                    QApplication::restoreOverrideCursor();
                }
            }
        });
    }
}

void SessionManagerPanel::closeSession(const QString &sessionId)
{
    if (!m_metadata.contains(sessionId)) {
        return;
    }

    QString sessionName = m_metadata[sessionId].sessionName;

    // Snapshot live session data into metadata BEFORE removing from active map
    if (m_activeSessions.contains(sessionId)) {
        ClaudeSession *session = m_activeSessions[sessionId];
        if (session) {
            if (!session->resumeSessionId().isEmpty()) {
                m_metadata[sessionId].lastResumeSessionId = session->resumeSessionId();
            }
            m_metadata[sessionId].subagents = session->subagents().values().toVector();
            m_metadata[sessionId].subprocesses = session->subprocesses().values().toVector();
            m_metadata[sessionId].promptGroupLabels = session->promptGroupLabels();
            m_metadata[sessionId].currentPromptRound = session->currentPromptRound();
            disconnect(session, nullptr, this, nullptr);
        }
    }

    // Remove from active sessions
    m_activeSessions.remove(sessionId);

    // Mark as explicitly closed so tree categorization skips the tmux-alive check
    // (tmux kill is async and may not have finished by tree update time)
    m_explicitlyClosed.insert(sessionId);

    // Update last accessed but do NOT mark as archived — it will appear in Closed
    m_metadata[sessionId].lastAccessed = QDateTime::currentDateTime();
    scheduleMetadataSave();

    // Clean up stale socket, yolo, and yolo-team files
    QString socketPath = ClaudeHookHandler::sessionDataDir() + QStringLiteral("/sessions/") + sessionId + QStringLiteral(".sock");
    if (QFile::exists(socketPath)) {
        QFile::remove(socketPath);
    }
    QString yoloPath = ClaudeHookHandler::sessionDataDir() + QStringLiteral("/sessions/") + sessionId + QStringLiteral(".yolo");
    if (QFile::exists(yoloPath)) {
        QFile::remove(yoloPath);
    }
    QString teamYoloPath = ClaudeHookHandler::sessionDataDir() + QStringLiteral("/sessions/") + sessionId + QStringLiteral(".yolo-team");
    if (QFile::exists(teamYoloPath)) {
        QFile::remove(teamYoloPath);
    }

    // Kill the tmux session asynchronously, then update tree AFTER kill completes.
    // This avoids a race where the tree queries tmux before the kill finishes,
    // causing the session to appear "Detached" instead of "Closed".
    if (sessionName.isEmpty()) {
        qDebug() << "SessionManagerPanel::closeSession - no session name, skipping tmux kill";
        updateTreeWidget();
    } else {
        if (m_pendingAsyncKills++ == 0) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
        }
        auto *tmux = new TmuxManager(nullptr);
        QPointer<SessionManagerPanel> guard(this);
        tmux->sessionExistsAsync(sessionName, [tmux, sessionName, guard](bool exists) {
            if (exists) {
                tmux->killSessionAsync(sessionName, [tmux, guard](bool) {
                    tmux->deleteLater();
                    if (guard) {
                        if (--guard->m_pendingAsyncKills == 0) {
                            QApplication::restoreOverrideCursor();
                        }
                        guard->updateTreeWidget();
                    } else {
                        QApplication::restoreOverrideCursor();
                    }
                });
            } else {
                tmux->deleteLater();
                if (guard) {
                    if (--guard->m_pendingAsyncKills == 0) {
                        QApplication::restoreOverrideCursor();
                    }
                    guard->updateTreeWidget();
                } else {
                    QApplication::restoreOverrideCursor();
                }
            }
        });
    }
}

void SessionManagerPanel::unarchiveSession(const QString &sessionId)
{
    if (!m_metadata.contains(sessionId)) {
        return;
    }

    const auto &meta = m_metadata[sessionId];

    // Emit signal to create new session with same ID (including remote SSH fields)
    Q_EMIT unarchiveRequested(sessionId, meta.workingDirectory, meta.isRemote, meta.sshHost, meta.sshUsername, meta.sshPort);
}

void SessionManagerPanel::markExpired(const QString &sessionName)
{
    // Find metadata by session name, falling back to sessionId lookup
    auto it = m_metadata.end();

    // Try direct sessionId lookup first (caller may pass sessionId)
    if (m_metadata.contains(sessionName)) {
        it = m_metadata.find(sessionName);
    } else {
        // Linear search by sessionName
        for (auto search = m_metadata.begin(); search != m_metadata.end(); ++search) {
            if (search->sessionName == sessionName) {
                it = search;
                break;
            }
        }
    }

    if (it != m_metadata.end()) {
        // Mark as expired but NOT archived - let it go to Closed category
        // User must explicitly archive if they don't want to see it
        it->isExpired = true;
        it->lastAccessed = QDateTime::currentDateTime();
        m_activeSessions.remove(it->sessionId);
        scheduleMetadataSave();
        updateTreeWidget();
        qDebug() << "SessionManagerPanel: Marked session as expired (tmux dead):" << sessionName;
    } else {
        qDebug() << "SessionManagerPanel: Could not find session to mark expired:" << sessionName;
    }
}

void SessionManagerPanel::autoArchiveOldClosedSessions()
{
    // Auto-archive sessions in "Closed" category (isExpired && !isArchived) that
    // have been closed for more than 7 days. Never touches Active, Detached, or Pinned.
    const int thresholdDays = 7;
    const QDateTime now = QDateTime::currentDateTime();
    int archived = 0;

    for (auto &meta : m_metadata) {
        // Only target closed (expired) sessions that aren't already archived/dismissed/pinned
        if (!meta.isExpired || meta.isArchived || meta.isDismissed || meta.isPinned) {
            continue;
        }
        // Don't touch sessions with active ClaudeSession objects
        if (m_activeSessions.contains(meta.sessionId)) {
            continue;
        }
        // Check age: lastAccessed must be > threshold days ago
        if (meta.lastAccessed.isValid() && meta.lastAccessed.daysTo(now) > thresholdDays) {
            meta.isArchived = true;
            ++archived;
            qDebug() << "SessionManagerPanel: Auto-archived closed session:" << meta.sessionId << "last accessed:" << meta.lastAccessed.toString(Qt::ISODate);
        }
    }

    if (archived > 0) {
        scheduleMetadataSave();
        scheduleTreeUpdate();
        qDebug() << "SessionManagerPanel: Auto-archived" << archived << "closed sessions older than" << thresholdDays << "days";
    }
}

QString SessionManagerPanel::sessionIdForName(const QString &sessionName) const
{
    // Try direct sessionId lookup first (caller may pass sessionId)
    if (m_metadata.contains(sessionName)) {
        return sessionName;
    }
    // Linear search by sessionName
    for (auto it = m_metadata.constBegin(); it != m_metadata.constEnd(); ++it) {
        if (it->sessionName == sessionName) {
            return it->sessionId;
        }
    }
    return {};
}

void SessionManagerPanel::dismissSession(const QString &sessionId)
{
    if (!m_metadata.contains(sessionId)) {
        return;
    }

    m_metadata[sessionId].isDismissed = true;
    m_metadata[sessionId].lastAccessed = QDateTime::currentDateTime();
    scheduleMetadataSave();
    updateTreeWidget();
    qDebug() << "SessionManagerPanel: Dismissed session:" << sessionId;
}

void SessionManagerPanel::restoreSession(const QString &sessionId)
{
    if (!m_metadata.contains(sessionId)) {
        return;
    }

    m_metadata[sessionId].isDismissed = false;
    m_metadata[sessionId].isArchived = true; // Restore to Archived state
    m_metadata[sessionId].lastAccessed = QDateTime::currentDateTime();
    scheduleMetadataSave();
    updateTreeWidget();
    qDebug() << "SessionManagerPanel: Restored dismissed session:" << sessionId;
}

void SessionManagerPanel::purgeSession(const QString &sessionId)
{
    if (!m_metadata.contains(sessionId)) {
        return;
    }

    // Also remove from the registry
    QString sessionName = m_metadata[sessionId].sessionName;
    if (m_registry && !sessionName.isEmpty()) {
        m_registry->removeSessionState(sessionName);
    }

    m_metadata.remove(sessionId);
    scheduleMetadataSave();
    updateTreeWidget();
    qDebug() << "SessionManagerPanel: Purged session metadata:" << sessionId;
}

void SessionManagerPanel::purgeDismissed()
{
    QStringList toRemove;
    for (auto it = m_metadata.constBegin(); it != m_metadata.constEnd(); ++it) {
        if (it->isDismissed) {
            toRemove.append(it.key());
        }
    }

    for (const QString &sessionId : toRemove) {
        QString sessionName = m_metadata[sessionId].sessionName;
        if (m_registry && !sessionName.isEmpty()) {
            m_registry->removeSessionState(sessionName);
        }
        m_metadata.remove(sessionId);
    }

    if (!toRemove.isEmpty()) {
        scheduleMetadataSave();
        updateTreeWidget();
        qDebug() << "SessionManagerPanel: Purged" << toRemove.size() << "dismissed sessions";
    }
}

void SessionManagerPanel::pauseBackgroundTimers()
{
    if (m_timersPaused) {
        return;
    }
    m_timersPaused = true;

    // Stop periodic timers
    if (m_remoteTmuxTimer) {
        m_remoteTmuxTimer->stop();
    }
    if (m_gitCacheTimer) {
        m_gitCacheTimer->stop();
    }
    if (m_convCacheTimer) {
        m_convCacheTimer->stop();
    }
    if (m_durationTimer) {
        m_durationTimer->stop();
    }
    if (m_autoArchiveTimer) {
        m_autoArchiveTimer->stop();
    }

    // Stop debounce timers — no point rebuilding tree or saving metadata
    // while nobody is looking. Deferred work flushes on resume.
    if (m_updateDebounce && m_updateDebounce->isActive()) {
        m_updateDebounce->stop();
        m_pendingUpdate = true;
    }
    if (m_saveDebounce && m_saveDebounce->isActive()) {
        m_saveDebounce->stop();
        m_pendingSave = true;
    }

    // Pause display timers on each active session
    for (auto it = m_activeSessions.constBegin(); it != m_activeSessions.constEnd(); ++it) {
        if (auto *session = it.value().data()) {
            session->pauseDisplayTimers();
        }
    }

    qDebug() << "SessionManagerPanel: Paused background timers (window inactive)";
}

void SessionManagerPanel::resumeBackgroundTimers()
{
    if (!m_timersPaused) {
        return;
    }
    m_timersPaused = false;

    // Restart periodic timers
    if (m_remoteTmuxTimer) {
        m_remoteTmuxTimer->start();
    }
    if (m_gitCacheTimer) {
        m_gitCacheTimer->start();
    }
    if (m_convCacheTimer) {
        m_convCacheTimer->start();
    }
    if (m_autoArchiveTimer) {
        m_autoArchiveTimer->start();
    }
    // m_durationTimer is started/stopped by updateTreeWidget based on active subagents,
    // so just let the tree rebuild handle it.

    // Resume display timers on each active session
    for (auto it = m_activeSessions.constBegin(); it != m_activeSessions.constEnd(); ++it) {
        if (auto *session = it.value().data()) {
            session->resumeDisplayTimers();
        }
    }

    // Flush deferred metadata save first (so tree sees current data)
    if (m_pendingSave) {
        m_pendingSave = false;
        scheduleMetadataSave();
    }

    // Schedule a gentle tree refresh to pick up changes that occurred while paused
    scheduleTreeUpdate();

    qDebug() << "SessionManagerPanel: Resumed background timers (window active)";
}

void SessionManagerPanel::onItemDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column)

    if (!item || item == m_pinnedCategory || item == m_activeCategory || item == m_archivedCategory || item == m_closedCategory || item == m_dismissedCategory
        || item == m_discoveredCategory) {
        return;
    }

    // Folder header — toggle expand/collapse
    if (!item->data(0, Qt::UserRole + 7).toString().isEmpty() && item->parent() == nullptr) {
        item->setExpanded(!item->isExpanded());
        return;
    }

    // Check if this is a subagent/subprocess child item (parent is a session item, not a category)
    QTreeWidgetItem *parentItem = item->parent();
    if (parentItem && parentItem->parent() != nullptr) {
        // Check if this is a prompt group item (toggle expand/collapse)
        QVariant promptGroupVar = item->data(0, Qt::UserRole + 3);
        if (promptGroupVar.isValid() && !promptGroupVar.isNull()) {
            item->setExpanded(!item->isExpanded());
            return;
        }

        // Check if this is a task group item (toggle expand/collapse)
        if (!item->data(0, Qt::UserRole + 2).toString().isEmpty()) {
            item->setExpanded(!item->isExpanded());
            return;
        }

        // Check if this is a subprocess item
        QString subprocessId = item->data(0, Qt::UserRole + 4).toString();
        if (!subprocessId.isEmpty()) {
            QString parentSessionId = item->data(0, Qt::UserRole + 1).toString();
            if (m_activeSessions.contains(parentSessionId)) {
                ClaudeSession *session = m_activeSessions[parentSessionId];
                if (session) {
                    const auto &procs = session->subprocesses();
                    if (procs.contains(subprocessId)) {
                        showSubprocessOutput(procs[subprocessId]);
                    }
                }
            }
            return;
        }

        // Subagent child item (depth 2 or 3)
        QString agentId = item->data(0, Qt::UserRole).toString();
        QString parentSessionId = item->data(0, Qt::UserRole + 1).toString();
        if (!agentId.isEmpty() && m_activeSessions.contains(parentSessionId)) {
            ClaudeSession *session = m_activeSessions[parentSessionId];
            if (session) {
                const auto &agents = session->subagents();
                if (agents.contains(agentId)) {
                    const auto &info = agents[agentId];
                    if (!info.transcriptPath.isEmpty() && QFile::exists(info.transcriptPath)) {
                        showSubagentTranscript(info);
                    } else {
                        showSubagentDetails(info);
                    }
                }
            }
        }
        return;
    }

    QString sessionId = item->data(0, Qt::UserRole).toString();
    if (sessionId.isEmpty()) {
        return;
    }

    // Check if this is a discovered session (parent is m_discoveredCategory)
    if (item->parent() == m_discoveredCategory) {
        QString workDir = item->data(0, Qt::UserRole + 1).toString();
        if (workDir.isEmpty()) {
            return;
        }
        // Check if this is a remote discovered item
        QString remoteHost = item->data(0, Qt::UserRole + 2).toString();
        bool isRemoteItem = !remoteHost.isEmpty();
        QString remoteUser = isRemoteItem ? item->data(0, Qt::UserRole + 3).toString() : QString();
        int remotePort = isRemoteItem ? item->data(0, Qt::UserRole + 4).toInt() : 22;
        if (remotePort <= 0) {
            remotePort = 22;
        }

        // Check for existing conversations — offer resume before creating new
        if (!isRemoteItem) {
            if (!m_conversationCache.contains(workDir)) {
                m_conversationCache.insert(workDir, ClaudeSessionRegistry::readClaudeConversations(workDir));
            }
            const auto &conversations = m_conversationCache[workDir];
            if (!conversations.isEmpty()) {
                QString id = ClaudeConversationPicker::pick(conversations, this);
                if (!id.isEmpty()) {
                    Q_EMIT resumeConversationRequested(workDir, id, QString(), QString(), 22);
                    return;
                }
                // User chose "Start Fresh" — fall through to create new
            }
        }

        if (isRemoteItem) {
            Q_EMIT remoteSessionRequested(remoteHost, remoteUser, remotePort, workDir);
        } else {
            Q_EMIT unarchiveRequested(sessionId, workDir, false, QString(), QString(), 22);
        }
        return;
    }

    if (!m_metadata.contains(sessionId)) {
        return;
    }

    const auto &meta = m_metadata[sessionId];

    if (meta.isArchived) {
        // Unarchive and attach
        unarchiveSession(sessionId);
    } else if (m_activeSessions.contains(sessionId)) {
        // Active session — focus its tab
        ClaudeSession *session = m_activeSessions[sessionId];
        if (session) {
            Q_EMIT focusSessionRequested(session);
        }
    } else if (item->parent() == m_closedCategory || meta.isExpired) {
        // Closed session — tmux is dead, recreate like unarchive
        unarchiveSession(sessionId);
    } else {
        // Detached session — reattach (remote or local)
        if (meta.isRemote) {
            Q_EMIT remoteAttachRequested(meta.sshHost, meta.sshUsername, meta.sshPort, meta.workingDirectory, meta.sessionName);
        } else {
            Q_EMIT attachRequested(meta.sessionName);
        }
    }
}

void SessionManagerPanel::onContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_treeWidget->itemAt(pos);
    if (!item) {
        return;
    }

    // Category header context menus — bulk actions
    if (item == m_pinnedCategory || item == m_activeCategory || item == m_detachedCategory
        || item == m_closedCategory || item == m_archivedCategory || item == m_dismissedCategory
        || item == m_discoveredCategory) {
        if (item->childCount() == 0) {
            return; // No children, no actions
        }
        QMenu menu(this);

        // Collect child session IDs for this category
        auto collectChildIds = [](QTreeWidgetItem *category) {
            QStringList ids;
            for (int i = 0; i < category->childCount(); ++i) {
                QString id = category->child(i)->data(0, Qt::UserRole).toString();
                if (!id.isEmpty()) {
                    ids << id;
                }
            }
            return ids;
        };

        // Identify which category this is, so lambdas use stable member pointers
        QTreeWidgetItem *categoryPtr = item; // item == one of our m_*Category members (never deleted)

        if (categoryPtr == m_detachedCategory) {
            QAction *attachAll = menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Attach All (%1)", categoryPtr->childCount()));
            connect(attachAll, &QAction::triggered, this, [this, collectChildIds]() {
                QStringList ids = collectChildIds(m_detachedCategory);
                if (ids.isEmpty()) return;
                int ret = QMessageBox::question(this, i18n("Attach All"),
                    i18n("Attach all %1 detached sessions?", ids.size()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (ret != QMessageBox::Yes) return;
                for (const auto &id : std::as_const(ids)) {
                    auto *meta = findMetadata(id);
                    if (!meta) continue;
                    if (meta->isRemote) {
                        Q_EMIT remoteAttachRequested(meta->sshHost, meta->sshUsername, meta->sshPort, meta->workingDirectory, meta->sessionName);
                    } else {
                        Q_EMIT attachRequested(meta->sessionName);
                    }
                }
            });
            menu.addSeparator();
        }

        if (categoryPtr == m_detachedCategory || categoryPtr == m_closedCategory || categoryPtr == m_pinnedCategory) {
            QAction *archiveAll = menu.addAction(QIcon::fromTheme(QStringLiteral("archive-insert")), i18n("Archive All (%1)", categoryPtr->childCount()));
            // Capture the member pointer directly — category headers are never deleted during tree rebuilds
            QTreeWidgetItem **categoryMember = (categoryPtr == m_detachedCategory) ? &m_detachedCategory
                                             : (categoryPtr == m_closedCategory)   ? &m_closedCategory
                                                                                   : &m_pinnedCategory;
            connect(archiveAll, &QAction::triggered, this, [this, categoryMember, collectChildIds]() {
                QStringList ids = collectChildIds(*categoryMember);
                if (ids.isEmpty()) return;
                int ret = QMessageBox::question(this, i18n("Archive All"),
                    i18n("Archive all %1 sessions in this category?", ids.size()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (ret != QMessageBox::Yes) return;
                for (const auto &id : std::as_const(ids)) {
                    archiveSession(id);
                }
            });
        }

        if (categoryPtr == m_detachedCategory) {
            menu.addSeparator();
            QAction *closeAll = menu.addAction(QIcon::fromTheme(QStringLiteral("window-close")), i18n("Close All (%1)", categoryPtr->childCount()));
            connect(closeAll, &QAction::triggered, this, [this, collectChildIds]() {
                QStringList ids = collectChildIds(m_detachedCategory);
                if (ids.isEmpty()) return;
                int ret = QMessageBox::question(this, i18n("Close All"),
                    i18n("Close all %1 detached sessions? This kills their tmux backends.", ids.size()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (ret != QMessageBox::Yes) return;
                for (const auto &id : std::as_const(ids)) {
                    closeSession(id);
                }
            });
        }

        if (categoryPtr == m_archivedCategory) {
            QAction *dismissAll = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Dismiss All (%1)", categoryPtr->childCount()));
            connect(dismissAll, &QAction::triggered, this, [this, collectChildIds]() {
                QStringList ids = collectChildIds(m_archivedCategory);
                if (ids.isEmpty()) return;
                int ret = QMessageBox::question(this, i18n("Dismiss All"),
                    i18n("Dismiss all %1 archived sessions? They can be restored later.", ids.size()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (ret != QMessageBox::Yes) return;
                for (const auto &id : std::as_const(ids)) {
                    dismissSession(id);
                }
            });

            menu.addSeparator();

            // Age-based dismiss options
            auto dismissOlderThan = [this, collectChildIds](int days, const QString &label) {
                QStringList ids = collectChildIds(m_archivedCategory);
                QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);
                QStringList old;
                for (const auto &id : std::as_const(ids)) {
                    auto *meta = findMetadata(id);
                    if (meta && meta->lastAccessed.isValid() && meta->lastAccessed < cutoff) {
                        old << id;
                    }
                }
                if (old.isEmpty()) {
                    QMessageBox::information(this, i18n("Nothing to Dismiss"),
                        i18n("No archived sessions older than %1.", label));
                    return;
                }
                int ret = QMessageBox::question(this, i18n("Dismiss Old Sessions"),
                    i18n("Dismiss %1 archived sessions older than %2?", old.size(), label),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (ret != QMessageBox::Yes) return;
                for (const auto &id : std::as_const(old)) {
                    dismissSession(id);
                }
            };

            QAction *dismiss1w = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Dismiss > 1 Week Old"));
            connect(dismiss1w, &QAction::triggered, this, [dismissOlderThan]() {
                dismissOlderThan(7, i18n("1 week"));
            });

            QAction *dismiss1m = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Dismiss > 1 Month Old"));
            connect(dismiss1m, &QAction::triggered, this, [dismissOlderThan]() {
                dismissOlderThan(30, i18n("1 month"));
            });
        }

        if (categoryPtr == m_dismissedCategory) {
            QAction *purgeAll = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete-remove")), i18n("Purge All (%1)", categoryPtr->childCount()));
            connect(purgeAll, &QAction::triggered, this, [this]() {
                int count = m_dismissedCategory->childCount();
                int ret = QMessageBox::question(this, i18n("Purge All Dismissed"),
                    i18n("Permanently delete metadata for all %1 dismissed sessions? This cannot be undone.", count),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (ret != QMessageBox::Yes) return;
                purgeDismissed();
            });

            QAction *restoreAll = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-undo")), i18n("Restore All (%1)", categoryPtr->childCount()));
            connect(restoreAll, &QAction::triggered, this, [this, collectChildIds]() {
                QStringList ids = collectChildIds(m_dismissedCategory);
                if (ids.isEmpty()) return;
                int ret = QMessageBox::question(this, i18n("Restore All"),
                    i18n("Restore all %1 dismissed sessions back to Archived?", ids.size()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (ret != QMessageBox::Yes) return;
                for (const auto &id : std::as_const(ids)) {
                    restoreSession(id);
                }
            });
        }

        if (!menu.isEmpty()) {
            menu.exec(m_treeWidget->viewport()->mapToGlobal(pos));
        }
        return;
    }

    // Folder header context menu
    {
        QString folderId = item->data(0, Qt::UserRole + 7).toString();
        if (!folderId.isEmpty() && m_folders.contains(folderId) && item->parent() == nullptr) {
            QMenu menu(this);

            QAction *renameAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18n("Rename Folder..."));
            connect(renameAction, &QAction::triggered, this, [this, folderId]() {
                bool ok;
                QString current = m_folders[folderId].name;
                QString newName = QInputDialog::getText(this, i18n("Rename Folder"),
                    i18n("Folder name:"), QLineEdit::Normal, current, &ok);
                if (ok && !newName.isEmpty()) {
                    renameFolder(folderId, newName);
                }
            });

            QAction *colorAction = menu.addAction(QIcon::fromTheme(QStringLiteral("color-picker")), i18n("Change Color..."));
            connect(colorAction, &QAction::triggered, this, [this, folderId]() {
                QColor current = m_folders[folderId].color;
                QColor newColor = QColorDialog::getColor(current, this, i18n("Folder Color"));
                if (newColor.isValid()) {
                    setFolderColor(folderId, newColor);
                }
            });

            menu.addSeparator();

            QAction *deleteAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Delete Folder"));
            connect(deleteAction, &QAction::triggered, this, [this, folderId]() {
                int count = m_folderTreeItems.contains(folderId) ? m_folderTreeItems[folderId]->childCount() : 0;
                auto answer = QMessageBox::question(this, i18n("Delete Folder"),
                    i18n("Delete folder '%1'? Its %2 sessions will become ungrouped.",
                         m_folders[folderId].name, count),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (answer == QMessageBox::Yes) {
                    deleteFolder(folderId);
                }
            });

            menu.exec(m_treeWidget->viewport()->mapToGlobal(pos));
            return;
        }
    }

    // Handle items deeper than direct children of categories (subagents, subprocesses, task/prompt groups)
    QTreeWidgetItem *parentItem = item->parent();
    if (parentItem && parentItem->parent() != nullptr) {
        // Check if this is a prompt group item (UserRole + 3)
        QVariant promptGroupVar = item->data(0, Qt::UserRole + 3);
        if (promptGroupVar.isValid() && !promptGroupVar.isNull()) {
            QMenu menu(this);
            QAction *expandAll = menu.addAction(QIcon::fromTheme(QStringLiteral("view-list-tree")), i18n("Expand All"));
            connect(expandAll, &QAction::triggered, this, [item]() {
                item->setExpanded(true);
                for (int i = 0; i < item->childCount(); ++i) {
                    item->child(i)->setExpanded(true);
                }
            });
            QAction *collapseAll = menu.addAction(QIcon::fromTheme(QStringLiteral("view-list-text")), i18n("Collapse"));
            connect(collapseAll, &QAction::triggered, this, [item]() {
                item->setExpanded(false);
            });
            menu.exec(m_treeWidget->viewport()->mapToGlobal(pos));
            return;
        }

        // Check if this is a task group item (UserRole + 2)
        QString taskGroupDesc = item->data(0, Qt::UserRole + 2).toString();
        if (!taskGroupDesc.isEmpty()) {
            QMenu menu(this);
            QAction *expandAll = menu.addAction(QIcon::fromTheme(QStringLiteral("view-list-tree")), i18n("Expand All"));
            connect(expandAll, &QAction::triggered, this, [item]() {
                item->setExpanded(true);
                for (int i = 0; i < item->childCount(); ++i) {
                    item->child(i)->setExpanded(true);
                }
            });
            QAction *collapseAll = menu.addAction(QIcon::fromTheme(QStringLiteral("view-list-text")), i18n("Collapse"));
            connect(collapseAll, &QAction::triggered, this, [item]() {
                item->setExpanded(false);
            });
            menu.addSeparator();
            QAction *copyDesc = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Task Description"));
            connect(copyDesc, &QAction::triggered, this, [taskGroupDesc]() {
                QApplication::clipboard()->setText(taskGroupDesc);
            });
            menu.exec(m_treeWidget->viewport()->mapToGlobal(pos));
            return;
        }

        // Check if this is a subprocess item (UserRole + 4)
        QString subprocessId = item->data(0, Qt::UserRole + 4).toString();
        if (!subprocessId.isEmpty()) {
            QString parentSessionId = item->data(0, Qt::UserRole + 1).toString();
            if (!m_activeSessions.contains(parentSessionId)) {
                return;
            }
            QPointer<ClaudeSession> session = m_activeSessions[parentSessionId];
            if (!session) {
                return;
            }
            const auto &procs = session->subprocesses();
            if (!procs.contains(subprocessId)) {
                return;
            }
            const auto &procInfo = procs[subprocessId];

            QMenu menu(this);

            QAction *viewOutput = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("View Output"));
            viewOutput->setEnabled(!procInfo.output.isEmpty() || procInfo.status != SubprocessInfo::Running);
            connect(viewOutput, &QAction::triggered, this, [this, procInfo]() {
                showSubprocessOutput(procInfo);
            });

            if (procInfo.status == SubprocessInfo::Running) {
                menu.addSeparator();
                QAction *killAction = menu.addAction(QIcon::fromTheme(QStringLiteral("process-stop")), i18n("Kill (SIGTERM)"));
                connect(killAction, &QAction::triggered, this, [session, subprocessId]() {
                    if (session) {
                        session->killSubprocess(subprocessId, 15); // SIGTERM
                    }
                });
                QAction *forceKillAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Force Kill (SIGKILL)"));
                connect(forceKillAction, &QAction::triggered, this, [session, subprocessId]() {
                    if (session) {
                        session->killSubprocess(subprocessId, 9); // SIGKILL
                    }
                });
            }

            menu.addSeparator();
            QAction *copyCmd = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Command"));
            connect(copyCmd, &QAction::triggered, this, [procInfo]() {
                QApplication::clipboard()->setText(procInfo.fullCommand);
            });

            menu.exec(m_treeWidget->viewport()->mapToGlobal(pos));
            return;
        }

        // Subagent child item
        QString agentId = item->data(0, Qt::UserRole).toString();
        QString parentSessionId = item->data(0, Qt::UserRole + 1).toString();
        if (agentId.isEmpty() || !m_activeSessions.contains(parentSessionId)) {
            return;
        }
        ClaudeSession *session = m_activeSessions[parentSessionId];
        if (!session) {
            return;
        }
        const auto &agents = session->subagents();
        if (!agents.contains(agentId)) {
            return;
        }
        const auto &info = agents[agentId];

        QMenu menu(this);

        bool hasTranscript = !info.transcriptPath.isEmpty() && QFile::exists(info.transcriptPath);

        QAction *viewTranscript = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("View Transcript"));
        viewTranscript->setEnabled(hasTranscript);
        connect(viewTranscript, &QAction::triggered, this, [this, info]() {
            showSubagentTranscript(info);
        });

        QAction *openExternal = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Open Transcript in Editor"));
        openExternal->setEnabled(hasTranscript);
        connect(openExternal, &QAction::triggered, this, [info]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(info.transcriptPath));
        });

        menu.addSeparator();

        QAction *copyId = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Agent ID"));
        connect(copyId, &QAction::triggered, this, [info]() {
            QApplication::clipboard()->setText(info.agentId);
        });

        QAction *details = menu.addAction(QIcon::fromTheme(QStringLiteral("dialog-information")), i18n("Show Details"));
        connect(details, &QAction::triggered, this, [this, info]() {
            showSubagentDetails(info);
        });

        menu.exec(m_treeWidget->viewport()->mapToGlobal(pos));
        return;
    }

    // Handle right-click on the Closed category header
    if (item == m_closedCategory) {
        int closedCount = m_closedCategory->childCount();
        if (closedCount > 0) {
            QMenu menu(this);
            QAction *archiveAllAction = menu.addAction(QIcon::fromTheme(QStringLiteral("archive-insert")), i18n("Archive All Closed (%1)", closedCount));
            connect(archiveAllAction, &QAction::triggered, this, [this, closedCount]() {
                auto answer = QMessageBox::question(this,
                                                    i18n("Archive Closed Sessions"),
                                                    i18n("Archive %1 closed session(s)?\n\n"
                                                         "They will be moved to the Archived category.",
                                                         closedCount),
                                                    QMessageBox::Yes | QMessageBox::No,
                                                    QMessageBox::No);
                if (answer == QMessageBox::Yes) {
                    // Collect session IDs first (archiveSession modifies the tree)
                    QStringList toArchive;
                    for (int i = 0; i < m_closedCategory->childCount(); ++i) {
                        auto *child = m_closedCategory->child(i);
                        if (!child) {
                            continue;
                        }
                        QString sid = child->data(0, Qt::UserRole).toString();
                        if (!sid.isEmpty()) {
                            toArchive.append(sid);
                        }
                    }
                    for (const auto &sid : toArchive) {
                        archiveSession(sid);
                    }
                }
            });
            menu.exec(m_treeWidget->viewport()->mapToGlobal(pos));
        }
        return;
    }

    // Handle right-click on the Dismissed category header
    if (item == m_dismissedCategory) {
        // Count dismissed sessions
        int dismissedCount = 0;
        for (const auto &meta : m_metadata) {
            if (meta.isDismissed) {
                dismissedCount++;
            }
        }
        if (dismissedCount > 0) {
            QMenu menu(this);
            QAction *purgeAllAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Purge All Dismissed (%1)", dismissedCount));
            connect(purgeAllAction, &QAction::triggered, this, [this, dismissedCount]() {
                auto answer = QMessageBox::question(this,
                                                    i18n("Purge Dismissed Sessions"),
                                                    i18n("Permanently remove metadata for %1 dismissed session(s)?\n\n"
                                                         "Project folders will NOT be affected.",
                                                         dismissedCount),
                                                    QMessageBox::Yes | QMessageBox::No,
                                                    QMessageBox::No);
                if (answer == QMessageBox::Yes) {
                    purgeDismissed();
                }
            });
            menu.exec(m_treeWidget->viewport()->mapToGlobal(pos));
        }
        return;
    }

    QString sessionId = item->data(0, Qt::UserRole).toString();
    if (sessionId.isEmpty()) {
        return;
    }

    // Handle discovered sessions
    if (item->parent() == m_discoveredCategory) {
        QString workDir = item->data(0, Qt::UserRole + 1).toString();
        if (workDir.isEmpty()) {
            return;
        }

        QString remoteHost = item->data(0, Qt::UserRole + 2).toString();
        bool isRemoteItem = !remoteHost.isEmpty();

        QMenu menu(this);

        // Resume Conversation action (for items with conversations)
        if (!isRemoteItem) {
            if (!m_conversationCache.contains(workDir)) {
                m_conversationCache.insert(workDir, ClaudeSessionRegistry::readClaudeConversations(workDir));
            }
            const auto &conversations = m_conversationCache[workDir];
            if (!conversations.isEmpty()) {
                QAction *resumeAction = menu.addAction(
                    QIcon::fromTheme(QStringLiteral("media-playback-start")),
                    i18n("Resume Conversation (%1)...", conversations.size()));
                connect(resumeAction, &QAction::triggered, this, [this, workDir, conversations]() {
                    QString id = ClaudeConversationPicker::pick(conversations, this);
                    if (!id.isEmpty()) {
                        Q_EMIT resumeConversationRequested(workDir, id, QString(), QString(), 22);
                    }
                });
                menu.addSeparator();
            }
        }

        QAction *openAction = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("Open New Session Here"));
        connect(openAction, &QAction::triggered, this, [this, sessionId, workDir, isRemoteItem, item]() {
            if (isRemoteItem) {
                QString host = item->data(0, Qt::UserRole + 2).toString();
                QString user = item->data(0, Qt::UserRole + 3).toString();
                int port = item->data(0, Qt::UserRole + 4).toInt();
                if (port <= 0) {
                    port = 22;
                }
                Q_EMIT remoteSessionRequested(host, user, port, workDir);
            } else {
                Q_EMIT unarchiveRequested(sessionId, workDir, false, QString(), QString(), 22);
            }
        });

        menu.addSeparator();

        QAction *trashAction = menu.addAction(QIcon::fromTheme(QStringLiteral("user-trash")), i18n("Move to Trash..."));
        connect(trashAction, &QAction::triggered, this, [this, workDir]() {
            auto answer = QMessageBox::question(this,
                                                i18n("Move to Trash"),
                                                i18n("Move this project folder to the trash?\n\n%1", workDir),
                                                QMessageBox::Yes | QMessageBox::No,
                                                QMessageBox::No);
            if (answer == QMessageBox::Yes) {
                QFile dir(workDir);
                if (dir.moveToTrash()) {
                    updateTreeWidget();
                } else {
                    QMessageBox::warning(this, i18n("Trash Failed"), i18n("Could not move folder to trash:\n%1", workDir));
                }
            }
        });

        menu.exec(m_treeWidget->viewport()->mapToGlobal(pos));
        return;
    }

    if (!m_metadata.contains(sessionId)) {
        return;
    }

    const auto &meta = m_metadata[sessionId];

    QMenu menu(this);

    if (meta.isDismissed) {
        // Collect all selected dismissed session IDs for batch operations
        QStringList selectedDismissed;
        const auto selectedItemsDismissed = m_treeWidget->selectedItems();
        for (auto *sel : selectedItemsDismissed) {
            if (sel->parent() == m_dismissedCategory) {
                QString sid = sel->data(0, Qt::UserRole).toString();
                if (!sid.isEmpty() && m_metadata.contains(sid) && m_metadata[sid].isDismissed) {
                    selectedDismissed.append(sid);
                }
            }
        }
        if (!selectedDismissed.contains(sessionId)) {
            selectedDismissed.append(sessionId);
        }

        int selectedCount = selectedDismissed.size();

        if (selectedCount > 1) {
            // Multi-selection batch menu
            QAction *restoreAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-undo")), i18n("Restore Selected (%1)", selectedCount));
            connect(restoreAction, &QAction::triggered, this, [this, selectedDismissed]() {
                for (const auto &sid : selectedDismissed) {
                    restoreSession(sid);
                }
            });

            menu.addSeparator();

            QAction *purgeAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Purge Selected (%1)", selectedCount));
            connect(purgeAction, &QAction::triggered, this, [this, selectedDismissed, selectedCount]() {
                auto answer = QMessageBox::question(this,
                                                    i18n("Purge Sessions"),
                                                    i18n("Permanently remove metadata for %1 dismissed session(s)?\n\n"
                                                         "Project folders will NOT be affected.",
                                                         selectedCount),
                                                    QMessageBox::Yes | QMessageBox::No,
                                                    QMessageBox::No);
                if (answer == QMessageBox::Yes) {
                    for (const auto &sid : selectedDismissed) {
                        purgeSession(sid);
                    }
                }
            });
        } else {
            // Single item menu
            QAction *restoreAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-undo")), i18n("Restore"));
            connect(restoreAction, &QAction::triggered, this, [this, sessionId]() {
                restoreSession(sessionId);
            });

            menu.addSeparator();

            QAction *purgeAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Purge"));
            connect(purgeAction, &QAction::triggered, this, [this, sessionId]() {
                auto answer = QMessageBox::question(this,
                                                    i18n("Purge Session"),
                                                    i18n("Permanently remove this session's metadata?\n\n"
                                                         "Project folder will NOT be affected."),
                                                    QMessageBox::Yes | QMessageBox::No,
                                                    QMessageBox::No);
                if (answer == QMessageBox::Yes) {
                    purgeSession(sessionId);
                }
            });
        }
    } else if (meta.isArchived) {
        // Collect all selected archived session IDs for batch operations
        QStringList selectedArchived;
        const auto selectedItems = m_treeWidget->selectedItems();
        for (auto *sel : selectedItems) {
            if (sel->parent() == m_archivedCategory) {
                QString sid = sel->data(0, Qt::UserRole).toString();
                if (!sid.isEmpty() && m_metadata.contains(sid) && m_metadata[sid].isArchived) {
                    selectedArchived.append(sid);
                }
            }
        }
        // Ensure the right-clicked item is included
        if (!selectedArchived.contains(sessionId)) {
            selectedArchived.append(sessionId);
        }

        int selectedCount = selectedArchived.size();

        if (selectedCount > 1) {
            // Multi-selection batch menu
            QAction *unarchiveAction =
                menu.addAction(QIcon::fromTheme(QStringLiteral("archive-extract")), i18n("Unarchive && Start Selected (%1)", selectedCount));
            connect(unarchiveAction, &QAction::triggered, this, [this, selectedArchived]() {
                for (const auto &sid : selectedArchived) {
                    unarchiveSession(sid);
                }
            });

            menu.addSeparator();

            QAction *dismissAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-clear-history")), i18n("Dismiss Selected (%1)", selectedCount));
            connect(dismissAction, &QAction::triggered, this, [this, selectedArchived, selectedCount]() {
                auto answer = QMessageBox::question(this,
                                                    i18n("Dismiss Sessions"),
                                                    i18n("Dismiss %1 archived session(s)?", selectedCount),
                                                    QMessageBox::Yes | QMessageBox::No,
                                                    QMessageBox::No);
                if (answer == QMessageBox::Yes) {
                    for (const auto &sid : selectedArchived) {
                        dismissSession(sid);
                    }
                }
            });
        } else {
            // Single item menu
            QAction *unarchiveAction = menu.addAction(QIcon::fromTheme(QStringLiteral("archive-extract")), i18n("Unarchive && Start"));
            connect(unarchiveAction, &QAction::triggered, this, [this, sessionId]() {
                unarchiveSession(sessionId);
            });

            menu.addSeparator();

            QAction *dismissAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-clear-history")), i18n("Dismiss"));
            connect(dismissAction, &QAction::triggered, this, [this, sessionId]() {
                dismissSession(sessionId);
            });

            if (!meta.workingDirectory.isEmpty() && QDir(meta.workingDirectory).exists()) {
                QAction *trashAction = menu.addAction(QIcon::fromTheme(QStringLiteral("user-trash")), i18n("Move to Trash..."));
                connect(trashAction, &QAction::triggered, this, [this, sessionId, meta]() {
                    auto answer = QMessageBox::question(this,
                                                        i18n("Move to Trash"),
                                                        i18n("Move this project folder to the trash?\n\n%1", meta.workingDirectory),
                                                        QMessageBox::Yes | QMessageBox::No,
                                                        QMessageBox::No);
                    if (answer == QMessageBox::Yes) {
                        QFile dir(meta.workingDirectory);
                        if (dir.moveToTrash()) {
                            m_metadata.remove(sessionId);
                            scheduleMetadataSave();
                            updateTreeWidget();
                        } else {
                            QMessageBox::warning(this, i18n("Trash Failed"), i18n("Could not move folder to trash:\n%1", meta.workingDirectory));
                        }
                    }
                });
            }
        }
    } else {
        // Active, detached, or closed session
        bool isActive = m_activeSessions.contains(sessionId);
        bool isClosed = (item->parent() == m_closedCategory);
        QPointer<ClaudeSession> activeSession = isActive ? m_activeSessions[sessionId] : nullptr;

        if (isActive && activeSession) {
            QAction *focusAction = menu.addAction(QIcon::fromTheme(QStringLiteral("go-jump")), i18n("Focus Tab"));
            connect(focusAction, &QAction::triggered, this, [this, activeSession]() {
                if (activeSession) {
                    Q_EMIT focusSessionRequested(activeSession);
                }
            });
        } else if (isClosed) {
            // Closed session — tmux is dead, offer restart (fresh tmux, optionally resume conversation)
            QAction *restartAction = menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Restart Session"));
            connect(restartAction, &QAction::triggered, this, [this, sessionId]() {
                unarchiveSession(sessionId);
            });
        } else if (!isActive) {
            if (meta.isRemote) {
                // Remote detached session — reattach via SSH
                QAction *attachAction = menu.addAction(QIcon::fromTheme(QStringLiteral("network-connect")), i18n("Attach (SSH)"));
                connect(attachAction, &QAction::triggered, this, [this, meta]() {
                    Q_EMIT remoteAttachRequested(meta.sshHost, meta.sshUsername, meta.sshPort, meta.workingDirectory, meta.sessionName);
                });
            } else {
                QAction *attachAction = menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Attach"));
                connect(attachAction, &QAction::triggered, this, [this, meta]() {
                    Q_EMIT attachRequested(meta.sessionName);
                });
            }
        }

        menu.addSeparator();

        if (meta.isPinned) {
            QAction *unpinAction = menu.addAction(QIcon::fromTheme(QStringLiteral("window-unpin")), i18n("Unpin"));
            connect(unpinAction, &QAction::triggered, this, [this, sessionId]() {
                unpinSession(sessionId);
            });
        } else {
            QAction *pinAction = menu.addAction(QIcon::fromTheme(QStringLiteral("pin")), i18n("Pin to Top"));
            connect(pinAction, &QAction::triggered, this, [this, sessionId]() {
                pinSession(sessionId);
            });
        }

        // Set/edit task description
        QAction *descAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18n("Set Description..."));
        connect(descAction, &QAction::triggered, this, [this, sessionId]() {
            editSessionDescription(sessionId);
        });

        // View Session Activity — show parsed conversation transcript
        // Defer the expensive .jsonl lookup to when user actually clicks the action
        if (!meta.workingDirectory.isEmpty()) {
            QString convId = meta.lastResumeSessionId;
            if (convId.isEmpty() && isActive && activeSession) {
                convId = activeSession->resumeSessionId();
            }
            QAction *activityAction = menu.addAction(QIcon::fromTheme(QStringLiteral("view-list-text")), i18n("View Session Activity"));
            connect(activityAction, &QAction::triggered, this, [this, convId, meta]() {
                // Use cached conversations to avoid disk I/O
                if (!m_conversationCache.contains(meta.workingDirectory)) {
                    m_conversationCache.insert(meta.workingDirectory, ClaudeSessionRegistry::readClaudeConversations(meta.workingDirectory));
                }
                const auto &conversations = m_conversationCache[meta.workingDirectory];

                // Find the .jsonl path for this conversation
                auto findJsonlPath = [](const QString &targetId) -> QString {
                    if (targetId.isEmpty())
                        return {};
                    QString projectsDir = QDir::homePath() + QStringLiteral("/.claude/projects");
                    QDirIterator it(projectsDir, QDir::Dirs | QDir::NoDotAndDotDot);
                    while (it.hasNext()) {
                        QString dir = it.next();
                        QString candidate = dir + QStringLiteral("/") + targetId + QStringLiteral(".jsonl");
                        if (QFile::exists(candidate)) {
                            return candidate;
                        }
                    }
                    return {};
                };

                QString jsonlPath;
                if (!convId.isEmpty()) {
                    for (const auto &conv : conversations) {
                        if (conv.sessionId == convId) {
                            jsonlPath = findJsonlPath(convId);
                            break;
                        }
                    }
                }
                // Fallback: most recent conversation
                if (jsonlPath.isEmpty() && !conversations.isEmpty()) {
                    jsonlPath = findJsonlPath(conversations.first().sessionId);
                }
                if (!jsonlPath.isEmpty()) {
                    showSessionActivity(jsonlPath, meta.workingDirectory);
                }
            });
        }

        // Show approval log for active sessions with approvals
        if (isActive && activeSession && activeSession->totalApprovalCount() > 0) {
            QAction *logAction =
                menu.addAction(QIcon::fromTheme(QStringLiteral("view-list-details")), i18n("View Approval Log (%1)", activeSession->totalApprovalCount()));
            connect(logAction, &QAction::triggered, this, [this, activeSession]() {
                if (activeSession) {
                    showApprovalLog(activeSession);
                }
            });
        }

        // Yolo mode toggles for active sessions
        if (isActive && activeSession) {
            menu.addSeparator();

            QAction *yoloAction = menu.addAction(i18n("Yolo Mode"));
            yoloAction->setCheckable(true);
            yoloAction->setChecked(activeSession->yoloMode());
            connect(yoloAction, &QAction::triggered, this, [this, activeSession](bool checked) {
                if (activeSession) {
                    activeSession->setYoloMode(checked);
                    if (checked) {
                        ensureHooksConfigured(activeSession);
                    }
                }
            });

            QAction *doubleYoloAction = menu.addAction(i18n("Double Yolo"));
            doubleYoloAction->setCheckable(true);
            doubleYoloAction->setChecked(activeSession->doubleYoloMode());
            connect(doubleYoloAction, &QAction::triggered, this, [this, activeSession](bool checked) {
                if (activeSession) {
                    activeSession->setDoubleYoloMode(checked);
                    if (checked) {
                        ensureHooksConfigured(activeSession);
                    }
                }
            });

            QAction *tripleYoloAction = menu.addAction(i18n("Triple Yolo"));
            tripleYoloAction->setCheckable(true);
            tripleYoloAction->setChecked(activeSession->tripleYoloMode());
            connect(tripleYoloAction, &QAction::triggered, this, [this, activeSession](bool checked) {
                if (activeSession) {
                    activeSession->setTripleYoloMode(checked);
                    if (checked) {
                        ensureHooksConfigured(activeSession);
                    }
                }
            });

            // Reset Hooks — force-clear all hooks and reset yolo state
            QAction *resetHooksAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-clear")), i18n("Reset Hooks"));
            connect(resetHooksAction, &QAction::triggered, this, [this, sessionId, activeSession]() {
                auto *meta = findMetadata(sessionId);
                if (!meta) {
                    return;
                }

                // 1. Clear all hooks from settings file
                ClaudeSession::removeHooksForWorkDir(meta->workingDirectory);

                // 2. Reset yolo state if session is active
                if (activeSession) {
                    activeSession->setYoloMode(false);
                    activeSession->setDoubleYoloMode(false);
                    activeSession->setTripleYoloMode(false);
                }

                // 3. Re-install fresh hooks if session is still active
                if (activeSession && m_activeSessions.contains(sessionId)) {
                    ensureHooksConfigured(activeSession);
                }

                qDebug() << "SessionManagerPanel: Reset hooks for session" << sessionId;
            });
        }

        // Edit budget controls for active sessions
        if (isActive && activeSession) {
            QAction *budgetAction = menu.addAction(QIcon::fromTheme(QStringLiteral("budget")), i18n("Edit Budget..."));
            connect(budgetAction, &QAction::triggered, this, [this, activeSession, sessionId]() {
                if (activeSession) {
                    editSessionBudget(activeSession, sessionId);
                }
            });
        }

        // Toggle completed agents visibility for sessions with subagents
        if (isActive && activeSession && !activeSession->subagents().isEmpty()) {
            bool hiding = m_hideCompletedAgents.contains(sessionId);
            QAction *toggleAction = menu.addAction(QIcon::fromTheme(QStringLiteral("view-visible")), i18n("Show Completed Agents"));
            toggleAction->setCheckable(true);
            toggleAction->setChecked(!hiding);
            connect(toggleAction, &QAction::triggered, this, [this, sessionId](bool checked) {
                if (checked) {
                    m_hideCompletedAgents.remove(sessionId);
                } else {
                    m_hideCompletedAgents.insert(sessionId);
                }
                scheduleTreeUpdate();
            });
        }

        // Mute auto-expand toggle for sessions with subagents
        if (isActive && activeSession && !activeSession->subagents().isEmpty()) {
            bool muted = m_mutedSessions.contains(sessionId);
            QAction *muteAction = menu.addAction(QIcon::fromTheme(muted ? QStringLiteral("audio-volume-high") : QStringLiteral("audio-volume-muted")),
                                                 muted ? i18n("Unmute Auto-Expand") : i18n("Mute Auto-Expand"));
            muteAction->setCheckable(true);
            muteAction->setChecked(muted);
            connect(muteAction, &QAction::triggered, this, [this, sessionId](bool checked) {
                if (checked) {
                    m_mutedSessions.insert(sessionId);
                    // Immediately collapse the session in the tree
                    if (QTreeWidgetItem *sessionItem = findTreeItem(sessionId)) {
                        sessionItem->setExpanded(false);
                    }
                } else {
                    m_mutedSessions.remove(sessionId);
                }
                scheduleTreeUpdate();
            });
        }

        // Restart option for active sessions
        if (isActive && activeSession) {
            QAction *restartAction = menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Restart Claude"));
            connect(restartAction, &QAction::triggered, this, [activeSession]() {
                if (activeSession) {
                    activeSession->restart();
                }
            });
        }

        // Create worktree session from this session's project
        if (!meta.workingDirectory.isEmpty()) {
            QAction *worktreeAction = menu.addAction(QIcon::fromTheme(QStringLiteral("vcs-branch")), i18n("New Worktree Session..."));
            connect(worktreeAction, &QAction::triggered, this, [this, meta]() {
                Q_EMIT worktreeSessionRequested(meta.workingDirectory);
            });
        }

        // Show Agent — navigate to agent panel for agent-originated sessions
        if (!meta.agentId.isEmpty()) {
            QAction *showAgentAction = menu.addAction(QIcon::fromTheme(QStringLiteral("system-run")), i18n("Show Agent"));
            QString agentIdCopy = meta.agentId;
            connect(showAgentAction, &QAction::triggered, this, [this, agentIdCopy]() {
                Q_EMIT showAgentRequested(agentIdCopy);
            });

            // Group All Agent Sessions — creates a folder with all sessions from this agent
            QAction *groupAgentAction = menu.addAction(QIcon::fromTheme(QStringLiteral("folder-new")), i18n("Group All '%1' Sessions", meta.agentId));
            connect(groupAgentAction, &QAction::triggered, this, [this, agentIdCopy]() {
                createFolder(agentIdCopy, QColor());
                // Find the just-created folder (most recent by createdAt)
                QString newestFolderId;
                QDateTime newest;
                for (auto fit = m_folders.constBegin(); fit != m_folders.constEnd(); ++fit) {
                    if (fit->createdAt > newest) {
                        newest = fit->createdAt;
                        newestFolderId = fit.key();
                    }
                }
                if (newestFolderId.isEmpty()) {
                    return;
                }
                for (auto &m : m_metadata) {
                    if (m.agentId == agentIdCopy) {
                        m.folderId = newestFolderId;
                    }
                }
                scheduleMetadataSave();
                scheduleTreeUpdate();
            });
        }

        // Move to Folder submenu
        {
            QMenu *folderMenu = menu.addMenu(QIcon::fromTheme(QStringLiteral("folder")), i18n("Move to Folder"));

            if (!meta.folderId.isEmpty()) {
                QAction *removeAction = folderMenu->addAction(i18n("Remove from Folder"));
                connect(removeAction, &QAction::triggered, this, [this, sessionId]() {
                    removeSessionFromFolder(sessionId);
                });
                folderMenu->addSeparator();
            }

            for (auto it = m_folders.constBegin(); it != m_folders.constEnd(); ++it) {
                if (it.key() == meta.folderId) {
                    continue; // Already in this folder
                }
                QAction *moveAction = folderMenu->addAction(QIcon::fromTheme(QStringLiteral("folder")), it->name);
                QString targetFolderId = it.key();
                connect(moveAction, &QAction::triggered, this, [this, sessionId, targetFolderId]() {
                    moveSessionToFolder(sessionId, targetFolderId);
                });
            }

            folderMenu->addSeparator();
            QAction *newFolderAction = folderMenu->addAction(QIcon::fromTheme(QStringLiteral("folder-new")), i18n("New Folder..."));
            connect(newFolderAction, &QAction::triggered, this, [this, sessionId]() {
                bool ok;
                QString name = QInputDialog::getText(this, i18n("New Folder"),
                    i18n("Folder name:"), QLineEdit::Normal, QString(), &ok);
                if (ok && !name.isEmpty()) {
                    createFolder(name, QColor());
                    // Find newest folder and move session into it
                    QString newestFolderId;
                    QDateTime newest;
                    for (auto fit = m_folders.constBegin(); fit != m_folders.constEnd(); ++fit) {
                        if (fit->createdAt > newest) {
                            newest = fit->createdAt;
                            newestFolderId = fit.key();
                        }
                    }
                    if (!newestFolderId.isEmpty()) {
                        moveSessionToFolder(sessionId, newestFolderId);
                    }
                }
            });
        }

        menu.addSeparator();

        QAction *closeAction = menu.addAction(QIcon::fromTheme(QStringLiteral("process-stop")), i18n("Close"));
        connect(closeAction, &QAction::triggered, this, [this, sessionId]() {
            closeSession(sessionId);
        });

        QAction *archiveAction = menu.addAction(QIcon::fromTheme(QStringLiteral("archive-remove")), i18n("Archive"));
        connect(archiveAction, &QAction::triggered, this, [this, sessionId]() {
            archiveSession(sessionId);
        });

        if (!isActive) {
            QAction *dismissAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-clear-history")), i18n("Dismiss"));
            connect(dismissAction, &QAction::triggered, this, [this, sessionId]() {
                dismissSession(sessionId);
            });
        }
    }

    menu.exec(m_treeWidget->viewport()->mapToGlobal(pos));
}

void SessionManagerPanel::onNewSessionClicked()
{
    Q_EMIT newSessionRequested();
}

void SessionManagerPanel::scheduleTreeUpdate()
{
    if (!m_initialized) {
        return;
    }

    // When window is inactive, skip tree rebuilds entirely — nobody is looking.
    // resumeBackgroundTimers() will trigger one rebuild when the window reactivates.
    if (m_timersPaused) {
        m_pendingUpdate = true;
        return;
    }

    // Debounce: coalesce rapid-fire signals (e.g. approvalCountChanged fires
    // many times per minute during yolo mode) into a single deferred update.
    if (!m_updateDebounce) {
        m_updateDebounce = new QTimer(this);
        m_updateDebounce->setSingleShot(true);
        connect(m_updateDebounce, &QTimer::timeout, this, &SessionManagerPanel::updateTreeWidget);
    }

    // Defer rebuild while user is interacting with the tree (hover or focus)
    if (isTreeInteractionActive()) {
        m_pendingUpdate = true;
        if (!m_deferRetryTimer) {
            m_deferRetryTimer = new QTimer(this);
            m_deferRetryTimer->setSingleShot(true);
            connect(m_deferRetryTimer, &QTimer::timeout, this, [this]() {
                if (!m_pendingUpdate) {
                    return;
                }
                if (isTreeInteractionActive()) {
                    m_deferRetryTimer->start(1000); // Still interacting, retry
                } else {
                    m_pendingUpdate = false;
                    m_updateDebounce->start(100); // Short debounce for deferred flush
                }
            });
        }
        if (!m_deferRetryTimer->isActive()) {
            m_deferRetryTimer->start(1000);
        }
        return;
    }

    m_pendingUpdate = false;
    // Restart the 500ms window on each call — only the last one fires.
    m_updateDebounce->start(500);
}

void SessionManagerPanel::updateDurationLabels()
{
    if (!m_treeWidget) {
        return;
    }

    // Walk tree items and update only the duration QLabel widgets in column 1,
    // avoiding a full teardown/rebuild just to refresh elapsed time strings.
    // Also check if any active items remain — stop timer if not.
    bool anyActive = false;

    // Helper: update labels for child items of a given tree item
    std::function<void(QTreeWidgetItem *)> walkChildren = [&](QTreeWidgetItem *parent) {
        for (int i = 0; i < parent->childCount(); ++i) {
            auto *child = parent->child(i);

            // Check if this item has a duration label widget in column 1
            auto *widget = m_treeWidget->itemWidget(child, 1);
            auto *label = qobject_cast<QLabel *>(widget);
            if (label) {
                // Look up the session and find the matching subagent/subprocess by ID
                QString parentSessionId = child->data(0, Qt::UserRole + 1).toString();
                ClaudeSession *session = m_activeSessions.value(parentSessionId);
                if (session) {
                    // Try as subagent (agentId in UserRole)
                    QString agentId = child->data(0, Qt::UserRole).toString();
                    if (!agentId.isEmpty() && session->subagents().contains(agentId)) {
                        const auto &info = session->subagents()[agentId];
                        QString elapsed = formatElapsed(info.startedAt);
                        if (label->text() != elapsed) {
                            label->setText(elapsed);
                        }
                        if (info.state == ClaudeProcess::State::Working || info.state == ClaudeProcess::State::Idle) {
                            anyActive = true;
                        }
                    }
                    // Try as subprocess (id in UserRole + 4)
                    QString procId = child->data(0, Qt::UserRole + 4).toString();
                    if (!procId.isEmpty() && session->subprocesses().contains(procId)) {
                        const auto &info = session->subprocesses()[procId];
                        QString col1;
                        QString elapsed = formatElapsed(info.startedAt);
                        if (!elapsed.isEmpty()) {
                            col1 = elapsed;
                        }
                        if (info.resourceUsage.rssBytes > 0 || info.resourceUsage.cpuPercent > 0.0) {
                            if (!col1.isEmpty())
                                col1 += QStringLiteral(" ");
                            col1 += info.resourceUsage.formatCompact();
                        }
                        if (label->text() != col1) {
                            label->setText(col1);
                        }
                        if (info.status == SubprocessInfo::Running) {
                            anyActive = true;
                        }
                    }
                }
            }

            // Recurse into children (prompt groups, subtasks containers, task groups)
            if (child->childCount() > 0) {
                walkChildren(child);
            }
        }
    };

    // Walk all category items
    auto categories = {m_pinnedCategory, m_activeCategory, m_detachedCategory, m_closedCategory, m_archivedCategory};
    for (auto *category : categories) {
        if (!category) {
            continue;
        }
        for (int i = 0; i < category->childCount(); ++i) {
            auto *sessionItem = category->child(i);
            walkChildren(sessionItem);
        }
    }

    // Stop duration timer if no active items remain
    if (!anyActive && m_durationTimer) {
        m_durationTimer->stop();
    }
}

void SessionManagerPanel::scheduleMetadataSave()
{
    // When window is inactive, defer saves — they'll flush on resume.
    if (m_timersPaused) {
        m_pendingSave = true;
        return;
    }

    if (!m_saveDebounce) {
        m_saveDebounce = new QTimer(this);
        m_saveDebounce->setSingleShot(true);
        connect(m_saveDebounce, &QTimer::timeout, this, [this]() {
            saveMetadata();
        });
    }
    m_saveDebounce->start(1000);
}

void SessionManagerPanel::updateTreeWidget()
{
    if (!m_initialized) {
        return;
    }

    // Guard against overlapping async tmux queries - if one is already running,
    // mark that we need another update after it finishes
    if (m_asyncQueryInFlight) {
        m_asyncQueryPending = true;
        return;
    }

    m_asyncQueryInFlight = true;
    m_asyncQueryPending = false;

    // Async pre-pass: query tmux for live sessions without blocking the GUI,
    // then call updateTreeWidgetWithLiveSessions() with the result.
    // TmuxManager is NOT parented to this, so it survives panel destruction.
    auto *tmux = new TmuxManager(nullptr);

    // Use QPointer to guard against the panel being destroyed before callback fires
    QPointer<SessionManagerPanel> guard(this);

    tmux->listKonsolaiSessionsAsync([guard, tmux](const QList<TmuxManager::SessionInfo> &liveSessions) {
        tmux->deleteLater();

        // Check if the panel was destroyed while waiting for async result
        if (!guard) {
            qDebug() << "SessionManagerPanel: Panel destroyed during async tmux query, skipping update";
            return;
        }

        guard->m_asyncQueryInFlight = false;

        QSet<QString> liveNames;
        for (const auto &info : liveSessions) {
            liveNames.insert(info.name);
        }
        guard->m_cachedLiveNames = liveNames;
        guard->updateTreeWidgetWithLiveSessions(liveNames);

        // If another update was requested while we were querying, run it now
        if (guard->m_asyncQueryPending) {
            guard->updateTreeWidget();
        }
    });
}

void SessionManagerPanel::refreshRemoteTmuxSessions()
{
    // Collect unique SSH hosts from metadata
    QMap<QString, QPair<QString, int>> hostCredentials; // host → (username, port)
    for (const auto &meta : std::as_const(m_metadata)) {
        if (meta.isRemote && !meta.sshHost.isEmpty()) {
            if (!hostCredentials.contains(meta.sshHost)) {
                hostCredentials[meta.sshHost] = {meta.sshUsername, meta.sshPort};
            }
        }
    }

    if (hostCredentials.isEmpty()) {
        return;
    }

    // Use a shared counter to track when all queries complete,
    // accumulating into a temporary set to avoid clearing the cache
    // while queries are still in flight.
    auto pendingCount = std::make_shared<int>(hostCredentials.size());
    auto accumulated = std::make_shared<QSet<QString>>();

    // Query each unique host asynchronously
    for (auto it = hostCredentials.constBegin(); it != hostCredentials.constEnd(); ++it) {
        const QString host = it.key();
        const QString user = it.value().first;
        const int port = it.value().second;

        auto *proc = new QProcess(this);
        QStringList args = {QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                            QStringLiteral("-o"), QStringLiteral("ConnectTimeout=5")};
        if (port != 22) {
            args << QStringLiteral("-p") << QString::number(port);
        }
        QString userHost = user.isEmpty() ? host : QStringLiteral("%1@%2").arg(user, host);
        args << userHost << QStringLiteral("tmux list-sessions -F '#{session_name}' 2>/dev/null | grep ^konsolai-");

        QPointer<SessionManagerPanel> guard(this);
        connect(proc,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                [guard, proc, pendingCount, accumulated](int exitCode, QProcess::ExitStatus) {
                    proc->deleteLater();
                    if (!guard) {
                        return;
                    }
                    if (exitCode == 0) {
                        QString output = QString::fromUtf8(proc->readAllStandardOutput());
                        const auto lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                        for (const auto &line : lines) {
                            QString name = line.trimmed();
                            if (!name.isEmpty()) {
                                accumulated->insert(name);
                            }
                        }
                    }
                    // Only update cache and tree when all host queries have completed
                    --(*pendingCount);
                    if (*pendingCount <= 0) {
                        guard->m_cachedRemoteLiveNames = *accumulated;
                        guard->scheduleTreeUpdate();
                    }
                });

        // Kill SSH process after 15s to prevent leak on network hang
        QPointer<QProcess> safeProc(proc);
        QTimer::singleShot(15000, proc, [safeProc, pendingCount, accumulated, guard]() {
            if (safeProc && safeProc->state() != QProcess::NotRunning) {
                safeProc->kill();
                // finished() signal will fire after kill, delivering deleteLater and
                // decrementing pendingCount via the connection above.
            }
        });

        proc->start(QStringLiteral("ssh"), args);
    }
}

void SessionManagerPanel::refreshCachesAsync()
{
    if (m_cacheRefreshInFlight) {
        return;
    }
    m_cacheRefreshInFlight = true;

    // Snapshot search root
    QString searchRoot;
    auto *settings = KonsolaiSettings::instance();
    if (settings) {
        searchRoot = settings->projectRoot();
    }
    if (searchRoot.isEmpty()) {
        searchRoot = QDir::homePath() + QStringLiteral("/projects");
    }

    // Snapshot known working directories from registry (discover filter)
    QSet<QString> knownDirs;
    if (m_registry) {
        for (const auto &state : m_registry->allSessionStates()) {
            knownDirs.insert(state.workingDirectory);
        }
    }

    // Collect all directories that need conversation refresh
    QSet<QString> convDirs;
    for (auto it = m_metadata.constBegin(); it != m_metadata.constEnd(); ++it) {
        if (!it.value().workingDirectory.isEmpty()) {
            convDirs.insert(it.value().workingDirectory);
        }
    }
    for (const auto &state : std::as_const(m_cachedDiscoveredSessions)) {
        convDirs.insert(state.workingDirectory);
    }

    QPointer<SessionManagerPanel> guard(this);
    auto future = QtConcurrent::run([searchRoot, knownDirs, convDirs]() -> CacheRefreshResult {
        CacheRefreshResult result;

        // Discover sessions (directory scan + settings.local.json reads)
        if (!searchRoot.isEmpty() && QDir(searchRoot).exists()) {
            QDir rootDir(searchRoot);
            const auto entries = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString &dirName : entries) {
                QString projectPath = rootDir.filePath(dirName);
                QString claudeDir = QDir(projectPath).filePath(QStringLiteral(".claude"));
                if (!QDir(claudeDir).exists()) {
                    continue;
                }
                if (knownDirs.contains(projectPath)) {
                    continue;
                }

                ClaudeSessionState state;
                state.sessionName = QStringLiteral("discovered-%1").arg(dirName);
                state.sessionId = dirName.left(8);
                state.workingDirectory = projectPath;
                state.isAttached = false;

                QString settingsPath = QDir(claudeDir).filePath(QStringLiteral("settings.local.json"));
                if (QFile::exists(settingsPath)) {
                    QFile f(settingsPath);
                    if (f.open(QIODevice::ReadOnly)) {
                        QJsonParseError err;
                        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
                        if (err.error == QJsonParseError::NoError && doc.isObject()) {
                            QString content = QString::fromUtf8(doc.toJson());
                            state.profileName = content.contains(QStringLiteral("konsolai")) ? QStringLiteral("Claude") : QStringLiteral("External");
                        }
                        f.close();
                    }
                } else {
                    state.profileName = QStringLiteral("External");
                }

                QFileInfo claudeDirInfo(claudeDir);
                state.created = claudeDirInfo.birthTime().isValid() ? claudeDirInfo.birthTime() : claudeDirInfo.lastModified();
                state.lastAccessed = claudeDirInfo.lastModified();

                result.discovered.append(state);
            }
        }

        // Read conversations for all directories (the expensive part)
        QSet<QString> allDirs = convDirs;
        for (const auto &state : std::as_const(result.discovered)) {
            allDirs.insert(state.workingDirectory);
        }
        for (const QString &dir : std::as_const(allDirs)) {
            result.conversations[dir] = ClaudeSessionRegistry::readClaudeConversations(dir);
        }

        return result;
    });

    auto *watcher = new QFutureWatcher<CacheRefreshResult>(this);
    connect(watcher, &QFutureWatcher<CacheRefreshResult>::finished, this, [this, guard, watcher]() {
        if (!guard) {
            watcher->deleteLater();
            return;
        }
        auto result = watcher->result();
        m_cachedDiscoveredSessions = result.discovered;
        m_discoveredCacheValid = true;
        for (auto it = result.conversations.constBegin(); it != result.conversations.constEnd(); ++it) {
            m_conversationCache[it.key()] = it.value();
        }
        m_cacheRefreshInFlight = false;
        watcher->deleteLater();
        scheduleTreeUpdate();
    });
    watcher->setFuture(future);
}

void SessionManagerPanel::updateTreeWidgetWithLiveSessions(const QSet<QString> &liveNames)
{
    // Purge stale null QPointer entries — sessions destroyed via deleteLater()
    // leave null entries in the map that would otherwise be treated as "active".
    for (auto it = m_activeSessions.begin(); it != m_activeSessions.end();) {
        if (it.value().isNull()) {
            it = m_activeSessions.erase(it);
        } else {
            ++it;
        }
    }

    // Caches are now TTL-based (git 60s, conversations/discovered/GSD 120s)
    // and invalidated on targeted events (register, unregister, workDir change).
    // No unconditional clear here — this was the main perf bottleneck.

    // Prune stale expansion keys if the set has grown large
    if (m_knownItems.size() > 500) {
        pruneStaleKeys();
    }

    // Save expansion state, scroll position, and selection before destroying items
    saveTreeState();

    // Suppress repaints during rebuild to eliminate flicker
    m_treeWidget->setUpdatesEnabled(false);

    // Clear existing items (except categories)
    while (m_pinnedCategory->childCount() > 0) {
        delete m_pinnedCategory->takeChild(0);
    }
    while (m_activeCategory->childCount() > 0) {
        delete m_activeCategory->takeChild(0);
    }
    while (m_detachedCategory->childCount() > 0) {
        delete m_detachedCategory->takeChild(0);
    }
    while (m_closedCategory->childCount() > 0) {
        delete m_closedCategory->takeChild(0);
    }
    while (m_archivedCategory->childCount() > 0) {
        delete m_archivedCategory->takeChild(0);
    }
    while (m_dismissedCategory->childCount() > 0) {
        delete m_dismissedCategory->takeChild(0);
    }

    // Clear and recreate folder top-level items
    for (auto *folderItem : std::as_const(m_folderTreeItems)) {
        int idx = m_treeWidget->indexOfTopLevelItem(folderItem);
        if (idx >= 0) {
            delete m_treeWidget->takeTopLevelItem(idx);
        }
    }
    m_folderTreeItems.clear();

    // Create folder items sorted by sortOrder, inserted BEFORE category items
    QList<SessionFolderInfo> sortedFolders = m_folders.values();
    std::sort(sortedFolders.begin(), sortedFolders.end(),
        [](const SessionFolderInfo &a, const SessionFolderInfo &b) {
            return a.sortOrder < b.sortOrder;
        });
    int folderInsertIdx = 0;
    for (const auto &folder : std::as_const(sortedFolders)) {
        auto *folderItem = new QTreeWidgetItem();
        folderItem->setText(0, folder.name);
        folderItem->setIcon(0, QIcon::fromTheme(QStringLiteral("folder")));
        if (folder.color.isValid()) {
            folderItem->setForeground(0, QBrush(folder.color));
        }
        folderItem->setFlags(Qt::ItemIsEnabled);
        folderItem->setData(0, Qt::UserRole + 7, folder.folderId);
        folderItem->setData(0, Qt::UserRole + 6, QStringLiteral("f:%1").arg(folder.folderId));
        folderItem->setExpanded(true);
        m_treeWidget->insertTopLevelItem(folderInsertIdx++, folderItem);
        m_folderTreeItems[folder.folderId] = folderItem;
    }

    // Note: We no longer auto-archive dead tmux sessions.
    // Dead sessions go to "Closed", user-archived sessions go to "Archived".

    // Sort sessions by last accessed (most recent first)
    QList<SessionMetadata> sortedMeta = m_metadata.values();
    std::sort(sortedMeta.begin(), sortedMeta.end(), [](const SessionMetadata &a, const SessionMetadata &b) {
        return a.lastAccessed > b.lastAccessed;
    });

    // Pre-pass: count sessions per (workingDirectory, category) to detect duplicates.
    // When multiple sessions share the same directory and end up in the same category,
    // addSessionToTree will append a creation timestamp for disambiguation.
    QHash<QString, int> dirCategoryCount; // "workDir|categoryName" → count
    auto categoryKey = [&](const SessionMetadata &m, const QString &cat) {
        return m.workingDirectory + QStringLiteral("|") + cat;
    };
    for (const auto &meta : sortedMeta) {
        bool isAct = m_activeSessions.contains(meta.sessionId);
        bool alive = liveNames.contains(meta.sessionName)
            || (meta.isRemote && m_cachedRemoteLiveNames.contains(meta.sessionName));
        bool closed = m_explicitlyClosed.contains(meta.sessionId);
        if (closed && !alive) closed = false;

        QString cat;
        if (meta.isDismissed) cat = QStringLiteral("dismissed");
        else if (meta.isArchived) cat = QStringLiteral("archived");
        else if (meta.isPinned) cat = QStringLiteral("pinned");
        else if (isAct) cat = QStringLiteral("active");
        else if (alive && !closed) cat = QStringLiteral("detached");
        else cat = QStringLiteral("closed");
        dirCategoryCount[categoryKey(meta, cat)]++;
    }

    // Add sessions to appropriate categories
    // Priority: Dismissed > Archived > Pinned > Active (has tab) > Detached (tmux alive) > Closed (tmux dead)
    for (const auto &meta : sortedMeta) {
        bool isActive = m_activeSessions.contains(meta.sessionId);
        bool tmuxAlive = liveNames.contains(meta.sessionName)
            || (meta.isRemote && m_cachedRemoteLiveNames.contains(meta.sessionName));
        bool wasClosed = m_explicitlyClosed.contains(meta.sessionId);

        // Clear from explicitly-closed set once tmux is actually dead
        if (wasClosed && !tmuxAlive) {
            m_explicitlyClosed.remove(meta.sessionId);
            wasClosed = false; // already categorized correctly via tmuxAlive=false
        }

        // Route to folder if session is in one, otherwise to lifecycle category
        if (!meta.folderId.isEmpty() && m_folderTreeItems.contains(meta.folderId)) {
            QTreeWidgetItem *folderParent = m_folderTreeItems[meta.folderId];
            addSessionToTree(meta, folderParent, false);
            continue;
        }

        QString cat;
        QTreeWidgetItem *parent = nullptr;
        if (meta.isDismissed) { cat = QStringLiteral("dismissed"); parent = m_dismissedCategory; }
        else if (meta.isArchived) { cat = QStringLiteral("archived"); parent = m_archivedCategory; }
        else if (meta.isPinned) { cat = QStringLiteral("pinned"); parent = m_pinnedCategory; }
        else if (isActive) { cat = QStringLiteral("active"); parent = m_activeCategory; }
        else if (tmuxAlive && !wasClosed) { cat = QStringLiteral("detached"); parent = m_detachedCategory; }
        else { cat = QStringLiteral("closed"); parent = m_closedCategory; }

        bool hasSiblings = dirCategoryCount.value(categoryKey(meta, cat), 0) > 1;
        addSessionToTree(meta, parent, hasSiblings);
    }

    // Add discovered sessions (from project folder scanning)
    while (m_discoveredCategory->childCount() > 0) {
        delete m_discoveredCategory->takeChild(0);
    }
    if (m_registry) {
        // Use cached data only — background refreshCachesAsync() keeps it fresh.
        // Never call discoverSessions() or readClaudeConversations() here
        // to avoid blocking the main thread.
        for (const auto &state : std::as_const(m_cachedDiscoveredSessions)) {
            auto *item = new QTreeWidgetItem(m_discoveredCategory);
            QString displayName = QDir(state.workingDirectory).dirName();
            item->setText(0, displayName);
            item->setData(0, Qt::UserRole, state.sessionId);
            item->setData(0, Qt::UserRole + 1, state.workingDirectory);
            item->setIcon(0, QIcon::fromTheme(QStringLiteral("folder-cloud")));

            // Show conversation count from cache (populated by refreshCachesAsync)
            const auto &conversations = m_conversationCache[state.workingDirectory];
            if (!conversations.isEmpty()) {
                item->setText(1, QStringLiteral("%1 conv").arg(conversations.size()));
            }

            item->setToolTip(0, QStringLiteral("%1\n%2\nLast modified: %3").arg(state.profileName, state.workingDirectory, state.lastAccessed.toString()));
        }
    }

    // Stop duration timer if no sessions have active teams
    if (m_durationTimer && m_durationTimer->isActive()) {
        bool anyActiveTeam = false;
        for (auto it = m_activeSessions.constBegin(); it != m_activeSessions.constEnd(); ++it) {
            if (it.value() && it.value()->hasActiveTeam()) {
                anyActiveTeam = true;
                break;
            }
        }
        if (!anyActiveTeam) {
            m_durationTimer->stop();
        }
    }

    // Update category visibility and counts in headers
    auto updateCategory = [](QTreeWidgetItem *cat, const QString &baseName) {
        int count = cat->childCount();
        cat->setHidden(count == 0);
        if (count > 0) {
            cat->setText(0, QStringLiteral("%1 (%2)").arg(baseName).arg(count));
        } else {
            cat->setText(0, baseName);
        }
    };
    updateCategory(m_pinnedCategory, i18n("Pinned"));
    updateCategory(m_activeCategory, i18n("Active"));
    updateCategory(m_detachedCategory, i18n("Detached"));
    updateCategory(m_closedCategory, i18n("Closed"));
    updateCategory(m_archivedCategory, i18n("Archived"));
    updateCategory(m_dismissedCategory, i18n("Dismissed"));
    updateCategory(m_discoveredCategory, i18n("Discovered"));

    // Update folder headers with member count
    for (auto it = m_folderTreeItems.constBegin(); it != m_folderTreeItems.constEnd(); ++it) {
        QTreeWidgetItem *folderItem = it.value();
        int count = folderItem->childCount();
        const auto &folder = m_folders[it.key()];
        folderItem->setHidden(count == 0);
        if (count > 0) {
            folderItem->setText(0, QStringLiteral("%1 (%2)").arg(folder.name).arg(count));
        } else {
            folderItem->setText(0, folder.name);
        }
    }

    // Show empty state if no sessions at all
    int totalChildren = m_pinnedCategory->childCount() + m_activeCategory->childCount() + m_detachedCategory->childCount()
        + m_closedCategory->childCount() + m_archivedCategory->childCount() + m_dismissedCategory->childCount() + m_discoveredCategory->childCount();
    // Include folder children in total count
    for (auto *folderItem : std::as_const(m_folderTreeItems)) {
        totalChildren += folderItem->childCount();
    }
    m_emptyStateLabel->setVisible(totalChildren == 0);

    // Re-apply active filter after tree rebuild
    if (!m_filterEdit->text().isEmpty()) {
        applyFilter(m_filterEdit->text());
    }

    // Re-enable repaints after rebuild
    m_treeWidget->setUpdatesEnabled(true);

    // Restore scroll position and selection
    restoreTreeState();
}

void SessionManagerPanel::applyFilter(const QString &text)
{
    if (!m_treeWidget) {
        return;
    }

    // Iterate all category items, show/hide children based on filter match
    QList<QTreeWidgetItem *> categories = {m_pinnedCategory, m_activeCategory,    m_detachedCategory,  m_closedCategory,
                                                  m_archivedCategory, m_dismissedCategory, m_discoveredCategory};
    // Include folder items in filtering
    for (auto *folderItem : std::as_const(m_folderTreeItems)) {
        categories.append(folderItem);
    }

    for (auto *cat : categories) {
        if (!cat) {
            continue;
        }
        int visibleChildren = 0;
        for (int i = 0; i < cat->childCount(); ++i) {
            auto *child = cat->child(i);
            if (text.isEmpty()) {
                child->setHidden(false);
                ++visibleChildren;
            } else {
                // Match against display name (col 0), tooltip (working dir + session name),
                // and the raw sessionId (for ID-based lookup)
                QString sessionId = child->data(0, Qt::UserRole).toString();
                bool matches = child->text(0).contains(text, Qt::CaseInsensitive) || child->toolTip(0).contains(text, Qt::CaseInsensitive)
                    || sessionId.contains(text, Qt::CaseInsensitive);
                child->setHidden(!matches);
                if (matches) {
                    ++visibleChildren;
                }
            }
        }
        cat->setHidden(text.isEmpty() ? cat->childCount() == 0 : visibleChildren == 0);
    }
}

void SessionManagerPanel::addSessionToTree(const SessionMetadata &meta, QTreeWidgetItem *parent, bool hasSiblings)
{
    auto *item = new QTreeWidgetItem(parent);

    // Display name: project directory or session name
    QString displayName;
    if (!meta.workingDirectory.isEmpty() && meta.workingDirectory != QStringLiteral(".") && meta.workingDirectory != QDir::homePath()) {
        displayName = QDir(meta.workingDirectory).dirName();
    }
    // Fallback to session name if display name is empty or just "." or "build" (which is misleading)
    if (displayName.isEmpty() || displayName == QStringLiteral(".") || displayName == QStringLiteral("build")) {
        displayName = meta.sessionName;
    }

    // Add task description for disambiguation
    // Priority: active taskDescription > persisted description > Claude CLI conversation > nothing
    bool isActive = m_activeSessions.contains(meta.sessionId);
    QString description;

    // 1. Live session task description
    if (isActive) {
        ClaudeSession *session = m_activeSessions[meta.sessionId];
        if (session && !session->taskDescription().isEmpty()) {
            description = session->taskDescription();
        }
    }

    // 2. Persisted description from previous run
    if (description.isEmpty() && !meta.description.isEmpty()) {
        description = meta.description;
    }

    // 3. Claude CLI conversation data (only available for local sessions)
    // Use cached data only — never block tree rebuild with disk I/O.
    // refreshCachesAsync() keeps the cache populated in background.
    if (description.isEmpty() && !meta.workingDirectory.isEmpty() && m_registry && m_conversationCache.contains(meta.workingDirectory)) {
        const auto &conversations = m_conversationCache[meta.workingDirectory];

        // 1. Direct match by conversation UUID
        if (!meta.lastResumeSessionId.isEmpty()) {
            for (const auto &conv : conversations) {
                if (conv.sessionId == meta.lastResumeSessionId) {
                    description = conv.summary.isEmpty() ? conv.firstPrompt : conv.summary;
                    break;
                }
            }
        }
        // 2. Match by closest creation timestamp
        if (description.isEmpty() && meta.createdAt.isValid() && !conversations.isEmpty()) {
            qint64 bestDelta = std::numeric_limits<qint64>::max();
            const ClaudeConversation *bestMatch = nullptr;
            for (const auto &conv : conversations) {
                if (!conv.created.isValid()) {
                    continue;
                }
                qint64 delta = qAbs(meta.createdAt.secsTo(conv.created));
                if (delta < bestDelta) {
                    bestDelta = delta;
                    bestMatch = &conv;
                }
            }
            if (bestMatch) {
                description = bestMatch->summary.isEmpty() ? bestMatch->firstPrompt : bestMatch->summary;
            }
        }
        // 3. Last resort: most recent conversation
        if (description.isEmpty() && !conversations.isEmpty()) {
            const auto &first = conversations.first();
            description = first.summary.isEmpty() ? first.firstPrompt : first.summary;
        }
    }

    // Apply description or fall back to session ID
    if (!description.isEmpty()) {
        // Collapse newlines and trim for single-line display
        description = description.simplified();
        if (description.length() > 35) {
            description = description.left(32) + QStringLiteral("...");
        }
        displayName += QStringLiteral(" (%1)").arg(description);
    }
    // No hash fallback — directory name alone is clearer than a random hex string

    // When multiple sessions share the same directory in the same category,
    // append creation date to disambiguate visually-identical entries.
    if (hasSiblings && meta.createdAt.isValid()) {
        // Use "MMM d" for older sessions, "h:mm AP" for today's sessions
        if (meta.createdAt.date() == QDate::currentDate()) {
            displayName += QStringLiteral(" — %1").arg(meta.createdAt.toString(QStringLiteral("h:mm AP")));
        } else {
            displayName += QStringLiteral(" — %1").arg(meta.createdAt.toString(QStringLiteral("MMM d")));
        }
    }

    // Add team badge when subagents exist (active or completed/persisted)
    if (isActive) {
        ClaudeSession *activeSession = m_activeSessions[meta.sessionId];
        if (activeSession && !activeSession->subagents().isEmpty()) {
            int activeCount = 0;
            int totalCount = activeSession->subagents().size();
            const auto &agents = activeSession->subagents();
            for (auto it = agents.constBegin(); it != agents.constEnd(); ++it) {
                if (it->state == ClaudeProcess::State::Working || it->state == ClaudeProcess::State::Idle) {
                    activeCount++;
                }
            }
            QString badgeLabel = activeSession->teamName().isEmpty() ? QStringLiteral("team") : activeSession->teamName();
            if (activeCount == 0) {
                displayName += QStringLiteral(" [%1: done]").arg(badgeLabel);
            } else if (activeCount < totalCount) {
                displayName += QStringLiteral(" [%1: %2/%3]").arg(badgeLabel).arg(activeCount).arg(totalCount);
            } else {
                displayName += QStringLiteral(" [%1: %2]").arg(badgeLabel).arg(activeCount);
            }
        }
    } else if (!meta.subagents.isEmpty()) {
        // Persisted team badge
        displayName += QStringLiteral(" [team: done]");
    }

    // Agent linkage badge
    if (!meta.agentId.isEmpty()) {
        displayName += QStringLiteral(" [\U0001F916 %1]").arg(meta.agentId);
    }

    // Add GSD badge when .planning/ or ROADMAP.md exists in working directory (cached)
    if (!meta.workingDirectory.isEmpty()) {
        if (!m_gsdBadgeCache.contains(meta.workingDirectory)) {
            QDir workDir(meta.workingDirectory);
            m_gsdBadgeCache.insert(meta.workingDirectory, workDir.exists(QStringLiteral(".planning")) || workDir.exists(QStringLiteral("ROADMAP.md")));
        }
        if (m_gsdBadgeCache.value(meta.workingDirectory)) {
            displayName += QStringLiteral(" [GSD]");
        }
    }

    // Git branch badge (local sessions only, cached per working directory)
    if (!meta.workingDirectory.isEmpty() && !meta.isRemote) {
        if (!m_gitBranchCache.contains(meta.workingDirectory)) {
            // Insert placeholder to prevent duplicate async queries for the same dir
            m_gitBranchCache.insert(meta.workingDirectory, QString());

            // Async git query — updates cache and triggers tree refresh when done
            auto *git = new QProcess(this);
            git->setWorkingDirectory(meta.workingDirectory);
            QPointer<SessionManagerPanel> guard(this);
            QString workDir = meta.workingDirectory;
            connect(git, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [guard, git, workDir](int exitCode, QProcess::ExitStatus) {
                git->deleteLater();
                if (!guard) {
                    return;
                }
                if (exitCode == 0) {
                    QString branch = QString::fromUtf8(git->readAllStandardOutput()).trimmed();
                    guard->m_gitBranchCache[workDir] = branch;
                } else {
                    guard->m_gitBranchCache[workDir] = QString();
                }
                guard->scheduleTreeUpdate();
            });
            git->start(QStringLiteral("git"), {QStringLiteral("branch"), QStringLiteral("--show-current")});
        }
        const QString &branch = m_gitBranchCache[meta.workingDirectory];
        if (!branch.isEmpty() && branch != QStringLiteral("main") && branch != QStringLiteral("master")) {
            displayName += QStringLiteral(" [%1]").arg(branch);
        }
    }

    // Disambiguate when multiple sessions share the same directory in the same category
    if (hasSiblings && description.isEmpty() && meta.createdAt.isValid()) {
        displayName += QStringLiteral(" — %1").arg(meta.createdAt.toString(QStringLiteral("MMM d h:mmap")));
    }

    // Add @host suffix for remote sessions
    if (meta.isRemote && !meta.sshHost.isEmpty()) {
        displayName += QStringLiteral(" @%1").arg(meta.sshHost);
    }

    // Muted indicator
    if (m_mutedSessions.contains(meta.sessionId)) {
        displayName += QStringLiteral(" [muted]");
    }

    // For folder-grouped sessions, prepend lifecycle state indicator
    QString parentFolderId = parent->data(0, Qt::UserRole + 7).toString();
    if (!parentFolderId.isEmpty()) {
        bool tmuxAlive = m_cachedLiveNames.contains(meta.sessionName)
            || (meta.isRemote && m_cachedRemoteLiveNames.contains(meta.sessionName));
        if (isActive) {
            displayName = QStringLiteral("\u25B6 ") + displayName; // ▶ active
        } else if (tmuxAlive) {
            displayName = QStringLiteral("\u23F8 ") + displayName; // ⏸ detached
        } else if (meta.isArchived) {
            displayName = QStringLiteral("\U0001F4E6 ") + displayName; // 📦 archived
        } else {
            displayName = QStringLiteral("\u2716 ") + displayName; // ✖ closed
        }
    }

    item->setText(0, displayName);
    item->setData(0, Qt::UserRole, meta.sessionId);

    // Composite key for expansion state preservation
    QString sessionKey = QStringLiteral("s:%1").arg(meta.sessionId);
    item->setData(0, Qt::UserRole + 6, sessionKey);

    // Muted visual styling
    if (m_mutedSessions.contains(meta.sessionId)) {
        QFont f = item->font(0);
        f.setItalic(true);
        item->setFont(0, f);
        item->setForeground(0, QBrush(QColor(140, 140, 140)));
    }

    // Enhanced tooltip
    QString tooltip;
    if (meta.isRemote) {
        QString userHost = meta.sshUsername.isEmpty() ? meta.sshHost : QStringLiteral("%1@%2").arg(meta.sshUsername, meta.sshHost);
        tooltip =
            QStringLiteral("%1\nRemote: %2\nPath: %3\nLast accessed: %4").arg(meta.sessionName, userHost, meta.workingDirectory, meta.lastAccessed.toString());
    } else {
        tooltip = QStringLiteral("%1\n%2\nLast accessed: %3").arg(meta.sessionName, meta.workingDirectory, meta.lastAccessed.toString());
    }
    // Append git branch to tooltip (always, including main/master)
    if (m_gitBranchCache.contains(meta.workingDirectory) && !m_gitBranchCache[meta.workingDirectory].isEmpty()) {
        tooltip += QStringLiteral("\nBranch: %1").arg(m_gitBranchCache[meta.workingDirectory]);
    }
    item->setToolTip(0, tooltip);

    // Add yolo mode and approval count indicators in column 1 (always visible)
    if (isActive) {
        ClaudeSession *session = m_activeSessions[meta.sessionId];
        if (session) {
            // Build rich text: gold bolt [yolo count]  blue bolt [double count]  purple bolt [triple count]
            QString boltsHtml;
            int yoloCount = session->yoloApprovalCount();
            int doubleCount = session->doubleYoloApprovalCount();
            int tripleCount = session->tripleYoloApprovalCount();

            if (session->yoloMode() || yoloCount > 0) {
                boltsHtml += QStringLiteral("<span style='color:#FFB300'>ϟ</span>"); // Gold
                if (yoloCount > 0) {
                    boltsHtml += QStringLiteral("<span style='color:#FFB300'>[%1]</span>").arg(yoloCount);
                }
            }
            if (session->doubleYoloMode() || doubleCount > 0) {
                if (!boltsHtml.isEmpty()) {
                    boltsHtml += QStringLiteral(" ");
                }
                boltsHtml += QStringLiteral("<span style='color:#42A5F5'>ϟ</span>"); // Blue
                if (doubleCount > 0) {
                    boltsHtml += QStringLiteral("<span style='color:#42A5F5'>[%1]</span>").arg(doubleCount);
                }
            }
            if (session->tripleYoloMode() || tripleCount > 0) {
                if (!boltsHtml.isEmpty()) {
                    boltsHtml += QStringLiteral(" ");
                }
                boltsHtml += QStringLiteral("<span style='color:#AB47BC'>ϟ</span>"); // Purple
                if (tripleCount > 0) {
                    boltsHtml += QStringLiteral("<span style='color:#AB47BC'>[%1]</span>").arg(tripleCount);
                }
            }

            // Add velocity + budget ETA when budget controller is active
            if (auto *bc = session->budgetController()) {
                if (bc->budget().hasAnyLimit()) {
                    // Show budget progress: elapsed/limit time | $current/$ceiling
                    QString budgetInfo;
                    if (bc->budget().timeLimitMinutes > 0) {
                        int elapsed = bc->budget().elapsedMinutes();
                        budgetInfo += QStringLiteral(" %1/%2m").arg(elapsed).arg(bc->budget().timeLimitMinutes);
                    }
                    if (bc->budget().costCeilingUSD > 0.0) {
                        double cost = session->tokenUsage().estimatedCostUSD();
                        if (!budgetInfo.isEmpty()) {
                            budgetInfo += QStringLiteral(" |");
                        }
                        budgetInfo += QStringLiteral(" $%1/$%2").arg(cost, 0, 'f', 2).arg(bc->budget().costCeilingUSD, 0, 'f', 2);
                    }
                    if (!budgetInfo.isEmpty()) {
                        boltsHtml += QStringLiteral("<span style='color:gray; font-size:10px'>%1</span>").arg(budgetInfo);
                    }
                }
                // Show velocity if available
                const auto &vel = bc->velocity();
                if (vel.tokensPerMinute() > 0) {
                    boltsHtml += QStringLiteral("<br><span style='color:gray; font-size:9px'>%1</span>").arg(vel.formatVelocity());
                }
            }

            // Add observer warning badge
            if (auto *obs = session->sessionObserver()) {
                int severity = obs->composedSeverity();
                if (severity >= 5) {
                    boltsHtml += QStringLiteral(" <span style='color:#F44336'>⚠ CRITICAL</span>");
                } else if (severity >= 3) {
                    boltsHtml += QStringLiteral(" <span style='color:#FF9800'>⚠</span>");
                } else if (severity > 0) {
                    boltsHtml += QStringLiteral(" <span style='color:#FFC107'>⚠</span>");
                }
            }

            if (!boltsHtml.isEmpty()) {
                auto *label = new QLabel(boltsHtml);
                label->setTextFormat(Qt::RichText);
                m_treeWidget->setItemWidget(item, 1, label);
            }
        }
    }

    // Set icon based on state (remote sessions use network icons)
    if (meta.isArchived && meta.isExpired) {
        item->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-warning")));
    } else if (meta.isArchived) {
        item->setIcon(0, QIcon::fromTheme(QStringLiteral("folder-grey")));
    } else if (meta.isRemote) {
        // Remote sessions use network icons
        if (isActive) {
            item->setIcon(0, QIcon::fromTheme(QStringLiteral("network-server"), QIcon::fromTheme(QStringLiteral("folder-remote"))));
        } else {
            item->setIcon(0, QIcon::fromTheme(QStringLiteral("network-server"), QIcon::fromTheme(QStringLiteral("folder-remote"))));
        }
    } else if (isActive) {
        item->setIcon(0, QIcon::fromTheme(QStringLiteral("folder-open")));
    } else {
        // Detached but not archived
        item->setIcon(0, QIcon::fromTheme(QStringLiteral("folder")));
    }

    // Add status indicator (green for local, cyan for remote)
    if (isActive) {
        if (meta.isRemote) {
            item->setForeground(0, QBrush(Qt::cyan));
        } else {
            item->setForeground(0, QBrush(Qt::darkGreen));
        }
    }

    // Live activity line for active sessions
    if (isActive) {
        ClaudeSession *session = m_activeSessions[meta.sessionId];
        if (session && session->claudeProcess()) {
            QString activity;
            auto procState = session->claudeProcess()->state();
            if (procState == ClaudeProcess::State::Working) {
                activity = session->currentTask();
                if (activity.isEmpty()) {
                    activity = i18n("Working...");
                }
            } else if (procState == ClaudeProcess::State::WaitingInput) {
                activity = i18n("Waiting for input");
            } else if (procState == ClaudeProcess::State::Idle) {
                activity = i18n("Idle");
            } else if (procState == ClaudeProcess::State::Starting) {
                activity = i18n("Starting...");
            }
            if (!activity.isEmpty()) {
                auto *activityItem = new QTreeWidgetItem(item);
                activityItem->setText(0, activity);
                activityItem->setForeground(0, QBrush(QColor(0x75, 0x75, 0x75)));
                QFont f = activityItem->font(0);
                f.setItalic(true);
                f.setPointSizeF(f.pointSizeF() * 0.9);
                activityItem->setFont(0, f);
                activityItem->setFlags(Qt::NoItemFlags); // Not selectable/clickable
            }
        }
    }

    // Add nested subagent + subprocess children (from live session or persisted metadata)
    QMap<QString, SubagentInfo> subagentsMap;
    QMap<QString, SubprocessInfo> subprocessesMap;
    QMap<int, QString> promptLabels;
    bool isPersistedTree = false;

    if (isActive) {
        ClaudeSession *session = m_activeSessions[meta.sessionId];
        if (session) {
            // Take snapshots — live maps could be modified by async hook events
            subagentsMap = session->subagents();
            subprocessesMap = session->subprocesses();
            promptLabels = session->promptGroupLabels();
        }
    } else if (!meta.subagents.isEmpty() || !meta.subprocesses.isEmpty()) {
        // Use persisted snapshots from metadata
        isPersistedTree = true;
        for (const auto &agent : meta.subagents) {
            SubagentInfo a = agent;
            // Force all persisted agents to "done" state
            a.state = ClaudeProcess::State::NotRunning;
            subagentsMap[a.agentId] = a;
        }
        for (const auto &proc : meta.subprocesses) {
            subprocessesMap[proc.id] = proc;
        }
        promptLabels = meta.promptGroupLabels;
    }

    {
        bool hasItems = !subagentsMap.isEmpty() || !subprocessesMap.isEmpty();
        if (hasItems) {
            const auto subagents = subagentsMap;
            const auto subprocesses = subprocessesMap;
            // Don't hide completed items in persisted trees — everything is already done
            bool hideCompleted = !isPersistedTree && m_hideCompletedAgents.contains(meta.sessionId);

            // Helper: create a subagent tree item under a given parent
            auto addSubagentItem = [&](QTreeWidgetItem *parentItem, const SubagentInfo &info) -> QTreeWidgetItem * {
                auto *childItem = new QTreeWidgetItem(parentItem);

                // Display: agentType (teammateName) — taskSubject (truncated)
                QString childName = info.agentType;
                if (!info.teammateName.isEmpty()) {
                    childName = QStringLiteral("%1 (%2)").arg(info.agentType, info.teammateName);
                }
                if (!info.currentTaskSubject.isEmpty()) {
                    QString truncTask = info.currentTaskSubject;
                    if (truncTask.length() > 30) {
                        truncTask = truncTask.left(27) + QStringLiteral("...");
                    }
                    childName += QStringLiteral(" \u2014 %1").arg(truncTask);
                }
                childItem->setText(0, childName);

                // Store agentId and parent sessionId for context menu / double-click
                childItem->setData(0, Qt::UserRole, info.agentId);
                childItem->setData(0, Qt::UserRole + 1, meta.sessionId);

                // Elapsed duration label in column 1
                QString elapsed = formatElapsed(info.startedAt);
                if (!elapsed.isEmpty()) {
                    auto *durationLabel = new QLabel(elapsed);
                    durationLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 10px;"));
                    m_treeWidget->setItemWidget(childItem, 1, durationLabel);
                }

                // Icon and color by state
                if (info.state == ClaudeProcess::State::Working) {
                    childItem->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-start")));
                    childItem->setForeground(0, QBrush(Qt::darkGreen));
                } else if (info.state == ClaudeProcess::State::Idle) {
                    childItem->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-pause")));
                    childItem->setForeground(0, QBrush(Qt::gray));
                } else {
                    childItem->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-ok"), QIcon::fromTheme(QStringLiteral("task-complete"))));
                    childItem->setForeground(0, QBrush(QColor(140, 140, 140)));
                }

                // Enhanced tooltip
                QString childTooltip = QStringLiteral("Agent: %1\nID: %2").arg(info.agentType, info.agentId);
                if (!info.teammateName.isEmpty()) {
                    childTooltip += QStringLiteral("\nName: %1").arg(info.teammateName);
                }
                if (!info.taskDescription.isEmpty()) {
                    childTooltip += QStringLiteral("\nTask: %1").arg(info.taskDescription);
                }
                if (!info.currentTaskSubject.isEmpty()) {
                    childTooltip += QStringLiteral("\nSubject: %1").arg(info.currentTaskSubject);
                }
                if (info.startedAt.isValid()) {
                    childTooltip += QStringLiteral("\nElapsed: %1").arg(formatElapsed(info.startedAt));
                }
                if (!info.transcriptPath.isEmpty()) {
                    childTooltip += QStringLiteral("\nTranscript: %1").arg(info.transcriptPath);
                }
                bool completed = (info.state != ClaudeProcess::State::Working && info.state != ClaudeProcess::State::Idle);
                if (completed) {
                    childTooltip += QStringLiteral("\nStatus: Completed");
                }
                childItem->setToolTip(0, childTooltip);
                childItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                return childItem;
            };

            // Helper: create a subprocess tree item under a given parent
            auto addSubprocessItem = [&](QTreeWidgetItem *parentItem, const SubprocessInfo &info) -> QTreeWidgetItem * {
                auto *childItem = new QTreeWidgetItem(parentItem);

                childItem->setText(0, info.command);

                // Store subprocess ID and parent sessionId for context menu / double-click
                childItem->setData(0, Qt::UserRole + 4, info.id); // subprocess ID
                childItem->setData(0, Qt::UserRole + 1, meta.sessionId);

                // Elapsed duration + resource stats in column 1
                QString col1;
                QString elapsed = formatElapsed(info.startedAt);
                if (!elapsed.isEmpty()) {
                    col1 = elapsed;
                }
                if (info.resourceUsage.rssBytes > 0 || info.resourceUsage.cpuPercent > 0.0) {
                    if (!col1.isEmpty())
                        col1 += QStringLiteral(" ");
                    col1 += info.resourceUsage.formatCompact();
                }
                if (!col1.isEmpty()) {
                    auto *statsLabel = new QLabel(col1);
                    statsLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 10px;"));
                    m_treeWidget->setItemWidget(childItem, 1, statsLabel);
                }

                // Icon and color by status
                if (info.status == SubprocessInfo::Running) {
                    childItem->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-start")));
                    childItem->setForeground(0, QBrush(Qt::darkGreen));
                } else if (info.status == SubprocessInfo::Failed) {
                    childItem->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-error")));
                    childItem->setForeground(0, QBrush(Qt::red));
                } else {
                    childItem->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-ok"), QIcon::fromTheme(QStringLiteral("task-complete"))));
                    childItem->setForeground(0, QBrush(QColor(140, 140, 140)));
                }

                // Tooltip
                QString procTip = QStringLiteral("Command: %1").arg(info.fullCommand);
                if (info.pid > 0) {
                    procTip += QStringLiteral("\nPID: %1").arg(info.pid);
                }
                if (info.startedAt.isValid()) {
                    procTip += QStringLiteral("\nStarted: %1").arg(info.startedAt.toString(Qt::ISODate));
                }
                if (info.finishedAt.isValid()) {
                    procTip += QStringLiteral("\nFinished: %1").arg(info.finishedAt.toString(Qt::ISODate));
                }
                if (info.exitCode >= 0 && info.status != SubprocessInfo::Running) {
                    procTip += QStringLiteral("\nExit code: %1").arg(info.exitCode);
                }
                childItem->setToolTip(0, procTip);
                childItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                return childItem;
            };

            // Collect all items per prompt round
            QSet<int> promptRounds;
            for (auto it = subagents.constBegin(); it != subagents.constEnd(); ++it) {
                promptRounds.insert(it->promptGroupId);
            }
            for (auto it = subprocesses.constBegin(); it != subprocesses.constEnd(); ++it) {
                // Filter: skip completed subprocesses that ran < 2 seconds (instant commands)
                if (it->status != SubprocessInfo::Running) {
                    if (it->startedAt.isValid() && it->finishedAt.isValid() && it->startedAt.secsTo(it->finishedAt) < 2) {
                        continue;
                    }
                }
                promptRounds.insert(it->promptGroupId);
            }

            bool multipleRounds = promptRounds.size() > 1;
            QList<int> sortedRounds = promptRounds.values();
            std::sort(sortedRounds.begin(), sortedRounds.end());

            bool hasActiveItems = false;

            for (int round : sortedRounds) {
                // Collect agents and subprocesses for this round
                QList<const SubagentInfo *> roundAgents;
                for (auto it = subagents.constBegin(); it != subagents.constEnd(); ++it) {
                    if (it->promptGroupId != round)
                        continue;
                    bool completed = (it->state != ClaudeProcess::State::Working && it->state != ClaudeProcess::State::Idle);
                    if (completed && hideCompleted)
                        continue;
                    if (!completed)
                        hasActiveItems = true;
                    roundAgents.append(&it.value());
                }

                QList<const SubprocessInfo *> roundProcs;
                for (auto it = subprocesses.constBegin(); it != subprocesses.constEnd(); ++it) {
                    if (it->promptGroupId != round)
                        continue;
                    // Filter instant commands
                    if (it->status != SubprocessInfo::Running) {
                        if (it->startedAt.isValid() && it->finishedAt.isValid() && it->startedAt.secsTo(it->finishedAt) < 2) {
                            continue;
                        }
                        if (hideCompleted)
                            continue;
                    } else {
                        hasActiveItems = true;
                    }
                    roundProcs.append(&it.value());
                }

                if (roundAgents.isEmpty() && roundProcs.isEmpty())
                    continue;

                // Sort agents: Working first, Idle second, NotRunning last
                auto stateRank = [](ClaudeProcess::State s) -> int {
                    if (s == ClaudeProcess::State::Working)
                        return 0;
                    if (s == ClaudeProcess::State::Idle)
                        return 1;
                    return 2;
                };
                std::sort(roundAgents.begin(), roundAgents.end(), [&stateRank](const SubagentInfo *a, const SubagentInfo *b) {
                    return stateRank(a->state) < stateRank(b->state);
                });

                // Compute aggregate state for this round (used by both prompt group and subtasks nodes)
                bool roundAnyWorking = false, roundAnyIdle = false;
                for (const auto *a : roundAgents) {
                    if (a->state == ClaudeProcess::State::Working)
                        roundAnyWorking = true;
                    if (a->state == ClaudeProcess::State::Idle)
                        roundAnyIdle = true;
                }
                for (const auto *p : roundProcs) {
                    if (p->status == SubprocessInfo::Running)
                        roundAnyWorking = true;
                }

                // Determine parent for this round's items
                QTreeWidgetItem *roundParent = item;
                if (multipleRounds) {
                    // Create prompt group node
                    auto *promptItem = new QTreeWidgetItem(item);
                    int totalItems = roundAgents.size() + roundProcs.size();
                    QString label = promptLabels.value(round, QStringLiteral("Prompt #%1").arg(round + 1));
                    promptItem->setText(0, QStringLiteral("%1 (%2 items)").arg(label).arg(totalItems));

                    // Store prompt group ID for context menu detection
                    promptItem->setData(0, Qt::UserRole + 3, round);
                    promptItem->setData(0, Qt::UserRole + 1, meta.sessionId);
                    promptItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

                    // Composite key for expansion state preservation
                    QString pgKey = QStringLiteral("pg:%1:%2").arg(meta.sessionId).arg(round);
                    promptItem->setData(0, Qt::UserRole + 6, pgKey);

                    if (roundAnyWorking) {
                        promptItem->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-start")));
                        promptItem->setForeground(0, QBrush(Qt::darkGreen));
                    } else if (roundAnyIdle) {
                        promptItem->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-pause")));
                        promptItem->setForeground(0, QBrush(Qt::gray));
                    } else {
                        promptItem->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-ok"), QIcon::fromTheme(QStringLiteral("task-complete"))));
                        promptItem->setForeground(0, QBrush(QColor(140, 140, 140)));
                    }

                    promptItem->setExpanded(shouldAutoExpand(pgKey, meta.sessionId, roundAnyWorking || roundAnyIdle));
                    m_knownItems.insert(pgKey);
                    roundParent = promptItem;
                }

                // Create "Subtasks" container node — collapsible wrapper for agents and processes
                auto *subtasksItem = new QTreeWidgetItem(roundParent);
                int totalItems = roundAgents.size() + roundProcs.size();
                subtasksItem->setText(0, QStringLiteral("Subtasks (%1)").arg(totalItems));
                subtasksItem->setData(0, Qt::UserRole + 5, QStringLiteral("subtasks"));
                subtasksItem->setData(0, Qt::UserRole + 1, meta.sessionId);
                subtasksItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

                // Composite key for expansion state preservation
                QString stKey = QStringLiteral("st:%1:%2").arg(meta.sessionId).arg(round);
                subtasksItem->setData(0, Qt::UserRole + 6, stKey);

                if (roundAnyWorking) {
                    subtasksItem->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-start")));
                    subtasksItem->setForeground(0, QBrush(Qt::darkGreen));
                } else if (roundAnyIdle) {
                    subtasksItem->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-pause")));
                    subtasksItem->setForeground(0, QBrush(Qt::gray));
                } else {
                    subtasksItem->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-ok"), QIcon::fromTheme(QStringLiteral("task-complete"))));
                    subtasksItem->setForeground(0, QBrush(QColor(140, 140, 140)));
                }

                // Smart expand: preserve user state, auto-expand only new items
                subtasksItem->setExpanded(shouldAutoExpand(stKey, meta.sessionId, roundAnyWorking || roundAnyIdle));
                m_knownItems.insert(stKey);

                // Group agents by taskDescription within this round
                QMap<QString, QList<const SubagentInfo *>> groups;
                QStringList groupOrder;
                for (const auto *agentInfo : roundAgents) {
                    const QString &key = agentInfo->taskDescription;
                    if (!groups.contains(key)) {
                        groupOrder.append(key);
                    }
                    groups[key].append(agentInfo);
                }

                // Sort groups: groups with active agents first
                std::sort(groupOrder.begin(), groupOrder.end(), [&groups, &stateRank](const QString &a, const QString &b) {
                    int bestA = 2, bestB = 2;
                    for (const auto *info : groups[a]) {
                        bestA = qMin(bestA, stateRank(info->state));
                    }
                    for (const auto *info : groups[b]) {
                        bestB = qMin(bestB, stateRank(info->state));
                    }
                    return bestA < bestB;
                });

                for (const QString &groupKey : std::as_const(groupOrder)) {
                    const auto &agentList = groups[groupKey];

                    if (groupKey.isEmpty()) {
                        // Ungrouped agents: add flat under subtasks
                        for (const auto *info : agentList) {
                            addSubagentItem(subtasksItem, *info);
                        }
                    } else if (agentList.size() == 1) {
                        // Single agent in group: show description inline
                        auto *childItem = addSubagentItem(subtasksItem, *agentList.first());
                        QString existingText = childItem->text(0);
                        childItem->setText(0, QStringLiteral("%1 \u2014 %2").arg(groupKey, existingText));
                    } else {
                        // Multiple agents: create task group node under subtasks
                        auto *groupItem = new QTreeWidgetItem(subtasksItem);

                        bool groupAnyWorking = false, groupAnyIdle = false;
                        for (const auto *info : agentList) {
                            if (info->state == ClaudeProcess::State::Working)
                                groupAnyWorking = true;
                            if (info->state == ClaudeProcess::State::Idle)
                                groupAnyIdle = true;
                        }

                        groupItem->setText(0, QStringLiteral("%1 (%2 agents)").arg(groupKey).arg(agentList.size()));
                        groupItem->setData(0, Qt::UserRole + 2, groupKey);
                        groupItem->setData(0, Qt::UserRole + 1, meta.sessionId);
                        groupItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

                        // Composite key for expansion state preservation
                        QString tgKey = QStringLiteral("tg:%1:%2").arg(meta.sessionId).arg(qHash(groupKey));
                        groupItem->setData(0, Qt::UserRole + 6, tgKey);

                        if (groupAnyWorking) {
                            groupItem->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-start")));
                            groupItem->setForeground(0, QBrush(Qt::darkGreen));
                        } else if (groupAnyIdle) {
                            groupItem->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-pause")));
                            groupItem->setForeground(0, QBrush(Qt::gray));
                        } else {
                            groupItem->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-ok"), QIcon::fromTheme(QStringLiteral("task-complete"))));
                            groupItem->setForeground(0, QBrush(QColor(140, 140, 140)));
                        }

                        groupItem->setToolTip(0, QStringLiteral("Task: %1\nAgents: %2").arg(groupKey).arg(agentList.size()));
                        groupItem->setExpanded(shouldAutoExpand(tgKey, meta.sessionId, groupAnyWorking || groupAnyIdle));
                        m_knownItems.insert(tgKey);

                        for (const auto *info : agentList) {
                            addSubagentItem(groupItem, *info);
                        }
                    }
                }

                // Add subprocess items for this round under subtasks
                for (const auto *procInfo : roundProcs) {
                    addSubprocessItem(subtasksItem, *procInfo);
                }
            }

            // Smart expand: preserve user state, auto-expand only new items
            item->setExpanded(shouldAutoExpand(sessionKey, meta.sessionId, hasActiveItems));
            m_knownItems.insert(sessionKey);

            if (hasActiveItems) {
                // Start duration timer if not already running
                if (!m_durationTimer) {
                    m_durationTimer = new QTimer(this);
                    m_durationTimer->setInterval(10000); // 10 seconds
                    // In-place label update instead of full tree rebuild
                    connect(m_durationTimer, &QTimer::timeout, this, &SessionManagerPanel::updateDurationLabels);
                }
                if (!m_durationTimer->isActive()) {
                    m_durationTimer->start();
                }
            }
        }
    }
}

void SessionManagerPanel::loadMetadata()
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataPath);
    QString filePath = dataPath + QStringLiteral("/sessions.json");

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "SessionManagerPanel::loadMetadata: JSON parse error at offset" << parseError.offset << ":" << parseError.errorString() << "in"
                   << filePath;
        return;
    }
    if (!doc.isArray()) {
        qWarning() << "SessionManagerPanel::loadMetadata: Expected JSON array in" << filePath;
        return;
    }

    QJsonArray array = doc.array();
    for (const auto &value : array) {
        QJsonObject obj = value.toObject();
        SessionMetadata meta;
        meta.sessionId = obj[QStringLiteral("sessionId")].toString();
        meta.sessionName = obj[QStringLiteral("sessionName")].toString();
        meta.profileName = obj[QStringLiteral("profileName")].toString();
        meta.workingDirectory = obj[QStringLiteral("workingDirectory")].toString();
        meta.isPinned = obj[QStringLiteral("isPinned")].toBool();
        meta.isArchived = obj[QStringLiteral("isArchived")].toBool();
        meta.isExpired = obj[QStringLiteral("isExpired")].toBool();
        meta.isDismissed = obj[QStringLiteral("isDismissed")].toBool();
        meta.lastAccessed = QDateTime::fromString(obj[QStringLiteral("lastAccessed")].toString(), Qt::ISODate);
        meta.createdAt = QDateTime::fromString(obj[QStringLiteral("createdAt")].toString(), Qt::ISODate);

        // SSH remote session fields
        meta.isRemote = obj[QStringLiteral("isRemote")].toBool();
        meta.sshHost = obj[QStringLiteral("sshHost")].toString();
        meta.sshUsername = obj[QStringLiteral("sshUsername")].toString();
        meta.sshPort = obj[QStringLiteral("sshPort")].toInt(22);

        // Per-session yolo mode settings
        meta.yoloMode = obj[QStringLiteral("yoloMode")].toBool();
        meta.doubleYoloMode = obj[QStringLiteral("doubleYoloMode")].toBool();
        meta.tripleYoloMode = obj[QStringLiteral("tripleYoloMode")].toBool();

        // Approval counts
        meta.yoloApprovalCount = obj[QStringLiteral("yoloApprovalCount")].toInt();
        meta.doubleYoloApprovalCount = obj[QStringLiteral("doubleYoloApprovalCount")].toInt();
        meta.tripleYoloApprovalCount = obj[QStringLiteral("tripleYoloApprovalCount")].toInt();

        // Approval log
        const QJsonArray logArray = obj[QStringLiteral("approvalLog")].toArray();
        for (const auto &logVal : logArray) {
            QJsonObject logObj = logVal.toObject();
            ApprovalLogEntry entry;
            entry.timestamp = QDateTime::fromString(logObj[QStringLiteral("time")].toString(), Qt::ISODate);
            entry.toolName = logObj[QStringLiteral("tool")].toString();
            entry.action = logObj[QStringLiteral("action")].toString();
            entry.yoloLevel = logObj[QStringLiteral("level")].toInt();
            entry.totalTokens = static_cast<quint64>(logObj[QStringLiteral("tokens")].toInteger(0));
            entry.estimatedCostUSD = logObj[QStringLiteral("cost")].toDouble();
            entry.toolInput = logObj[QStringLiteral("input")].toString();
            entry.toolOutput = logObj[QStringLiteral("output")].toString();
            meta.approvalLog.append(entry);
        }

        // Resume session ID, description, and agent linkage
        meta.lastResumeSessionId = obj[QStringLiteral("lastResumeSessionId")].toString();
        meta.description = obj[QStringLiteral("description")].toString();
        meta.agentId = obj[QStringLiteral("agentId")].toString();
        meta.folderId = obj[QStringLiteral("folderId")].toString();

        // Budget settings
        meta.budgetTimeLimitMinutes = obj[QStringLiteral("budgetTimeLimitMinutes")].toInt();
        meta.budgetCostCeilingUSD = obj[QStringLiteral("budgetCostCeilingUSD")].toDouble();
        meta.budgetTokenCeiling = static_cast<quint64>(obj[QStringLiteral("budgetTokenCeiling")].toInteger(0));

        // Subagent/subprocess snapshots
        if (obj.contains(QStringLiteral("subagents"))) {
            const QJsonArray agentArray = obj[QStringLiteral("subagents")].toArray();
            for (const auto &val : agentArray) {
                meta.subagents.append(SubagentInfo::fromJson(val.toObject()));
            }
        }
        if (obj.contains(QStringLiteral("subprocesses"))) {
            const QJsonArray procArray = obj[QStringLiteral("subprocesses")].toArray();
            for (const auto &val : procArray) {
                meta.subprocesses.append(SubprocessInfo::fromJson(val.toObject()));
            }
        }
        if (obj.contains(QStringLiteral("promptLabels"))) {
            const QJsonObject labelsObj = obj[QStringLiteral("promptLabels")].toObject();
            for (auto it = labelsObj.constBegin(); it != labelsObj.constEnd(); ++it) {
                meta.promptGroupLabels[it.key().toInt()] = it.value().toString();
            }
        }
        meta.currentPromptRound = obj[QStringLiteral("promptRound")].toInt(0);

        if (!meta.sessionId.isEmpty() && !meta.sessionName.isEmpty()) {
            m_metadata[meta.sessionId] = meta;
        }
    }
}

void SessionManagerPanel::saveMetadata(bool sync)
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataPath);
    QString filePath = dataPath + QStringLiteral("/sessions.json");

    QJsonArray array;
    for (auto &meta : m_metadata) {
        // Snapshot live session data into metadata before serializing
        // Use QPointer to safely detect if the session was deleted between
        // the map lookup and the dereference (e.g., during archiveSession).
        if (m_activeSessions.contains(meta.sessionId)) {
            QPointer<ClaudeSession> session = m_activeSessions[meta.sessionId];
            if (session) {
                meta.subagents = session->subagents().values().toVector();
                meta.subprocesses = session->subprocesses().values().toVector();
                meta.promptGroupLabels = session->promptGroupLabels();
                meta.currentPromptRound = session->currentPromptRound();
            }
        }

        QJsonObject obj;
        obj[QStringLiteral("sessionId")] = meta.sessionId;
        obj[QStringLiteral("sessionName")] = meta.sessionName;
        obj[QStringLiteral("profileName")] = meta.profileName;
        obj[QStringLiteral("workingDirectory")] = meta.workingDirectory;
        obj[QStringLiteral("isPinned")] = meta.isPinned;
        obj[QStringLiteral("isArchived")] = meta.isArchived;
        obj[QStringLiteral("isExpired")] = meta.isExpired;
        obj[QStringLiteral("lastAccessed")] = meta.lastAccessed.toString(Qt::ISODate);
        obj[QStringLiteral("createdAt")] = meta.createdAt.toString(Qt::ISODate);

        // SSH remote session fields
        if (meta.isRemote) {
            obj[QStringLiteral("isRemote")] = true;
            obj[QStringLiteral("sshHost")] = meta.sshHost;
            obj[QStringLiteral("sshUsername")] = meta.sshUsername;
            obj[QStringLiteral("sshPort")] = meta.sshPort;
        }

        // Per-session yolo mode settings (only save if enabled to keep JSON clean)
        if (meta.yoloMode) {
            obj[QStringLiteral("yoloMode")] = true;
        }
        if (meta.doubleYoloMode) {
            obj[QStringLiteral("doubleYoloMode")] = true;
        }
        if (meta.tripleYoloMode) {
            obj[QStringLiteral("tripleYoloMode")] = true;
        }
        if (meta.isDismissed) {
            obj[QStringLiteral("isDismissed")] = true;
        }

        // Approval counts (only save if non-zero)
        int totalApprovals = meta.yoloApprovalCount + meta.doubleYoloApprovalCount + meta.tripleYoloApprovalCount;
        if (totalApprovals > 0) {
            obj[QStringLiteral("yoloApprovalCount")] = meta.yoloApprovalCount;
            obj[QStringLiteral("doubleYoloApprovalCount")] = meta.doubleYoloApprovalCount;
            obj[QStringLiteral("tripleYoloApprovalCount")] = meta.tripleYoloApprovalCount;

            // Approval log (cap at 500 most recent entries to keep JSON manageable)
            if (!meta.approvalLog.isEmpty()) {
                QJsonArray logArray;
                int startIdx = qMax(0, meta.approvalLog.size() - 500);
                for (int i = startIdx; i < meta.approvalLog.size(); ++i) {
                    const auto &entry = meta.approvalLog[i];
                    QJsonObject logObj;
                    logObj[QStringLiteral("time")] = entry.timestamp.toString(Qt::ISODate);
                    logObj[QStringLiteral("tool")] = entry.toolName;
                    logObj[QStringLiteral("action")] = entry.action;
                    logObj[QStringLiteral("level")] = entry.yoloLevel;
                    if (entry.totalTokens > 0) {
                        logObj[QStringLiteral("tokens")] = static_cast<double>(entry.totalTokens);
                        logObj[QStringLiteral("cost")] = entry.estimatedCostUSD;
                    }
                    if (!entry.toolInput.isEmpty()) {
                        logObj[QStringLiteral("input")] = entry.toolInput;
                    }
                    if (!entry.toolOutput.isEmpty()) {
                        logObj[QStringLiteral("output")] = entry.toolOutput;
                    }
                    logArray.append(logObj);
                }
                obj[QStringLiteral("approvalLog")] = logArray;
            }
        }

        // Resume session ID and description (only save if non-empty)
        if (!meta.lastResumeSessionId.isEmpty()) {
            obj[QStringLiteral("lastResumeSessionId")] = meta.lastResumeSessionId;
        }
        if (!meta.description.isEmpty()) {
            obj[QStringLiteral("description")] = meta.description;
        }
        if (!meta.agentId.isEmpty()) {
            obj[QStringLiteral("agentId")] = meta.agentId;
        }
        if (!meta.folderId.isEmpty()) {
            obj[QStringLiteral("folderId")] = meta.folderId;
        }

        // Budget settings (only save if any limit is set)
        if (meta.budgetTimeLimitMinutes > 0) {
            obj[QStringLiteral("budgetTimeLimitMinutes")] = meta.budgetTimeLimitMinutes;
        }
        if (meta.budgetCostCeilingUSD > 0.0) {
            obj[QStringLiteral("budgetCostCeilingUSD")] = meta.budgetCostCeilingUSD;
        }
        if (meta.budgetTokenCeiling > 0) {
            obj[QStringLiteral("budgetTokenCeiling")] = static_cast<double>(meta.budgetTokenCeiling);
        }

        // Subagent/subprocess snapshots (only write if non-empty)
        if (!meta.subagents.isEmpty()) {
            QJsonArray agentArray;
            for (const auto &agent : meta.subagents) {
                agentArray.append(agent.toJson());
            }
            obj[QStringLiteral("subagents")] = agentArray;
        }
        if (!meta.subprocesses.isEmpty()) {
            QJsonArray procArray;
            for (const auto &proc : meta.subprocesses) {
                procArray.append(proc.toJson());
            }
            obj[QStringLiteral("subprocesses")] = procArray;
        }
        if (!meta.promptGroupLabels.isEmpty()) {
            QJsonObject labelsObj;
            for (auto it = meta.promptGroupLabels.constBegin(); it != meta.promptGroupLabels.constEnd(); ++it) {
                labelsObj[QString::number(it.key())] = it.value();
            }
            obj[QStringLiteral("promptLabels")] = labelsObj;
        }
        if (meta.currentPromptRound > 0) {
            obj[QStringLiteral("promptRound")] = meta.currentPromptRound;
        }

        array.append(obj);
    }

    // Serialize + write: async by default (avoids blocking UI for 1MB+ files),
    // but synchronous during destruction to prevent races with the next operation.
    QJsonDocument doc(array);
    auto writeFile = [filePath, doc]() {
        QByteArray json = doc.toJson();
        QFile file(filePath);
        if (file.open(QIODevice::WriteOnly)) {
            qint64 written = file.write(json);
            if (written != json.size()) {
                qWarning() << "SessionManagerPanel::saveMetadata: Incomplete write —" << written << "of" << json.size() << "bytes to" << filePath;
            }
            file.close();
        } else {
            qWarning() << "SessionManagerPanel::saveMetadata: Failed to open" << filePath << "for writing:" << file.errorString();
        }
    };
    if (sync) {
        writeFile();
    } else {
        (void)QtConcurrent::run(writeFile);
    }

    Q_EMIT usageAggregateChanged();
}

void SessionManagerPanel::loadFolders()
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString filePath = dataPath + QStringLiteral("/session_folders.json");

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        qWarning() << "SessionManagerPanel::loadFolders: parse error in" << filePath;
        return;
    }

    QJsonArray array = doc.array();
    for (const auto &value : array) {
        QJsonObject obj = value.toObject();
        SessionFolderInfo folder;
        folder.folderId = obj[QStringLiteral("folderId")].toString();
        folder.name = obj[QStringLiteral("name")].toString();
        folder.color = QColor(obj[QStringLiteral("color")].toString());
        folder.createdAt = QDateTime::fromString(obj[QStringLiteral("createdAt")].toString(), Qt::ISODate);
        folder.sortOrder = obj[QStringLiteral("sortOrder")].toInt();
        if (!folder.folderId.isEmpty() && !folder.name.isEmpty()) {
            m_folders[folder.folderId] = folder;
        }
    }
}

void SessionManagerPanel::saveFolders()
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataPath);
    QString filePath = dataPath + QStringLiteral("/session_folders.json");

    QJsonArray array;
    for (const auto &folder : std::as_const(m_folders)) {
        QJsonObject obj;
        obj[QStringLiteral("folderId")] = folder.folderId;
        obj[QStringLiteral("name")] = folder.name;
        if (folder.color.isValid()) {
            obj[QStringLiteral("color")] = folder.color.name();
        }
        obj[QStringLiteral("createdAt")] = folder.createdAt.toString(Qt::ISODate);
        obj[QStringLiteral("sortOrder")] = folder.sortOrder;
        array.append(obj);
    }

    QJsonDocument doc(array);
    QByteArray json = doc.toJson(QJsonDocument::Indented);
    (void)QtConcurrent::run([filePath, json]() {
        QFile file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(json);
            file.close();
        } else {
            qWarning() << "SessionManagerPanel::saveFolders: Failed to write" << filePath;
        }
    });
}

void SessionManagerPanel::createFolder(const QString &name, const QColor &color)
{
    SessionFolderInfo folder;
    folder.folderId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    folder.name = name;
    folder.color = color;
    folder.createdAt = QDateTime::currentDateTime();
    // Place new folder after existing ones
    int maxOrder = 0;
    for (const auto &f : std::as_const(m_folders)) {
        maxOrder = qMax(maxOrder, f.sortOrder);
    }
    folder.sortOrder = maxOrder + 1;
    m_folders[folder.folderId] = folder;

    saveFolders();
    scheduleTreeUpdate();
    Q_EMIT foldersChanged();
}

void SessionManagerPanel::renameFolder(const QString &folderId, const QString &newName)
{
    if (!m_folders.contains(folderId) || newName.isEmpty()) {
        return;
    }
    m_folders[folderId].name = newName;
    saveFolders();
    scheduleTreeUpdate();
    Q_EMIT foldersChanged();
}

void SessionManagerPanel::setFolderColor(const QString &folderId, const QColor &color)
{
    if (!m_folders.contains(folderId)) {
        return;
    }
    m_folders[folderId].color = color;
    saveFolders();
    scheduleTreeUpdate();
    Q_EMIT foldersChanged();
}

void SessionManagerPanel::deleteFolder(const QString &folderId)
{
    if (!m_folders.contains(folderId)) {
        return;
    }
    // Ungroup all member sessions
    for (auto &meta : m_metadata) {
        if (meta.folderId == folderId) {
            meta.folderId.clear();
        }
    }
    m_folders.remove(folderId);

    saveFolders();
    scheduleMetadataSave();
    scheduleTreeUpdate();
    Q_EMIT foldersChanged();
}

void SessionManagerPanel::moveSessionToFolder(const QString &sessionId, const QString &folderId)
{
    auto *meta = findMetadata(sessionId);
    if (!meta || !m_folders.contains(folderId)) {
        return;
    }
    meta->folderId = folderId;
    scheduleMetadataSave();
    scheduleTreeUpdate();
}

void SessionManagerPanel::removeSessionFromFolder(const QString &sessionId)
{
    auto *meta = findMetadata(sessionId);
    if (!meta || meta->folderId.isEmpty()) {
        return;
    }
    meta->folderId.clear();
    scheduleMetadataSave();
    scheduleTreeUpdate();
}

QList<SessionFolderInfo> SessionManagerPanel::allFolders() const
{
    return m_folders.values();
}

// Compute incremental cost within a time window from cumulative approval log entries.
// Each entry's estimatedCostUSD is cumulative for the session, so we need the delta
// between the last entry in the window and the last entry before the window.
static double periodCostFromLog(const QVector<ApprovalLogEntry> &log, const QDateTime &windowStart)
{
    if (log.isEmpty()) {
        return 0.0;
    }

    // Find the last entry in the window and the last entry before the window
    double lastInWindow = -1.0;
    double lastBeforeWindow = 0.0;
    bool hasEntryInWindow = false;

    for (const auto &entry : log) {
        if (entry.timestamp < windowStart) {
            lastBeforeWindow = entry.estimatedCostUSD;
        } else {
            lastInWindow = entry.estimatedCostUSD;
            hasEntryInWindow = true;
        }
    }

    if (!hasEntryInWindow) {
        return 0.0;
    }

    return lastInWindow - lastBeforeWindow;
}

double SessionManagerPanel::weeklySpentUSD() const
{
    QDateTime now = QDateTime::currentDateTime();
    QDate today = now.date();
    // dayOfWeek: 1=Mon .. 7=Sun
    QDate weekStart = today.addDays(-(today.dayOfWeek() - 1));
    QDateTime windowStart(weekStart, QTime(0, 0, 0));

    double total = 0.0;
    for (const auto &meta : m_metadata) {
        total += periodCostFromLog(meta.approvalLog, windowStart);
    }
    return total;
}

double SessionManagerPanel::monthlySpentUSD() const
{
    QDateTime now = QDateTime::currentDateTime();
    QDate monthStart(now.date().year(), now.date().month(), 1);
    QDateTime windowStart(monthStart, QTime(0, 0, 0));

    double total = 0.0;
    for (const auto &meta : m_metadata) {
        total += periodCostFromLog(meta.approvalLog, windowStart);
    }
    return total;
}

SessionMetadata *SessionManagerPanel::findMetadata(const QString &sessionId)
{
    if (m_metadata.contains(sessionId)) {
        return &m_metadata[sessionId];
    }
    return nullptr;
}

QTreeWidgetItem *SessionManagerPanel::findTreeItem(const QString &sessionId)
{
    for (int i = 0; i < m_treeWidget->topLevelItemCount(); ++i) {
        QTreeWidgetItem *category = m_treeWidget->topLevelItem(i);
        for (int j = 0; j < category->childCount(); ++j) {
            QTreeWidgetItem *item = category->child(j);
            if (item->data(0, Qt::UserRole).toString() == sessionId) {
                return item;
            }
        }
    }
    return nullptr;
}

void SessionManagerPanel::showApprovalLog(ClaudeSession *session)
{
    if (!session) {
        return;
    }

    const auto &log = session->approvalLog();

    QDialog dialog(this);
    dialog.setWindowTitle(i18n("Approval Log - %1", QDir(session->workingDirectory()).dirName()));
    auto *layout = new QVBoxLayout(&dialog);

    auto *summary = new QLabel(i18n("Total auto-approvals: %1 (Yolo: %2, Double: %3, Triple: %4)",
                                    session->totalApprovalCount(),
                                    session->yoloApprovalCount(),
                                    session->doubleYoloApprovalCount(),
                                    session->tripleYoloApprovalCount()),
                               &dialog);
    layout->addWidget(summary);

    auto *splitter = new QSplitter(Qt::Vertical, &dialog);

    auto *tree = new QTreeWidget(splitter);
    tree->setHeaderLabels({i18n("Time"), i18n("Tool"), i18n("Action"), i18n("Level"), i18n("Tokens"), i18n("Cost")});
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setSortingEnabled(true);

    // Show most recent first
    for (int i = log.size() - 1; i >= 0; --i) {
        const auto &entry = log[i];
        auto *item = new QTreeWidgetItem(tree);
        item->setText(0, entry.timestamp.toString(QStringLiteral("hh:mm:ss")));
        item->setText(1, entry.toolName);
        item->setText(2, entry.action);
        QString levelStr;
        if (entry.yoloLevel == 1) {
            levelStr = QStringLiteral("Yolo");
        } else if (entry.yoloLevel == 2) {
            levelStr = QStringLiteral("Double");
        } else if (entry.yoloLevel == 3) {
            levelStr = QStringLiteral("Triple");
        }
        item->setText(3, levelStr);
        if (entry.totalTokens > 0) {
            // Format tokens compactly: "12.3K", "1.2M"
            double t = static_cast<double>(entry.totalTokens);
            QString tokenStr = t >= 1000000.0 ? QStringLiteral("%1M").arg(t / 1000000.0, 0, 'f', 1)
                : t >= 1000.0                 ? QStringLiteral("%1K").arg(t / 1000.0, 0, 'f', 1)
                                              : QString::number(entry.totalTokens);
            item->setText(4, tokenStr);
            item->setText(5, QStringLiteral("$%1").arg(entry.estimatedCostUSD, 0, 'f', 4));
            item->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(5, Qt::AlignRight | Qt::AlignVCenter);
            // Store raw values for correct numeric sorting
            item->setData(4, Qt::UserRole, static_cast<qulonglong>(entry.totalTokens));
            item->setData(5, Qt::UserRole, entry.estimatedCostUSD);
        }
        // Store original log index for detail lookup
        item->setData(0, Qt::UserRole + 1, i);
        // Store yolo level for numeric sorting
        item->setData(3, Qt::UserRole, entry.yoloLevel);
    }

    // Default sort by time descending (most recent first)
    tree->sortByColumn(0, Qt::DescendingOrder);

    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);

    auto *detailEdit = new QPlainTextEdit(splitter);
    detailEdit->setReadOnly(true);
    detailEdit->setPlaceholderText(i18n("Select an entry above to view tool input/output"));

    splitter->addWidget(tree);
    splitter->addWidget(detailEdit);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter);

    // Show tool input/output when an entry is selected.
    // Capture log by value — it's a const ref to caller's data that may go out of scope.
    const auto logCopy = log;
    QObject::connect(tree, &QTreeWidget::currentItemChanged, &dialog, [logCopy, detailEdit](QTreeWidgetItem *current, QTreeWidgetItem *) {
        if (!current) {
            detailEdit->clear();
            return;
        }
        int idx = current->data(0, Qt::UserRole + 1).toInt();
        if (idx < 0 || idx >= logCopy.size()) {
            detailEdit->clear();
            return;
        }
        const auto &entry = logCopy[idx];
        QString detail;

        // For double yolo (suggestion acceptance) and triple yolo (auto-continue),
        // show the prompt that was sent
        if (entry.yoloLevel == 2) {
            detail += QStringLiteral("--- Action ---\nAccepted inline suggestion (Tab + Enter)\n");
        } else if (entry.yoloLevel == 3) {
            detail += QStringLiteral("--- Auto-Continue Prompt ---\n");
            if (!entry.toolInput.isEmpty()) {
                detail += entry.toolInput;
            } else {
                detail += QStringLiteral("(prompt not recorded)");
            }
            detail += QStringLiteral("\n");
        }

        if (!entry.toolInput.isEmpty() && entry.yoloLevel != 3) {
            if (!detail.isEmpty()) {
                detail += QStringLiteral("\n");
            }
            detail += QStringLiteral("--- Input ---\n") + entry.toolInput;
        }
        if (!entry.toolOutput.isEmpty()) {
            if (!detail.isEmpty()) {
                detail += QStringLiteral("\n");
            }
            detail += QStringLiteral("--- Output ---\n") + entry.toolOutput;
        }
        if (detail.isEmpty()) {
            detail = QStringLiteral("(no tool input/output recorded)");
        }
        detailEdit->setPlainText(detail);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.resize(800, 500);
    dialog.exec();
}

void SessionManagerPanel::showSubagentTranscript(const SubagentInfo &info)
{
    if (info.transcriptPath.isEmpty() || !QFile::exists(info.transcriptPath)) {
        QMessageBox::information(this, i18n("No Transcript"), i18n("Transcript file is not available yet.\nIt will be available after the agent completes."));
        return;
    }

    QFile file(info.transcriptPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, i18n("Read Error"), i18n("Could not open transcript file:\n%1", info.transcriptPath));
        return;
    }

    // Parse JSONL and extract readable content
    // Cap at 5000 lines to prevent UI freeze on large transcripts
    static constexpr int MAX_LINES = 5000;
    QString readable;
    int lineCount = 0;

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        if (++lineCount > MAX_LINES) {
            readable += i18n("\n\n(Truncated at %1 lines — open externally for full transcript)\n", MAX_LINES);
            break;
        }
        QString line = stream.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QJsonDocument lineDoc = QJsonDocument::fromJson(line.toUtf8());
        if (!lineDoc.isObject()) {
            continue;
        }
        QJsonObject obj = lineDoc.object();
        QString type = obj[QStringLiteral("type")].toString();

        if (type == QStringLiteral("assistant")) {
            // Extract assistant message content
            QJsonArray content = obj[QStringLiteral("message")].toObject()[QStringLiteral("content")].toArray();
            for (const auto &block : content) {
                QJsonObject b = block.toObject();
                if (b[QStringLiteral("type")].toString() == QStringLiteral("text")) {
                    readable += QStringLiteral("[Assistant]\n%1\n\n").arg(b[QStringLiteral("text")].toString());
                } else if (b[QStringLiteral("type")].toString() == QStringLiteral("tool_use")) {
                    readable += QStringLiteral("[Tool: %1]\n").arg(b[QStringLiteral("name")].toString());
                    QString inputStr = QString::fromUtf8(QJsonDocument(b[QStringLiteral("input")].toObject()).toJson(QJsonDocument::Compact));
                    if (inputStr.length() > 200) {
                        inputStr = inputStr.left(197) + QStringLiteral("...");
                    }
                    readable += inputStr + QStringLiteral("\n\n");
                }
            }
        } else if (type == QStringLiteral("tool_result") || type == QStringLiteral("result")) {
            QJsonArray content = obj[QStringLiteral("content")].toArray();
            for (const auto &block : content) {
                QJsonObject b = block.toObject();
                if (b[QStringLiteral("type")].toString() == QStringLiteral("text")) {
                    QString text = b[QStringLiteral("text")].toString();
                    if (text.length() > 500) {
                        text = text.left(497) + QStringLiteral("...");
                    }
                    readable += QStringLiteral("[Result]\n%1\n\n").arg(text);
                }
            }
        } else if (type == QStringLiteral("human") || type == QStringLiteral("user")) {
            QJsonArray content = obj[QStringLiteral("message")].toObject()[QStringLiteral("content")].toArray();
            for (const auto &block : content) {
                QJsonObject b = block.toObject();
                if (b[QStringLiteral("type")].toString() == QStringLiteral("text")) {
                    readable += QStringLiteral("[User]\n%1\n\n").arg(b[QStringLiteral("text")].toString());
                }
            }
        }
    }
    file.close();

    if (readable.isEmpty()) {
        // Fallback: show first 5000 lines of raw JSONL (capped to avoid UI freeze)
        QFile raw(info.transcriptPath);
        if (raw.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream rawStream(&raw);
            int rawLines = 0;
            while (!rawStream.atEnd() && rawLines < MAX_LINES) {
                readable += rawStream.readLine() + QStringLiteral("\n");
                rawLines++;
            }
            if (!rawStream.atEnd()) {
                readable += i18n("\n(Truncated at %1 lines)\n", MAX_LINES);
            }
            raw.close();
        }
    }

    // Build viewer dialog
    QString title = i18n("Transcript \u2014 %1", info.agentType);
    if (!info.teammateName.isEmpty()) {
        title = i18n("Transcript \u2014 %1 (%2)", info.agentType, info.teammateName);
    }

    QDialog dialog(this);
    dialog.setWindowTitle(title);
    auto *layout = new QVBoxLayout(&dialog);

    auto *toolbar = new QToolBar(&dialog);
    QAction *openExternalAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Open in External Editor"));
    connect(openExternalAction, &QAction::triggered, &dialog, [info]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.transcriptPath));
    });
    layout->addWidget(toolbar);

    auto *textEdit = new QPlainTextEdit(&dialog);
    textEdit->setReadOnly(true);
    QFont monoFont(QStringLiteral("monospace"));
    monoFont.setStyleHint(QFont::TypeWriter);
    textEdit->setFont(monoFont);
    textEdit->setPlainText(readable);
    layout->addWidget(textEdit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.resize(700, 500);
    dialog.exec();
}

void SessionManagerPanel::showSessionActivity(const QString &jsonlPath, const QString &workDir)
{
    QFile file(jsonlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, i18n("Read Error"), i18n("Could not open conversation file:\n%1", jsonlPath));
        return;
    }

    // Parse JSONL: build structured activity and collect file paths from tool_use
    // Cap at 5000 lines to prevent UI freeze on large conversations (10K+ lines → 500ms+)
    static constexpr int MAX_LINES = 5000;
    QString readable;
    QSet<QString> filesModified; // unique file paths from Write/Edit tool calls
    int toolCallCount = 0;
    int userMessageCount = 0;
    int lineCount = 0;
    bool truncated = false;

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        if (++lineCount > MAX_LINES) {
            truncated = true;
            break;
        }
        QString line = stream.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QJsonDocument lineDoc = QJsonDocument::fromJson(line.toUtf8());
        if (!lineDoc.isObject()) {
            continue;
        }
        QJsonObject obj = lineDoc.object();
        QString type = obj[QStringLiteral("type")].toString();

        if (type == QStringLiteral("assistant")) {
            QJsonArray content = obj[QStringLiteral("message")].toObject()[QStringLiteral("content")].toArray();
            for (const auto &block : content) {
                QJsonObject b = block.toObject();
                QString blockType = b[QStringLiteral("type")].toString();
                if (blockType == QStringLiteral("text")) {
                    readable += QStringLiteral("[Assistant]\n%1\n\n").arg(b[QStringLiteral("text")].toString());
                } else if (blockType == QStringLiteral("tool_use")) {
                    toolCallCount++;
                    QString toolName = b[QStringLiteral("name")].toString();
                    QJsonObject input = b[QStringLiteral("input")].toObject();

                    // Extract file path from Write/Edit/Read tool calls
                    QString filePath = input[QStringLiteral("file_path")].toString();
                    if (!filePath.isEmpty() && (toolName == QStringLiteral("Write") || toolName == QStringLiteral("Edit"))) {
                        filesModified.insert(filePath);
                    }

                    readable += QStringLiteral("[Tool: %1]").arg(toolName);
                    if (!filePath.isEmpty()) {
                        readable += QStringLiteral("  %1").arg(filePath);
                    }
                    readable += QStringLiteral("\n");

                    // Show compact input for non-file tools
                    if (filePath.isEmpty()) {
                        QString inputStr = QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact));
                        if (inputStr.length() > 200) {
                            inputStr = inputStr.left(197) + QStringLiteral("...");
                        }
                        readable += inputStr;
                    }
                    readable += QStringLiteral("\n\n");
                }
            }
        } else if (type == QStringLiteral("human") || type == QStringLiteral("user")) {
            userMessageCount++;
            QJsonValue content = obj[QStringLiteral("message")].toObject()[QStringLiteral("content")];
            if (content.isString()) {
                readable += QStringLiteral("[User]\n%1\n\n").arg(content.toString());
            } else if (content.isArray()) {
                for (const auto &block : content.toArray()) {
                    QJsonObject b = block.toObject();
                    if (b[QStringLiteral("type")].toString() == QStringLiteral("text")) {
                        readable += QStringLiteral("[User]\n%1\n\n").arg(b[QStringLiteral("text")].toString());
                    }
                }
            }
        }
        // Skip tool_result for activity view — focus on actions, not outputs
    }
    file.close();

    // Build summary header
    QString summary;
    summary += i18n("Project: %1\n", QDir(workDir).dirName());
    summary += i18n("Messages: %1 user, Tool calls: %2\n", userMessageCount, toolCallCount);
    if (truncated) {
        summary += i18n("(Showing first %1 lines — open externally for full transcript)\n", MAX_LINES);
    }
    if (!filesModified.isEmpty()) {
        QStringList sortedFiles = filesModified.values();
        sortedFiles.sort();
        summary += i18n("Files modified (%1):\n", sortedFiles.size());
        for (const auto &f : std::as_const(sortedFiles)) {
            summary += QStringLiteral("  %1\n").arg(f);
        }
    }
    summary += QStringLiteral("\n") + QString(60, QChar(0x2500)) + QStringLiteral("\n\n");

    QString fullText = summary + readable;

    // Build viewer dialog
    QDialog dialog(this);
    dialog.setWindowTitle(i18n("Session Activity \u2014 %1", QDir(workDir).dirName()));
    auto *layout = new QVBoxLayout(&dialog);

    auto *toolbar = new QToolBar(&dialog);
    QAction *openExternalAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Open in External Editor"));
    connect(openExternalAction, &QAction::triggered, &dialog, [jsonlPath]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(jsonlPath));
    });
    layout->addWidget(toolbar);

    auto *textEdit = new QPlainTextEdit(&dialog);
    textEdit->setReadOnly(true);
    QFont monoFont(QStringLiteral("monospace"));
    monoFont.setStyleHint(QFont::TypeWriter);
    textEdit->setFont(monoFont);
    textEdit->setPlainText(fullText);
    layout->addWidget(textEdit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.resize(800, 600);
    dialog.exec();
}

void SessionManagerPanel::showSubagentDetails(const SubagentInfo &info)
{
    QString title = i18n("Agent Details \u2014 %1", info.agentType);
    if (!info.teammateName.isEmpty()) {
        title = i18n("Agent Details \u2014 %1 (%2)", info.agentType, info.teammateName);
    }

    QString stateStr;
    switch (info.state) {
    case ClaudeProcess::State::Working:
        stateStr = i18n("Working");
        break;
    case ClaudeProcess::State::Idle:
        stateStr = i18n("Idle");
        break;
    case ClaudeProcess::State::NotRunning:
        stateStr = i18n("Not Running");
        break;
    default:
        stateStr = i18n("Unknown");
        break;
    }

    QString details;
    details += QStringLiteral("<b>Agent Type:</b> %1<br>").arg(info.agentType.toHtmlEscaped());
    details += QStringLiteral("<b>Agent ID:</b> %1<br>").arg(info.agentId.toHtmlEscaped());
    if (!info.teammateName.isEmpty()) {
        details += QStringLiteral("<b>Teammate Name:</b> %1<br>").arg(info.teammateName.toHtmlEscaped());
    }
    details += QStringLiteral("<b>State:</b> %1<br>").arg(stateStr);
    if (info.startedAt.isValid()) {
        details += QStringLiteral("<b>Started:</b> %1<br>").arg(info.startedAt.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));
        details += QStringLiteral("<b>Elapsed:</b> %1<br>").arg(formatElapsed(info.startedAt));
    }
    if (!info.currentTaskSubject.isEmpty()) {
        details += QStringLiteral("<b>Task:</b> %1<br>").arg(info.currentTaskSubject.toHtmlEscaped());
    }
    if (!info.transcriptPath.isEmpty()) {
        details += QStringLiteral("<b>Transcript:</b> %1<br>").arg(info.transcriptPath.toHtmlEscaped());
    }

    QMessageBox::information(this, title, details);
}

void SessionManagerPanel::showSubprocessOutput(const SubprocessInfo &info)
{
    QString title = i18n("Subprocess Output");

    auto *dialog = new QDialog(this);
    dialog->setWindowTitle(title);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(700, 500);

    auto *layout = new QVBoxLayout(dialog);

    // Header with command and status info
    QString statusStr;
    switch (info.status) {
    case SubprocessInfo::Running:
        statusStr = i18n("Running");
        break;
    case SubprocessInfo::Completed:
        statusStr = i18n("Completed (exit %1)", info.exitCode);
        break;
    case SubprocessInfo::Failed:
        statusStr = i18n("Failed (exit %1)", info.exitCode);
        break;
    }

    QString header;
    header += QStringLiteral("<b>Command:</b> %1<br>").arg(info.fullCommand.toHtmlEscaped());
    header += QStringLiteral("<b>Status:</b> %1<br>").arg(statusStr);
    if (info.pid > 0) {
        header += QStringLiteral("<b>PID:</b> %1<br>").arg(info.pid);
    }
    if (info.startedAt.isValid()) {
        header += QStringLiteral("<b>Started:</b> %1<br>").arg(info.startedAt.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));
        header += QStringLiteral("<b>Elapsed:</b> %1<br>").arg(formatElapsed(info.startedAt));
    }
    if (info.resourceUsage.rssBytes > 0) {
        header += QStringLiteral("<b>Resources:</b> %1<br>").arg(info.resourceUsage.formatCompact());
    }

    auto *headerLabel = new QLabel(header);
    headerLabel->setTextFormat(Qt::RichText);
    headerLabel->setWordWrap(true);
    layout->addWidget(headerLabel);

    // Output text
    auto *outputEdit = new QPlainTextEdit();
    outputEdit->setReadOnly(true);
    outputEdit->setFont(QFont(QStringLiteral("monospace"), 9));
    outputEdit->setPlainText(info.output.isEmpty() ? i18n("(no output captured)") : info.output);
    layout->addWidget(outputEdit);

    // Copy button
    auto *copyButton = new QPushButton(i18n("Copy Output"));
    connect(copyButton, &QPushButton::clicked, dialog, [outputEdit]() {
        QApplication::clipboard()->setText(outputEdit->toPlainText());
    });
    layout->addWidget(copyButton);

    dialog->show();
}

void SessionManagerPanel::editSessionDescription(const QString &sessionId)
{
    // Get current description (from active session or empty)
    QString currentDesc;
    if (m_activeSessions.contains(sessionId)) {
        ClaudeSession *session = m_activeSessions[sessionId];
        if (session) {
            currentDesc = session->taskDescription();
        }
    }

    // Show input dialog
    bool ok = false;
    QString newDesc =
        QInputDialog::getText(this, i18n("Set Session Description"), i18n("Description (shown in tabs and panel):"), QLineEdit::Normal, currentDesc, &ok);

    if (!ok) {
        return; // User cancelled
    }

    // Update active session if exists
    if (m_activeSessions.contains(sessionId)) {
        ClaudeSession *session = m_activeSessions[sessionId];
        if (session) {
            session->setTaskDescription(newDesc);
        }
    }

    // Update metadata for inactive sessions so the description persists
    if (m_metadata.contains(sessionId)) {
        m_metadata[sessionId].description = newDesc;
        scheduleMetadataSave();
    }

    // Refresh display
    scheduleTreeUpdate();
}

void SessionManagerPanel::editSessionBudget(ClaudeSession *session, const QString &sessionId)
{
    auto *bc = session->budgetController();
    if (!bc) {
        return;
    }

    auto &budget = bc->budget();
    auto &gate = bc->resourceGate();

    auto *dlg = new QDialog(this);
    dlg->setWindowTitle(i18n("Edit Budget — %1", session->sessionName()));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    auto *mainLayout = new QVBoxLayout(dlg);

    // --- Budget limits ---
    auto *budgetGroup = new QGroupBox(i18n("Budget Limits"), dlg);
    auto *grid = new QGridLayout(budgetGroup);

    grid->addWidget(new QLabel(i18n("Time limit (min):"), dlg), 0, 0);
    auto *timeSpin = new QSpinBox(dlg);
    timeSpin->setRange(0, 1440);
    timeSpin->setSpecialValueText(i18n("Unlimited"));
    timeSpin->setSuffix(i18n(" min"));
    timeSpin->setValue(budget.timeLimitMinutes);
    grid->addWidget(timeSpin, 0, 1);

    grid->addWidget(new QLabel(i18n("Cost ceiling ($):"), dlg), 1, 0);
    auto *costSpin = new QDoubleSpinBox(dlg);
    costSpin->setRange(0.0, 1000.0);
    costSpin->setDecimals(2);
    costSpin->setSingleStep(0.50);
    costSpin->setSpecialValueText(i18n("Unlimited"));
    costSpin->setPrefix(QStringLiteral("$"));
    costSpin->setValue(budget.costCeilingUSD);
    grid->addWidget(costSpin, 1, 1);

    grid->addWidget(new QLabel(i18n("Token ceiling (K):"), dlg), 2, 0);
    auto *tokenSpin = new QSpinBox(dlg);
    tokenSpin->setRange(0, 100000);
    tokenSpin->setSingleStep(100);
    tokenSpin->setSpecialValueText(i18n("Unlimited"));
    tokenSpin->setSuffix(QStringLiteral("K"));
    tokenSpin->setValue(static_cast<int>(budget.tokenCeiling / 1000));
    grid->addWidget(tokenSpin, 2, 1);

    mainLayout->addWidget(budgetGroup);

    // --- Resource gate ---
    auto *gateGroup = new QGroupBox(i18n("Resource Gate"), dlg);
    auto *gateGrid = new QGridLayout(gateGroup);

    gateGrid->addWidget(new QLabel(i18n("CPU threshold (%):"), dlg), 0, 0);
    auto *cpuSpin = new QDoubleSpinBox(dlg);
    cpuSpin->setRange(50.0, 100.0);
    cpuSpin->setDecimals(0);
    cpuSpin->setSingleStep(5.0);
    cpuSpin->setSuffix(QStringLiteral("%"));
    cpuSpin->setValue(gate.cpuThresholdPercent);
    gateGrid->addWidget(cpuSpin, 0, 1);

    gateGrid->addWidget(new QLabel(i18n("RAM threshold (GB):"), dlg), 1, 0);
    auto *ramSpin = new QDoubleSpinBox(dlg);
    ramSpin->setRange(0.0, 128.0);
    ramSpin->setDecimals(1);
    ramSpin->setSingleStep(1.0);
    ramSpin->setSpecialValueText(i18n("Auto (80%%)"));
    double ramGB = gate.rssThresholdBytes > 0 ? static_cast<double>(gate.rssThresholdBytes) / (1024.0 * 1024.0 * 1024.0) : 0.0;
    ramSpin->setValue(ramGB);
    gateGrid->addWidget(ramSpin, 1, 1);

    gateGrid->addWidget(new QLabel(i18n("Gate action:"), dlg), 2, 0);
    auto *actionCombo = new QComboBox(dlg);
    actionCombo->addItem(i18n("Pause Yolo"));
    actionCombo->addItem(i18n("Reduce Yolo"));
    actionCombo->addItem(i18n("Notify Only"));
    actionCombo->setCurrentIndex(static_cast<int>(gate.action));
    gateGrid->addWidget(actionCombo, 2, 1);

    mainLayout->addWidget(gateGroup);

    // --- Current usage summary ---
    const auto &usage = session->tokenUsage();
    QString usageSummary = QStringLiteral("Current: %1 tokens, $%2").arg(usage.formatCompact()).arg(usage.estimatedCostUSD(), 0, 'f', 2);
    if (budget.startedAt.isValid()) {
        usageSummary += QStringLiteral(", %1 min elapsed").arg(budget.elapsedMinutes());
    }
    auto *usageLabel = new QLabel(usageSummary, dlg);
    usageLabel->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
    mainLayout->addWidget(usageLabel);

    // --- Buttons ---
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);

    QPointer<ClaudeSession> safeSession(session);
    connect(buttons, &QDialogButtonBox::accepted, dlg, [=]() {
        if (!safeSession) {
            dlg->reject();
            return;
        }
        auto *ctrl = safeSession->budgetController();

        // Apply budget limits
        SessionBudget newBudget = ctrl->budget();
        newBudget.timeLimitMinutes = timeSpin->value();
        newBudget.costCeilingUSD = costSpin->value();
        newBudget.tokenCeiling = static_cast<quint64>(tokenSpin->value()) * 1000;

        // Start the clock if a time limit is newly set
        if (newBudget.timeLimitMinutes > 0 && !newBudget.startedAt.isValid()) {
            newBudget.startedAt = QDateTime::currentDateTime();
        }

        // Clear exceeded flags when limits are raised/removed
        if (newBudget.costCeilingUSD == 0.0 || newBudget.costCeilingUSD > ctrl->budget().costCeilingUSD) {
            newBudget.costExceeded = false;
        }
        if (newBudget.tokenCeiling == 0 || newBudget.tokenCeiling > ctrl->budget().tokenCeiling) {
            newBudget.tokenExceeded = false;
        }
        if (newBudget.timeLimitMinutes == 0 || newBudget.timeLimitMinutes > ctrl->budget().timeLimitMinutes) {
            newBudget.timeExceeded = false;
        }

        ctrl->setBudget(newBudget);

        // Apply resource gate
        auto &g = ctrl->resourceGate();
        g.cpuThresholdPercent = cpuSpin->value();
        g.rssThresholdBytes = ramSpin->value() > 0.0 ? static_cast<quint64>(ramSpin->value() * 1024.0 * 1024.0 * 1024.0) : 0;
        g.action = static_cast<ResourceGate::Action>(actionCombo->currentIndex());

        // Persist budget limits in metadata
        if (m_metadata.contains(sessionId)) {
            m_metadata[sessionId].budgetTimeLimitMinutes = newBudget.timeLimitMinutes;
            m_metadata[sessionId].budgetCostCeilingUSD = newBudget.costCeilingUSD;
            m_metadata[sessionId].budgetTokenCeiling = newBudget.tokenCeiling;
            scheduleMetadataSave();
        }

        scheduleTreeUpdate();
        dlg->accept();
    });

    mainLayout->addWidget(buttons);
    dlg->resize(380, 360);
    dlg->show();
}

void SessionManagerPanel::updateSessionDescription(const QString &sessionId, const QString &desc)
{
    auto *meta = findMetadata(sessionId);
    if (!meta) {
        qWarning() << "SessionManagerPanel::updateSessionDescription: unknown sessionId" << sessionId;
        return;
    }
    meta->description = desc;
    scheduleMetadataSave();
    scheduleTreeUpdate();
}

void SessionManagerPanel::setSessionAgentId(const QString &sessionId, const QString &agentId)
{
    auto *meta = findMetadata(sessionId);
    if (!meta) {
        qWarning() << "SessionManagerPanel::setSessionAgentId: unknown sessionId" << sessionId;
        return;
    }
    meta->agentId = agentId;
    scheduleMetadataSave();
    scheduleTreeUpdate();
}

// --- Tree expansion state preservation ---

QString SessionManagerPanel::compositeKeyForItem(QTreeWidgetItem *item) const
{
    if (!item) {
        return {};
    }
    return item->data(0, Qt::UserRole + 6).toString();
}

void SessionManagerPanel::saveTreeState()
{
    m_expansionState.clear();
    m_savedSelectedKey.clear();
    m_savedScrollPosition = 0;

    if (!m_treeWidget) {
        return;
    }

    // Save scroll position
    if (m_treeWidget->verticalScrollBar()) {
        m_savedScrollPosition = m_treeWidget->verticalScrollBar()->value();
    }

    // Save selected item
    if (QTreeWidgetItem *sel = m_treeWidget->currentItem()) {
        m_savedSelectedKey = compositeKeyForItem(sel);
    }

    // Walk all categories → session items → children recursively
    auto walkItem = [this](QTreeWidgetItem *item, auto &&self) -> void {
        QString key = compositeKeyForItem(item);
        if (!key.isEmpty()) {
            m_expansionState[key] = item->isExpanded();
        }
        for (int i = 0; i < item->childCount(); ++i) {
            self(item->child(i), self);
        }
    };

    for (int i = 0; i < m_treeWidget->topLevelItemCount(); ++i) {
        QTreeWidgetItem *topItem = m_treeWidget->topLevelItem(i);
        // Save expansion for the top-level item itself (folder items need this)
        QString topKey = compositeKeyForItem(topItem);
        if (!topKey.isEmpty()) {
            m_expansionState[topKey] = topItem->isExpanded();
        }
        for (int j = 0; j < topItem->childCount(); ++j) {
            walkItem(topItem->child(j), walkItem);
        }
    }
}

void SessionManagerPanel::restoreTreeState()
{
    if (!m_treeWidget) {
        return;
    }

    // Restore selection
    if (!m_savedSelectedKey.isEmpty()) {
        bool found = false;
        auto walkRestore = [this, &found](QTreeWidgetItem *item, auto &&self) -> void {
            if (found) {
                return;
            }
            if (compositeKeyForItem(item) == m_savedSelectedKey) {
                m_treeWidget->setCurrentItem(item);
                found = true;
                return;
            }
            for (int i = 0; i < item->childCount(); ++i) {
                self(item->child(i), self);
            }
        };
        for (int i = 0; i < m_treeWidget->topLevelItemCount() && !found; ++i) {
            QTreeWidgetItem *topItem = m_treeWidget->topLevelItem(i);
            // Check top-level item itself (for folder items)
            if (compositeKeyForItem(topItem) == m_savedSelectedKey) {
                m_treeWidget->setCurrentItem(topItem);
                found = true;
                break;
            }
            for (int j = 0; j < topItem->childCount() && !found; ++j) {
                walkRestore(topItem->child(j), walkRestore);
            }
        }
    }

    // Restore expansion state for top-level folder items
    for (int i = 0; i < m_treeWidget->topLevelItemCount(); ++i) {
        QTreeWidgetItem *topItem = m_treeWidget->topLevelItem(i);
        QString topKey = compositeKeyForItem(topItem);
        if (!topKey.isEmpty()) {
            auto it = m_expansionState.constFind(topKey);
            if (it != m_expansionState.constEnd()) {
                topItem->setExpanded(it.value());
            }
        }
    }

    // Restore scroll position after layout has settled
    int savedScroll = m_savedScrollPosition;
    QTimer::singleShot(0, this, [this, savedScroll]() {
        if (m_treeWidget && m_treeWidget->verticalScrollBar()) {
            m_treeWidget->verticalScrollBar()->setValue(savedScroll);
        }
    });
}

bool SessionManagerPanel::shouldAutoExpand(const QString &key, const QString &sessionId, bool hasActiveChildren) const
{
    // Muted sessions are always collapsed
    if (m_mutedSessions.contains(sessionId)) {
        return false;
    }

    // Known item → restore saved state
    if (m_knownItems.contains(key)) {
        auto it = m_expansionState.constFind(key);
        if (it != m_expansionState.constEnd()) {
            return it.value();
        }
        // Known but somehow missing from expansion state — default collapsed
        return false;
    }

    // New item → auto-expand if it has active children
    return hasActiveChildren;
}

void SessionManagerPanel::pruneStaleKeys()
{
    // Collect valid session IDs
    QSet<QString> validIds;
    for (auto it = m_metadata.constBegin(); it != m_metadata.constEnd(); ++it) {
        validIds.insert(it.key());
    }

    // Helper: extract session ID from composite key "prefix:sessionId" or "prefix:sessionId:extra"
    auto extractSessionId = [](const QString &key) -> QString {
        int first = key.indexOf(QLatin1Char(':'));
        if (first < 0) {
            return {};
        }
        int second = key.indexOf(QLatin1Char(':'), first + 1);
        if (second > 0) {
            return key.mid(first + 1, second - first - 1);
        }
        return key.mid(first + 1);
    };

    auto pruneSet = [&](QSet<QString> &set) {
        auto it = set.begin();
        while (it != set.end()) {
            QString sid = extractSessionId(*it);
            if (!sid.isEmpty() && !validIds.contains(sid)) {
                it = set.erase(it);
            } else {
                ++it;
            }
        }
    };

    auto pruneHash = [&](QHash<QString, bool> &hash) {
        auto it = hash.begin();
        while (it != hash.end()) {
            QString sid = extractSessionId(it.key());
            if (!sid.isEmpty() && !validIds.contains(sid)) {
                it = hash.erase(it);
            } else {
                ++it;
            }
        }
    };

    pruneSet(m_knownItems);
    pruneHash(m_expansionState);
    m_mutedSessions.intersect(validIds);
}

bool SessionManagerPanel::isTreeInteractionActive() const
{
    if (!m_treeWidget || !m_treeWidget->isVisible()) {
        return false;
    }
    return m_treeWidget->underMouse() || m_treeWidget->hasFocus();
}

bool SessionManagerPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (m_treeWidget && (watched == m_treeWidget || watched == m_treeWidget->viewport())) {
        if (event->type() == QEvent::Leave || event->type() == QEvent::FocusOut) {
            if (m_pendingUpdate && !isTreeInteractionActive()) {
                m_pendingUpdate = false;
                scheduleTreeUpdate();
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace Konsolai

#include "moc_SessionManagerPanel.cpp"
