#include "ui/widgets/session_toolbar.h"

#include <QButtonGroup>
#include <QEasingCurve>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>

namespace ccv2 {

namespace {
QString fmtTime(qint64 frames, double fs) {
    if (fs <= 0.0 || frames < 0) return QStringLiteral("00:00");
    const qint64 s = static_cast<qint64>(frames / fs);
    if (s >= 3600) {
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600)
            .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(s % 60, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QFrame *makeVSep() {
    auto *sep = new QFrame;
    sep->setObjectName(QStringLiteral("toolbarSep"));
    sep->setFrameShape(QFrame::VLine);
    sep->setFixedHeight(18);
    return sep;
}

enum class Glyph { JumpStart, Rewind, Play, Pause, Forward, JumpEnd };

// Crisp monochrome transport icons drawn by hand (the Unicode media glyphs
// render as colour emoji on Windows, which looked off in the toolbar).
QIcon transportIcon(Glyph g, const QColor &color) {
    const int sz = 24;
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);

    auto tri = [&](qreal cx, qreal w, bool right, qreal h = 9.5) {
        QPainterPath path;
        if (right) {
            path.moveTo(cx - w / 2, 12 - h / 2);
            path.lineTo(cx - w / 2, 12 + h / 2);
            path.lineTo(cx + w / 2, 12);
        } else {
            path.moveTo(cx + w / 2, 12 - h / 2);
            path.lineTo(cx + w / 2, 12 + h / 2);
            path.lineTo(cx - w / 2, 12);
        }
        path.closeSubpath();
        // Rounded join softens the sharp triangle tips.
        QPen pen(color);
        pen.setWidthF(1.4);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
    };
    auto bar = [&](qreal cx) { p.drawRoundedRect(QRectF(cx - 1.05, 7.2, 2.1, 9.6), 1.0, 1.0); };

    switch (g) {
    case Glyph::Play:      tri(12.6, 9.5, true, 10.0); break;
    case Glyph::Pause:     p.drawRoundedRect(QRectF(8.5, 7.2, 2.5, 9.6), 1, 1);
                           p.drawRoundedRect(QRectF(13.0, 7.2, 2.5, 9.6), 1, 1); break;
    case Glyph::Rewind:    tri(8.9, 6.6, false); tri(15.1, 6.6, false); break;
    case Glyph::Forward:   tri(8.9, 6.6, true);  tri(15.1, 6.6, true);  break;
    case Glyph::JumpStart: bar(6.8); tri(15.2, 8.0, false); break;
    case Glyph::JumpEnd:   tri(8.8, 8.0, true); bar(17.2); break;
    }
    p.end();
    return QIcon(pm);
}
}  // namespace

SessionToolbar::SessionToolbar(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sessionToolbar"));
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(12, 5, 12, 5);
    row->setSpacing(10);

    // === Mode segment: user intent (实时/回放), pure view switch ===
    auto *seg = new QFrame;
    seg->setObjectName(QStringLiteral("segmented"));
    auto *segRow = new QHBoxLayout(seg);
    segRow->setContentsMargins(2, 2, 2, 2);
    segRow->setSpacing(2);
    m_segLiveBtn = new QPushButton(QStringLiteral("实时"));
    m_segReplayBtn = new QPushButton(QStringLiteral("回放"));
    auto *segGroup = new QButtonGroup(this);
    segGroup->setExclusive(true);
    for (QPushButton *b : {m_segLiveBtn, m_segReplayBtn}) {
        b->setObjectName(QStringLiteral("segItem"));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        segGroup->addButton(b);
        segRow->addWidget(b);
    }
    m_segLiveBtn->setChecked(true);
    row->addWidget(seg);
    row->addSpacing(12);

    m_modeStack = new QStackedWidget;

    // === Page 0: live receive ===
    auto *livePage = new QWidget;
    auto *lp = new QHBoxLayout(livePage);
    lp->setContentsMargins(0, 0, 0, 0);
    lp->setSpacing(10);
    m_runBtn = new QPushButton(QStringLiteral("▶ 开始接收"));
    m_runBtn->setProperty("variant", "primary");
    m_runBtn->setCursor(Qt::PointingHandCursor);
    lp->addWidget(m_runBtn);
    m_connLabel = new QLabel(QStringLiteral("采集待机"));
    m_connLabel->setObjectName(QStringLiteral("connDot"));
    m_connLabel->setProperty("state", "info");
    lp->addWidget(m_connLabel);
    m_rateLabel = new QLabel;
    m_rateLabel->setProperty("role", "data-value");
    m_rateLabel->setVisible(false);
    lp->addWidget(m_rateLabel);
    lp->addStretch(1);
    m_modeStack->addWidget(livePage);

    // === Page 1: file replay ===
    auto *replayPage = new QWidget;
    auto *rp = new QHBoxLayout(replayPage);
    rp->setContentsMargins(0, 0, 0, 0);
    rp->setSpacing(6);
    m_loadBinBtn = new QPushButton(QStringLiteral("打开文件…"));
    m_loadBinBtn->setProperty("variant", "secondary");
    m_loadBinBtn->setCursor(Qt::PointingHandCursor);
    rp->addWidget(m_loadBinBtn);

    // Loaded-file chip: filename + × (unload). Hidden until a file is open —
    // the filename itself is the state indicator, no grey "未加载" noise.
    m_fileChip = new QFrame;
    m_fileChip->setObjectName(QStringLiteral("fileChip"));
    auto *fc = new QHBoxLayout(m_fileChip);
    fc->setContentsMargins(10, 2, 4, 2);
    fc->setSpacing(4);
    m_replayFileLabel = new QLabel;
    m_replayFileLabel->setObjectName(QStringLiteral("fileChipName"));
    fc->addWidget(m_replayFileLabel);
    m_fileCloseBtn = new QPushButton(QStringLiteral("×"));
    m_fileCloseBtn->setObjectName(QStringLiteral("fileChipClose"));
    m_fileCloseBtn->setCursor(Qt::PointingHandCursor);
    m_fileCloseBtn->setToolTip(QStringLiteral("停止回放并卸载文件"));
    m_fileCloseBtn->setFixedSize(18, 18);
    fc->addWidget(m_fileCloseBtn);
    m_fileChip->setVisible(false);
    rp->addWidget(m_fileChip);
    rp->addSpacing(2);

    const QColor iconColor(0xC8, 0xD1, 0xDC);
    auto mkBtn = [&](Glyph g, const QString &tip, const char *objName) {
        auto *b = new QPushButton;
        b->setObjectName(QString::fromLatin1(objName));
        b->setIcon(transportIcon(g, objName == QLatin1String("transportPlay") ? Qt::white : iconColor));
        b->setIconSize(QSize(24, 24));
        b->setToolTip(tip);
        b->setEnabled(false);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(30, 30);
        rp->addWidget(b);
        return b;
    };
    m_jumpStartBtn = mkBtn(Glyph::JumpStart, QStringLiteral("跳到开头"), "transportBtn");
    m_rwBtn        = mkBtn(Glyph::Rewind,    QStringLiteral("后退 2.5 秒"), "transportBtn");
    m_playBtn      = mkBtn(Glyph::Play,      QStringLiteral("播放 / 暂停"), "transportPlay");
    m_ffBtn        = mkBtn(Glyph::Forward,   QStringLiteral("前进 2.5 秒"), "transportBtn");
    m_jumpEndBtn   = mkBtn(Glyph::JumpEnd,   QStringLiteral("跳到结尾"), "transportBtn");

    rp->addSpacing(4);
    m_seekSlider = new QSlider(Qt::Horizontal);
    m_seekSlider->setRange(0, 1000);
    m_seekSlider->setEnabled(false);
    m_seekSlider->setFixedWidth(200);
    rp->addWidget(m_seekSlider);
    m_replayPosLabel = new QLabel(QStringLiteral("00:00 / 00:00"));
    m_replayPosLabel->setProperty("role", "data-value");
    rp->addWidget(m_replayPosLabel);
    rp->addStretch(1);
    m_modeStack->addWidget(replayPage);

    // Mode controls fill the middle; recording pins to the far right in both
    // modes so the row reads as [mode | controls  ………  record].
    row->addWidget(m_modeStack, 1);
    m_recInfoLabel = new QLabel;
    m_recInfoLabel->setProperty("role", "data-value");
    m_recInfoLabel->setVisible(false);
    row->addWidget(m_recInfoLabel);
    row->addWidget(makeVSep());
    m_recordBtn = new QPushButton(QStringLiteral("● 录制"));
    m_recordBtn->setObjectName(QStringLiteral("recordChip"));
    m_recordBtn->setProperty("recording", false);
    m_recordBtn->setCursor(Qt::PointingHandCursor);
    row->addWidget(m_recordBtn);

    // Segment switches the visible control set only; sessions are switched by
    // the actions themselves (开始接收 ends replay, 打开文件 ends live).
    connect(m_segLiveBtn, &QPushButton::toggled, this, [this](bool on) {
        if (on) m_modeStack->setCurrentIndex(0);
    });
    connect(m_segReplayBtn, &QPushButton::toggled, this, [this](bool on) {
        if (on) m_modeStack->setCurrentIndex(1);
    });

    connect(m_runBtn, &QPushButton::clicked, this, &SessionToolbar::runToggleRequested);
    connect(m_recordBtn, &QPushButton::clicked, this, &SessionToolbar::recordToggleRequested);
    connect(m_loadBinBtn, &QPushButton::clicked, this, [this]() {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择 ADC_DATA.bin"), m_lastDir,
            QStringLiteral("BIN (*.bin);;所有文件 (*)"));
        if (!f.isEmpty()) {
            m_lastDir = QFileInfo(f).absolutePath();
            emit replayLoadRequested(f);
        }
    });
    connect(m_fileCloseBtn, &QPushButton::clicked, this, &SessionToolbar::replayStopRequested);
    connect(m_playBtn, &QPushButton::clicked, this, &SessionToolbar::replayTogglePlayRequested);
    connect(m_jumpStartBtn, &QPushButton::clicked, this, &SessionToolbar::replayJumpStartRequested);
    connect(m_jumpEndBtn, &QPushButton::clicked, this, &SessionToolbar::replayJumpEndRequested);
    connect(m_rwBtn, &QPushButton::clicked, this, [this]() { emit replaySkipRequested(-2.5); });
    connect(m_ffBtn, &QPushButton::clicked, this, [this]() { emit replaySkipRequested(2.5); });
    connect(m_seekSlider, &QSlider::sliderPressed, this, [this]() { m_seekDragging = true; });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this]() {
        m_seekDragging = false;
        emit replaySeekRequested(m_seekSlider->value() / 1000.0);
    });

    updateControls();
}

void SessionToolbar::updateControls() {
    // Live receive and file playback are mutually exclusive.
    if (m_loadBinBtn) {
        m_loadBinBtn->setEnabled(!m_recording);
        m_loadBinBtn->setToolTip(m_recording
                                     ? QStringLiteral("录制中不能切换到回放")
                                     : QString());
    }
    for (QPushButton *b : {m_jumpStartBtn, m_rwBtn, m_playBtn, m_ffBtn, m_jumpEndBtn}) {
        if (b) b->setEnabled(m_replayLoaded);
    }
    if (m_seekSlider) m_seekSlider->setEnabled(m_replayLoaded);
    if (m_fileChip) m_fileChip->setVisible(m_replayLoaded);
    if (m_recordBtn) {
        m_recordBtn->setEnabled(!m_replayLoaded);
        if (!m_recording) {
            m_recordBtn->setToolTip(m_replayLoaded
                                        ? QStringLiteral("回放模式不能录制")
                                        : QString());
        }
    }
}

void SessionToolbar::setRunning(bool running) {
    m_running = running;
    if (m_runBtn) {
        m_runBtn->setText(running ? QStringLiteral("■ 停止接收") : QStringLiteral("▶ 开始接收"));
        m_runBtn->setProperty("variant", running ? "danger" : "primary");
        m_runBtn->style()->unpolish(m_runBtn);
        m_runBtn->style()->polish(m_runBtn);
    }
    if (running && m_segLiveBtn && !m_segLiveBtn->isChecked()) {
        m_segLiveBtn->setChecked(true);  // pull the view to the live page
    }
}

void SessionToolbar::setLiveConnection(const QString &text, const char *state) {
    if (!m_connLabel) return;
    m_connLabel->setText(QStringLiteral("● ") + text);
    m_connLabel->setProperty("state", state);
    m_connLabel->style()->unpolish(m_connLabel);
    m_connLabel->style()->polish(m_connLabel);
}

void SessionToolbar::setNetworkRate(qint64 bytesPerSec) {
    if (!m_rateLabel) return;
    if (bytesPerSec < 0) {
        m_rateLabel->setVisible(false);
        return;
    }
    QString text;
    const double mb = bytesPerSec / (1024.0 * 1024.0);
    if (mb >= 1.0) {
        text = QStringLiteral("↓ %1 MB/s").arg(mb, 0, 'f', 1);
    } else {
        text = QStringLiteral("↓ %1 KB/s").arg(bytesPerSec / 1024.0, 0, 'f', 0);
    }
    m_rateLabel->setText(text);
    m_rateLabel->setVisible(true);
}

void SessionToolbar::setRecording(bool recording, const QString &text) {
    m_recording = recording;
    if (!m_recordBtn) return;
    m_recordBtn->setText(recording ? QStringLiteral("● 停止录制") : QStringLiteral("● 录制"));
    m_recordBtn->setProperty("recording", recording);
    m_recordBtn->style()->unpolish(m_recordBtn);
    m_recordBtn->style()->polish(m_recordBtn);
    if (m_recInfoLabel) {
        m_recInfoLabel->setVisible(recording && !text.isEmpty());
        m_recInfoLabel->setText(text);
    }
    updateControls();

    // Pulsing recording indicator: throb the red chip while capturing.
    if (recording) {
        if (!m_recPulseFx) {
            m_recPulseFx = new QGraphicsOpacityEffect(m_recordBtn);
            m_recordBtn->setGraphicsEffect(m_recPulseFx);
            m_recPulse = new QPropertyAnimation(m_recPulseFx, "opacity", this);
            m_recPulse->setDuration(950);
            m_recPulse->setStartValue(1.0);
            m_recPulse->setKeyValueAt(0.5, 0.5);
            m_recPulse->setEndValue(1.0);
            m_recPulse->setLoopCount(-1);
            m_recPulse->setEasingCurve(QEasingCurve::InOutSine);
        }
        m_recPulse->start();
    } else {
        if (m_recPulse) m_recPulse->stop();
        if (m_recPulseFx) m_recPulseFx->setOpacity(1.0);
    }
}

void SessionToolbar::setReplayLoaded(const QString &file) {
    if (m_replayPosLabel) m_replayPosLabel->setToolTip(QString());
    m_replayLoaded = !file.isEmpty();
    if (m_replayFileLabel) {
        m_replayFileLabel->setText(m_replayLoaded ? QFileInfo(file).fileName() : QString());
        m_replayFileLabel->setToolTip(file);
    }
    updateControls();
    if (m_replayLoaded) {
        if (m_segReplayBtn && !m_segReplayBtn->isChecked()) {
            m_segReplayBtn->setChecked(true);  // pull the view to the replay page
        }
    } else {
        setReplayPlaying(false);
        if (m_replayPosLabel) m_replayPosLabel->setText(QStringLiteral("00:00 / 00:00"));
        if (m_seekSlider) {
            const QSignalBlocker blocker(m_seekSlider);
            m_seekSlider->setValue(0);
        }
    }
}

void SessionToolbar::setReplayError(const QString &message) {
    setReplayLoaded(QString());
    m_replayPosLabel->setText(QStringLiteral("回放失败"));
    m_replayPosLabel->setToolTip(message);
}

void SessionToolbar::setReplayPlaying(bool playing) {
    if (m_playBtn) {
        m_playBtn->setIcon(transportIcon(playing ? Glyph::Pause : Glyph::Play, Qt::white));
    }
}

void SessionToolbar::setReplayPosition(qint64 curFrame, qint64 totalFrames, double sampleRate) {
    if (m_seekSlider && !m_seekDragging && totalFrames > 0) {
        const QSignalBlocker blocker(m_seekSlider);
        m_seekSlider->setValue(static_cast<int>(1000.0 * curFrame / totalFrames));
    }
    if (m_replayPosLabel) {
        m_replayPosLabel->setText(fmtTime(curFrame, sampleRate) + QStringLiteral(" / ") +
                                  fmtTime(totalFrames, sampleRate));
    }
}

}  // namespace ccv2
