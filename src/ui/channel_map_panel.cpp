#include "ui/channel_map_panel.h"

#include <algorithm>
#include <functional>
#include <initializer_list>

#include <QAbstractItemView>
#include <QComboBox>
#include <QFrame>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionGraphicsItem>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "core/constants.h"

namespace ccv2 {

namespace {

// Sentinel for "the selected channels disagree on this field". Never written to
// a register: a field showing it is skipped so each channel keeps its own value.
constexpr int kMixedCode = -1;
const char *const kMixedText = "— 混合 —";

// Select the item carrying `code`. kMixedCode gets a temporary leading item so
// the combo can display an indeterminate state; it is removed again as soon as
// the field becomes uniform.
void setComboCode(QComboBox *combo, int code) {
    if (!combo) return;
    // Never mutate the item list while the user has the popup open: inserting
    // or removing the mixed item shifts every index under their pointer and
    // their click would land on the neighbouring option.
    if (combo->view() && combo->view()->isVisible()) return;
    const QSignalBlocker blocker(combo);
    const bool hasMixedItem = combo->count() > 0 && combo->itemData(0).toInt() == kMixedCode;
    if (code == kMixedCode) {
        if (!hasMixedItem) {
            combo->insertItem(0, QString::fromUtf8(kMixedText), kMixedCode);
        }
        combo->setCurrentIndex(0);
        return;
    }
    if (hasMixedItem) {
        combo->removeItem(0);
    }
    const int index = combo->findData(code);
    if (index >= 0) {
        combo->setCurrentIndex(index);
        return;
    }
    // The register holds a code this combo has no item for (e.g. CT pulse
    // widths only enumerate 0-8 of a 4-bit field). Show the indeterminate
    // state instead of silently keeping a stale selection — a write with the
    // stale value would otherwise overwrite the channel's actual code.
    combo->insertItem(0, QString::fromUtf8(kMixedText), kMixedCode);
    combo->setCurrentIndex(0);
}

// Apply a combo's field onto `reg`, unless it is in the mixed state.
template <typename Field, typename WithFn>
quint16 applyIfSet(quint16 reg, const QComboBox *combo, Field field, WithFn with) {
    if (!combo) return reg;
    const int code = combo->currentData().toInt();
    if (code == kMixedCode) return reg;
    return with(reg, field, code);
}

QString blendHex(const QString &colorA, const QString &colorB, double ratio) {
    const double r = qBound(0.0, ratio, 1.0);
    QColor a(colorA);
    QColor b(colorB);
    if (!a.isValid()) {
        a = QColor(QStringLiteral("#4d4d4d"));
    }
    if (!b.isValid()) {
        b = QColor(QStringLiteral("#b0b0b0"));
    }

    const int rr = static_cast<int>(a.red() * (1.0 - r) + b.red() * r);
    const int gg = static_cast<int>(a.green() * (1.0 - r) + b.green() * r);
    const int bb = static_cast<int>(a.blue() * (1.0 - r) + b.blue() * r);
    return QColor(rr, gg, bb).name();
}

struct ElectrodeInfo {
    double x{0.0};
    double y{0.0};
    int pCol{0};
    int pRow{0};
    int block{0};
    int blockInCol{0};
    int idInBlock{0};
    int electrodeGlobal{0};
    int localChannel{0};
    int globalChannel{0};
};

int localElectrodeInChannel(int idInBlock) {
    return idInBlock % 4;
}

bool isRoutedElectrodeGlobal(int electrodeGlobal) {
    if (electrodeGlobal < 0 || electrodeGlobal >= 1024) {
        return false;
    }

    const int block = electrodeGlobal / 16;
    const int idInBlock = electrodeGlobal % 16;
    const bool routedEven = (block % 8) < 4;
    return (idInBlock % 2 == 0) == routedEven;
}

// Electrode inner-core fill, updated by ElectrodeMapView::setThemePalette so
// the map follows theme switches (the old hardcoded light values glared on
// every dark theme). Shared by all items; GUI thread only.
QColor g_electrodeInnerFill(QStringLiteral("#f6fbff"));
QColor g_electrodeInnerFillSelected(QStringLiteral("#fff3cf"));

class ElectrodeRectItem : public QGraphicsRectItem {
public:
    ElectrodeRectItem(const QRectF &rect, int idx, const std::function<void(int)> &clickCallback, QGraphicsItem *parent = nullptr)
        : QGraphicsRectItem(rect, parent), m_idx(idx), m_clickCallback(clickCallback) {}

protected:
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override {
        Q_UNUSED(option);
        Q_UNUSED(widget);

        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(pen());
        painter->setBrush(brush());
        painter->drawRoundedRect(rect(), 2.2, 2.2);

        const QRectF inner = rect().adjusted(2.2, 2.2, -2.2, -2.2);
        if (inner.width() > 0.0 && inner.height() > 0.0) {
            QColor innerColor = isSelected() ? g_electrodeInnerFillSelected : g_electrodeInnerFill;
            innerColor.setAlpha(220);
            painter->setPen(Qt::NoPen);
            painter->setBrush(innerColor);
            painter->drawRoundedRect(inner, 1.8, 1.8);
        }
    }

    void mousePressEvent(QGraphicsSceneMouseEvent *event) override {
        QGraphicsRectItem::mousePressEvent(event);
        if (m_clickCallback) {
            m_clickCallback(m_idx);
        }
    }

private:
    int m_idx{0};
    std::function<void(int)> m_clickCallback;
};

}  // namespace

class ElectrodeMapView : public QGraphicsView {
public:
    explicit ElectrodeMapView(QWidget *parent = nullptr)
        : QGraphicsView(parent) {
        setScene(new QGraphicsScene(this));
        setBackgroundBrush(QBrush(QColor(m_bgColor)));
        setRenderHint(QPainter::Antialiasing, true);
        setDragMode(QGraphicsView::RubberBandDrag);
        setRubberBandSelectionMode(Qt::IntersectsItemShape);
        setTransformationAnchor(QGraphicsView::AnchorViewCenter);
        setResizeAnchor(QGraphicsView::AnchorViewCenter);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

        generateLayoutDataStrict();
        computeColumnPositions();

        connect(scene(), &QGraphicsScene::selectionChanged, this, [this]() { onSceneSelectionChanged(); });
        rebuildScene(false);
        fitToChip();
    }

    ~ElectrodeMapView() override {
        // QWidget deletes the owned scene only after this class's item index
        // and callbacks have been destroyed. Removing selected scene items
        // emits selectionChanged; do not re-enter refreshItemStyles then.
        if (scene()) QObject::disconnect(scene(), nullptr, this, nullptr);
    }

    void setOnPairSelectionChanged(const std::function<void(const QSet<QPair<int, int>> &)> &cb) { m_pairSelectionChanged = cb; }
    void setOnElectrodeClicked(const std::function<void(const QVariantMap &)> &cb) { m_electrodeClicked = cb; }
    void setOnZoomChanged(const std::function<void(int)> &cb) { m_zoomChanged = cb; }
    void setRoutedOnly(bool routedOnly) {
        if (m_routedOnly == routedOnly) {
            return;
        }
        m_routedOnly = routedOnly;
        rebuildScene(true);
        if (m_autoFit) {
            fitToChip();
        } else {
            setZoomPercent(m_zoomPercent);
        }
    }

    // Manual KeepAspectRatio fit: QGraphicsView::fitInView doesn't reliably
    // converge here (scrollbar interplay), leaving the chip overflowing at
    // "100%". Compute the scale directly from the actual viewport size so
    // 100% always shows the whole chip, and higher percentages scale from it.
    void applyZoom(int percent) {
        const QRectF bbox = scene()->itemsBoundingRect();
        if (bbox.isEmpty()) {
            return;
        }
        const QRectF padded = bbox.adjusted(-12, -12, 12, 12);
        // Constrain the scrollable area to the actual content. The scene's own
        // sceneRect only ever grows, so after rebuildScene it stays oversized
        // and leaves a phantom scrollbar even when everything fits.
        setSceneRect(padded);
        resetTransform();
        const double base = qMin(viewport()->width() / padded.width(),
                                 viewport()->height() / padded.height());
        const double factor = base * (static_cast<double>(percent) / 100.0);
        if (factor > 0.0) {
            scale(factor, factor);
        }
        centerOn(padded.center());
        m_zoomPercent = percent;
        m_autoFit = (percent == 100);
        if (m_zoomChanged) {
            m_zoomChanged(m_zoomPercent);
        }
    }

    void fitToChip() { applyZoom(100); }
    void setZoomPercent(int percent) { applyZoom(qBound(20, percent, 500)); }
    void zoomIn() { setZoomPercent(m_zoomPercent + 10); }
    void zoomOut() { setZoomPercent(m_zoomPercent - 10); }

    void setSelectedPairs(const QSet<QPair<int, int>> &pairs) {
        m_syncing = true;
        for (auto it = m_itemsByIdx.cbegin(); it != m_itemsByIdx.cend(); ++it) {
            it.value()->setSelected(false);
        }

        for (auto it = pairs.cbegin(); it != pairs.cend(); ++it) {
            const QVector<ElectrodeRectItem *> items = m_pairToItems.value(*it);
            for (ElectrodeRectItem *item : items) {
                item->setSelected(true);
            }
        }

        m_syncing = false;
        refreshItemStyles();
    }

    void setThemePalette(const QMap<QString, QString> &palette) {
        bool changed = false;
        auto updateColor = [&](const QString &key, QString &dst) {
            const QString value = palette.value(key).trimmed();
            if (!value.isEmpty()) {
                dst = value;
                changed = true;
            }
        };

        updateColor(QStringLiteral("bg"), m_bgColor);
        updateColor(QStringLiteral("electrode"), m_electrodeColor);
        updateColor(QStringLiteral("outline"), m_electrodeOutline);
        updateColor(QStringLiteral("selected"), m_selectedColor);
        updateColor(QStringLiteral("block_deep"), m_blockColorDeep);
        updateColor(QStringLiteral("block_light"), m_blockColorLight);
        updateColor(QStringLiteral("block_label_deep"), m_blockLabelTextDeep);
        updateColor(QStringLiteral("block_label_light"), m_blockLabelTextLight);
        updateColor(QStringLiteral("boundary"), m_chipBoundaryColor);

        QString innerFill;
        QString innerFillSelected;
        updateColor(QStringLiteral("inner"), innerFill);
        updateColor(QStringLiteral("inner_selected"), innerFillSelected);
        if (!innerFill.isEmpty()) {
            g_electrodeInnerFill = QColor(innerFill);
        }
        if (!innerFillSelected.isEmpty()) {
            g_electrodeInnerFillSelected = QColor(innerFillSelected);
        }

        if (!changed) {
            return;
        }

        rebuildScene(true);
        if (m_autoFit) {
            fitToChip();
        } else {
            setZoomPercent(m_zoomPercent);
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event);
        if (m_autoFit) {
            fitToChip();
        }
    }

    void wheelEvent(QWheelEvent *event) override {
        if (event->modifiers() & Qt::ControlModifier) {
            if (event->angleDelta().y() > 0) {
                zoomIn();
            } else {
                zoomOut();
            }
            event->accept();
            return;
        }
        QGraphicsView::wheelEvent(event);
    }

private:
    static constexpr int kPadSize = 18;
    static constexpr int kYSpacing = 20;
    static constexpr int kSmallXSpacing = 50;
    static constexpr int kLargeXSpacing = 118;
    static constexpr int kChipBoundaryPadding = 96;
    static constexpr int kNumPhysicalCols = 16;
    static constexpr int kNumBlocksPerCol = 4;
    static constexpr int kNumElectrodesPerBlock = 16;

    QString m_bgColor{QStringLiteral("#f4fbff")};
    QString m_electrodeColor{QStringLiteral("#d9eaf4")};
    QString m_electrodeOutline{QStringLiteral("#000508")};
    QString m_selectedColor{QStringLiteral("#ff6b35")};
    QString m_blockColorDeep{QStringLiteral("#1C6CA1")};
    QString m_blockColorLight{QStringLiteral("#98c1d9")};
    QString m_blockLabelTextDeep{QStringLiteral("#ffffff")};
    QString m_blockLabelTextLight{QStringLiteral("#072039")};
    QString m_chipBoundaryColor{QStringLiteral("#072039")};

    QVector<ElectrodeInfo> m_electrodesData;
    QMap<int, double> m_colX;
    QMap<int, double> m_colHalfWidth;
    QMap<int, ElectrodeRectItem *> m_itemsByIdx;
    QMap<QPair<int, int>, QVector<ElectrodeRectItem *>> m_pairToItems;

    bool m_syncing{false};
    int m_zoomPercent{100};
    bool m_autoFit{true};
    bool m_routedOnly{true};
    Qt::AspectRatioMode m_fitMode{Qt::KeepAspectRatio};

    std::function<void(const QSet<QPair<int, int>> &)> m_pairSelectionChanged;
    std::function<void(const QVariantMap &)> m_electrodeClicked;
    std::function<void(int)> m_zoomChanged;

    QSet<QPair<int, int>> selectedPairsFromScene() const {
        QSet<QPair<int, int>> selectedPairs;
        const QList<QGraphicsItem *> items = scene()->selectedItems();
        for (QGraphicsItem *item : items) {
            const QVariant idxVar = item->data(0);
            if (!idxVar.isValid()) {
                continue;
            }
            const int idx = idxVar.toInt();
            if (idx < 0 || idx >= m_electrodesData.size()) {
                continue;
            }
            const ElectrodeInfo &info = m_electrodesData[idx];
            if (m_routedOnly && !isRoutedElectrodeGlobal(info.electrodeGlobal)) {
                continue;
            }
            selectedPairs.insert({info.block, info.localChannel});
        }
        return selectedPairs;
    }

    void rebuildScene(bool keepSelection) {
        const QSet<QPair<int, int>> selectedPairs = keepSelection ? selectedPairsFromScene() : QSet<QPair<int, int>>{};

        m_syncing = true;
        scene()->clear();
        m_itemsByIdx.clear();
        m_pairToItems.clear();
        setBackgroundBrush(QBrush(QColor(m_bgColor)));
        drawChipBoundary();
        drawBlockBackgroundsAndLabels();
        drawElectrodes();
        m_syncing = false;

        if (!selectedPairs.isEmpty()) {
            setSelectedPairs(selectedPairs);
        }
    }

    void generateLayoutDataStrict() {
        m_electrodesData.clear();
        const double startX = 60.0;
        const double startY = 60.0;
        double currentX = startX;

        for (int pCol = 0; pCol < kNumPhysicalCols; ++pCol) {
            for (int blockInCol = 0; blockInCol < kNumBlocksPerCol; ++blockInCol) {
                for (int idInBlock = 0; idInBlock < kNumElectrodesPerBlock; ++idInBlock) {
                    const int pRow = blockInCol * kNumElectrodesPerBlock + idInBlock;
                    const double y = startY + pRow * kYSpacing;

                    const int base = pCol * kNumBlocksPerCol;
                    const int blockNum = (pCol % 2 == 0) ? (base + blockInCol) : (base + (kNumBlocksPerCol - 1 - blockInCol));

                    ElectrodeInfo info;
                    info.x = currentX;
                    info.y = y;
                    info.pCol = pCol;
                    info.pRow = pRow;
                    info.block = blockNum;
                    info.blockInCol = blockInCol;
                    info.idInBlock = idInBlock;
                    info.electrodeGlobal = blockNum * kNumElectrodesPerBlock + idInBlock;
                    info.localChannel = idInBlock / 4;
                    info.globalChannel = blockNum * 4 + info.localChannel;
                    m_electrodesData.push_back(info);
                }
            }

            currentX += (pCol % 2 == 0) ? kLargeXSpacing : kSmallXSpacing;
        }
    }

    void computeColumnPositions() {
        m_colX.clear();
        m_colHalfWidth.clear();

        for (const ElectrodeInfo &e : m_electrodesData) {
            if (!m_colX.contains(e.pCol)) {
                m_colX.insert(e.pCol, e.x);
            }
        }

        for (int p = 0; p < kNumPhysicalCols; ++p) {
            const double x = m_colX.value(p);
            QVector<double> candidates;
            if (m_colX.contains(p - 1)) {
                candidates.push_back((x - m_colX.value(p - 1)) / 2.0);
            }
            if (m_colX.contains(p + 1)) {
                candidates.push_back((m_colX.value(p + 1) - x) / 2.0);
            }

            double half = candidates.isEmpty() ? std::max(kPadSize * 4.0, 26.0)
                                               : *std::min_element(candidates.cbegin(), candidates.cend());
            half = std::max(half * 0.95, kPadSize * 2.2);
            m_colHalfWidth.insert(p, half);
        }
    }

    void drawChipBoundary() {
        if (m_electrodesData.isEmpty()) {
            return;
        }

        double minX = m_electrodesData.first().x;
        double maxX = m_electrodesData.first().x;
        double minY = m_electrodesData.first().y;
        double maxY = m_electrodesData.first().y;

        for (const ElectrodeInfo &e : m_electrodesData) {
            minX = std::min(minX, e.x);
            maxX = std::max(maxX, e.x);
            minY = std::min(minY, e.y);
            maxY = std::max(maxY, e.y);
        }

        const QRectF chipRect(minX - kChipBoundaryPadding,
                              minY - kChipBoundaryPadding,
                              (maxX - minX) + 2.0 * kChipBoundaryPadding,
                              (maxY - minY) + 2.0 * kChipBoundaryPadding);

        QColor boardBg(m_blockColorLight);
        boardBg.setAlpha(28);
        QPen boardPen{QColor(m_chipBoundaryColor)};
        boardPen.setWidthF(1.2);
        boardPen.setStyle(Qt::SolidLine);

        QPainterPath boardPath;
        boardPath.addRoundedRect(chipRect, 14.0, 14.0);
        QGraphicsPathItem *boardItem = scene()->addPath(boardPath, boardPen, QBrush(boardBg));
        boardItem->setZValue(-12.0);

        QPen innerPen(QColor(blendHex(m_chipBoundaryColor, m_blockLabelTextDeep, 0.35)));
        innerPen.setWidthF(0.8);
        innerPen.setStyle(Qt::DashLine);
        QGraphicsPathItem *innerItem = scene()->addPath(boardPath, innerPen, QBrush(Qt::NoBrush));
        innerItem->setZValue(-11.0);

        scene()->setSceneRect(chipRect.adjusted(-32.0, -32.0, 32.0, 32.0));
    }

    void drawBlockBackgroundsAndLabels() {
        struct BlockMeta {
            QVector<double> ys;
            int pCol{0};
            int blockInCol{0};
        };

        QMap<int, BlockMeta> blocks;
        for (const ElectrodeInfo &e : m_electrodesData) {
            BlockMeta &meta = blocks[e.block];
            meta.ys.push_back(e.y);
            meta.pCol = e.pCol;
            meta.blockInCol = e.blockInCol;
        }

        for (auto it = blocks.cbegin(); it != blocks.cend(); ++it) {
            const int blockNum = it.key();
            const BlockMeta meta = it.value();

            const double minY = *std::min_element(meta.ys.cbegin(), meta.ys.cend());
            const double maxY = *std::max_element(meta.ys.cbegin(), meta.ys.cend());
            const double cx = m_colX.value(meta.pCol);
            const double halfW = m_colHalfWidth.value(meta.pCol, std::max(kPadSize * 3.0, 22.0));

            const double x0 = cx - halfW;
            const double x1 = cx + halfW;
            const double y0 = minY - kPadSize;
            const double y1 = maxY + kPadSize;

            const bool deep = (meta.blockInCol % 2 == 0);
            QColor bgColor(deep ? m_blockColorDeep : m_blockColorLight);
            QColor textColor(deep ? m_blockLabelTextDeep : m_blockLabelTextLight);
            bgColor.setAlpha(deep ? 190 : 155);

            QPen stroke(QColor(blendHex(bgColor.name(), m_electrodeOutline, 0.45)));
            stroke.setWidthF(0.5);

            QPainterPath blockPath;
            blockPath.addRoundedRect(QRectF(x0, y0, x1 - x0, y1 - y0), 5.0, 5.0);
            QGraphicsPathItem *rectItem = scene()->addPath(blockPath, stroke, QBrush(bgColor));
            rectItem->setZValue(-5.0);

            QGraphicsTextItem *textItem = scene()->addText(QStringLiteral("B%1").arg(blockNum, 2, 10, QLatin1Char('0')));
            textItem->setDefaultTextColor(textColor);
            QFont font = textItem->font();
            font.setPointSize(7);
            font.setBold(true);
            textItem->setFont(font);

            const QRectF tr = textItem->boundingRect();
            textItem->setPos(cx - tr.width() / 2.0, y0 + 4.0);
            textItem->setZValue(-4.0);
        }
    }

    void drawElectrodes() {
        m_itemsByIdx.clear();
        m_pairToItems.clear();

        const double half = static_cast<double>(kPadSize) / 2.0;
        for (int idx = 0; idx < m_electrodesData.size(); ++idx) {
            const ElectrodeInfo &e = m_electrodesData[idx];
            const bool routed = isRoutedElectrodeGlobal(e.electrodeGlobal);
            if (m_routedOnly && !routed) {
                continue;
            }

            const QRectF rect(e.x - half, e.y - half, kPadSize, kPadSize);

            auto *item = new ElectrodeRectItem(rect, idx, [this](int i) { onItemClicked(i); });
            item->setFlag(QGraphicsItem::ItemIsSelectable, true);
            item->setBrush(QBrush(QColor(m_electrodeColor)));
            item->setPen(QPen(QColor(m_electrodeOutline), 1.1));
            item->setData(0, idx);
            item->setToolTip(
                QStringLiteral("Block=%1, Local=%2, Local ELE=%3, Global=%4, Electrode=%5%6")
                    .arg(e.block, 2, 10, QLatin1Char('0'))
                    .arg(QString::number(e.localChannel, 2).rightJustified(2, QLatin1Char('0')))
                    .arg(localElectrodeInChannel(e.idInBlock))
                    .arg(e.globalChannel, 3, 10, QLatin1Char('0'))
                    .arg(e.electrodeGlobal)
                    .arg(routed ? QStringLiteral(" routed") : QStringLiteral(" not routed")));
            scene()->addItem(item);

            m_itemsByIdx.insert(idx, item);
            m_pairToItems[{e.block, e.localChannel}].push_back(item);
        }
    }

    void onItemClicked(int idx) {
        if (idx < 0 || idx >= m_electrodesData.size()) {
            return;
        }
        const ElectrodeInfo &e = m_electrodesData[idx];
        if (m_routedOnly && !isRoutedElectrodeGlobal(e.electrodeGlobal)) {
            return;
        }
        const QString localBits = QString::number(e.localChannel, 2).rightJustified(2, QLatin1Char('0'));

        QVariantMap info;
        info.insert(QStringLiteral("block"), e.block);
        info.insert(QStringLiteral("local_channel"), e.localChannel);
        info.insert(QStringLiteral("local_electrode"), localElectrodeInChannel(e.idInBlock));
        info.insert(QStringLiteral("global_channel"), e.globalChannel);
        info.insert(QStringLiteral("electrode_global"), e.electrodeGlobal);
        info.insert(QStringLiteral("local_bits"), localBits);
        info.insert(QStringLiteral("address_bits"), QString::number(e.block, 2).rightJustified(8, QLatin1Char('0')) + localBits);

        if (m_electrodeClicked) {
            m_electrodeClicked(info);
        }
    }

    void refreshItemStyles() {
        const QBrush baseBrush{QColor(m_electrodeColor)};
        const QBrush selBrush{QColor(blendHex(m_selectedColor, QStringLiteral("#ffffff"), 0.08))};

        QPen basePen(QColor(m_electrodeOutline), 1.1);
        QPen selPen(QColor(m_selectedColor), 2.2);
        basePen.setCosmetic(true);
        selPen.setCosmetic(true);

        for (auto it = m_itemsByIdx.cbegin(); it != m_itemsByIdx.cend(); ++it) {
            ElectrodeRectItem *item = it.value();
            if (item->isSelected()) {
                item->setBrush(selBrush);
                item->setPen(selPen);
            } else {
                item->setBrush(baseBrush);
                item->setPen(basePen);
            }
        }
    }

    void onSceneSelectionChanged() {
        // During setSelectedPairs() every individual setSelected() re-emits
        // the scene's selectionChanged; refreshing all ~1024 items on each of
        // them is O(N^2) ("select all" froze the UI for hundreds of ms).
        // setSelectedPairs() refreshes styles exactly once when it finishes.
        if (m_syncing) {
            return;
        }
        refreshItemStyles();
        if (m_pairSelectionChanged) {
            m_pairSelectionChanged(selectedPairsFromScene());
        }
    }
};

ChannelMapPanel::ChannelMapPanel(ChannelAddressState *channelState,
                                 ConfigManager *cfgMgr,
                                 NetworkState *networkState,
                                 QWidget *parent)
    : QWidget(parent),
      m_channelState(channelState),
      m_cfgMgr(cfgMgr),
      m_networkState(networkState) {
    m_saveDebounceTimer.setSingleShot(true);
    m_saveDebounceTimer.setInterval(500);
    connect(&m_saveDebounceTimer, &QTimer::timeout, this, [this]() { savePanelConfig(); });

    m_shadowSaveTimer.setSingleShot(true);
    m_shadowSaveTimer.setInterval(1000);
    connect(&m_shadowSaveTimer, &QTimer::timeout, this, [this]() { saveShadowConfig(); });

    // Not restarted while active: during a long batch the applied-command
    // feedback arrives continuously, and a restarting timer would postpone
    // the control refresh until the batch ends.
    m_cfgSyncTimer.setSingleShot(true);
    m_cfgSyncTimer.setInterval(100);
    connect(&m_cfgSyncTimer, &QTimer::timeout, this, [this]() { syncCfgUiFromShadow(); });

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto *splitter = new QSplitter(Qt::Horizontal);
    root->addWidget(splitter, 1);

    auto *leftWrap = new QWidget;
    auto *left = new QVBoxLayout(leftWrap);
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(6);
    splitter->addWidget(leftWrap);

    auto *mapGrp = new QWidget;
    mapGrp->setProperty("role", "plot-area");
    auto *mapLayout = new QVBoxLayout(mapGrp);
    mapLayout->setContentsMargins(6, 6, 6, 6);
    mapLayout->setSpacing(6);
    auto *mapHeader = new QHBoxLayout;
    auto *mapTitle = new QLabel(QStringLiteral("1024通道分布"));
    mapTitle->setObjectName(QStringLiteral("title"));
    auto *tip = new QLabel(QStringLiteral("支持单点选择、Ctrl多选、框选；Ctrl+滚轮缩放；选中颜色与当前主题联动。"));
    tip->setProperty("role", "caption");
    mapHeader->addWidget(mapTitle);
    mapHeader->addStretch(1);
    mapHeader->addWidget(tip);
    mapLayout->addLayout(mapHeader);

    // Zoom + quick-select live in one toolbar card above the map, and the
    // last-click readout is a slim status strip below it — selection stays
    // with the map, the right column keeps only config + history.
    auto *controlsWrap = new QWidget;
    controlsWrap->setProperty("role", "page-control-panel");
    auto *controlsBox = new QVBoxLayout(controlsWrap);
    controlsBox->setContentsMargins(0, 0, 0, 0);
    controlsBox->setSpacing(5);

    auto *zoomBar = new QHBoxLayout;
    zoomBar->setSpacing(8);
    auto *zoomLabel = new QLabel(QStringLiteral("缩放"));
    zoomLabel->setProperty("role", "field-label");
    zoomBar->addWidget(zoomLabel);
    auto *btnZoomOut = new QPushButton(QStringLiteral("-"));
    btnZoomOut->setFixedWidth(36);
    auto *btnZoomIn = new QPushButton(QStringLiteral("+"));
    btnZoomIn->setFixedWidth(36);
    auto *btnZoomFit = new QPushButton(QStringLiteral("适应窗口"));
    btnZoomFit->setProperty("variant", "secondary");
    m_routedOnlyCheck = new QCheckBox(QStringLiteral("仅显示512引出ELE"));
    m_routedOnlyCheck->setChecked(true);

    m_zoomSlider = new QSlider(Qt::Horizontal);
    m_zoomSlider->setRange(20, 500);
    m_zoomSlider->setValue(100);
    m_zoomLabel = new QLabel(QStringLiteral("100%"));
    m_zoomLabel->setMinimumWidth(52);

    zoomBar->addWidget(btnZoomOut);
    zoomBar->addWidget(btnZoomIn);
    zoomBar->addWidget(m_zoomSlider, 1);
    zoomBar->addWidget(m_zoomLabel);
    zoomBar->addWidget(btnZoomFit);
    zoomBar->addWidget(m_routedOnlyCheck);
    controlsBox->addLayout(zoomBar);

    auto *selectBar = new QHBoxLayout;
    selectBar->setSpacing(8);
    auto *selLabel = new QLabel(QStringLiteral("选择"));
    selLabel->setProperty("role", "field-label");
    selectBar->addWidget(selLabel);
    selectBar->addWidget(new QLabel(QStringLiteral("Block")));
    m_blockRangeEdit = new QLineEdit(QStringLiteral("0-63"));
    m_blockRangeEdit->setMaximumWidth(110);
    selectBar->addWidget(m_blockRangeEdit);
    selectBar->addWidget(new QLabel(QStringLiteral("本地")));
    for (int i = 0; i < 4; ++i) {
        auto *cb = new QCheckBox(QString::number(i, 2).rightJustified(2, QLatin1Char('0')));
        cb->setChecked(true);
        m_localChecks.push_back(cb);
        selectBar->addWidget(cb);
    }
    auto *btnBatchSelect = new QPushButton(QStringLiteral("批量选中"));
    auto *btnBatchUnselect = new QPushButton(QStringLiteral("批量取消"));
    auto *btnSelectAll = new QPushButton(QStringLiteral("全选"));
    auto *btnClearAll = new QPushButton(QStringLiteral("清空"));
    btnBatchSelect->setProperty("variant", "primary");
    btnBatchUnselect->setProperty("variant", "secondary");
    btnSelectAll->setProperty("variant", "secondary");
    btnClearAll->setProperty("variant", "danger");
    selectBar->addWidget(btnBatchSelect);
    selectBar->addWidget(btnBatchUnselect);
    selectBar->addWidget(btnSelectAll);
    selectBar->addWidget(btnClearAll);
    selectBar->addStretch(1);
    controlsBox->addLayout(selectBar);

    auto *globalBar = new QHBoxLayout;
    globalBar->setSpacing(8);
    auto *globalLbl = new QLabel(QStringLiteral("全局通道"));
    globalLbl->setProperty("role", "field-label");
    globalBar->addWidget(globalLbl);
    m_globalEdit = new QLineEdit;
    m_globalEdit->setPlaceholderText(QStringLiteral("0-31,64,128-140"));
    globalBar->addWidget(m_globalEdit, 1);
    auto *btnApplyGlobal = new QPushButton(QStringLiteral("应用选择"));
    btnApplyGlobal->setProperty("variant", "primary");
    globalBar->addWidget(btnApplyGlobal);
    controlsBox->addLayout(globalBar);

    mapLayout->addWidget(controlsWrap);

    m_mapView = new ElectrodeMapView;
    m_mapView->setMinimumSize(560, 420);
    mapLayout->addWidget(m_mapView, 1);

    // Last-click readout strip (was the 选中电极信息 form in the right column).
    auto *infoWrap = new QWidget;
    infoWrap->setProperty("role", "page-control-panel");
    auto *infoBar = new QHBoxLayout(infoWrap);
    infoBar->setContentsMargins(0, 0, 0, 0);
    infoBar->setSpacing(6);
    auto *infoTitle = new QLabel(QStringLiteral("最近点击"));
    infoTitle->setProperty("role", "field-label");
    infoBar->addWidget(infoTitle);
    m_blockLabel = new QLabel(QStringLiteral("-"));
    m_localLabel = new QLabel(QStringLiteral("-"));
    m_localEleLabel = new QLabel(QStringLiteral("-"));
    m_globalLabel = new QLabel(QStringLiteral("-"));
    m_electrodeLabel = new QLabel(QStringLiteral("-"));
    m_spiBitsLabel = new QLabel(QStringLiteral("-"));
    auto addInfoPair = [&](const QString &caption, QLabel *value) {
        auto *c = new QLabel(caption);
        c->setProperty("role", "caption");
        value->setProperty("role", "data-value");
        infoBar->addSpacing(8);
        infoBar->addWidget(c);
        infoBar->addWidget(value);
    };
    addInfoPair(QStringLiteral("Block"), m_blockLabel);
    addInfoPair(QStringLiteral("本地"), m_localLabel);
    addInfoPair(QStringLiteral("本地ELE"), m_localEleLabel);
    addInfoPair(QStringLiteral("全局"), m_globalLabel);
    addInfoPair(QStringLiteral("电极"), m_electrodeLabel);
    addInfoPair(QStringLiteral("SPI(8b+2b)"), m_spiBitsLabel);
    infoBar->addStretch(1);
    mapLayout->addWidget(infoWrap);

    left->addWidget(mapGrp, 1);

    auto *rightWrap = new QWidget;
    rightWrap->setProperty("role", "page-control-panel");
    auto *rightLayout = new QVBoxLayout(rightWrap);
    rightLayout->setContentsMargins(0, 0, 4, 0);
    rightLayout->setSpacing(6);
    // Scroll the control column so a tall stack of groups never gets squished
    // below its minimum (which overlaps rows).
    auto *rightScroll = new QScrollArea;
    rightScroll->setWidget(rightWrap);
    rightScroll->setWidgetResizable(true);
    rightScroll->setFrameShape(QFrame::NoFrame);
    rightScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rightScroll->setMinimumWidth(440);
    rightScroll->setMaximumWidth(580);
    splitter->addWidget(rightScroll);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);
    splitter->setSizes({1150, 520});
    m_splitter = splitter;

    // Right column: batch config is the primary tool; passive click history
    // sits below it (quick-select and last-click info moved onto the map).
    buildBatchConfigGroup(rightLayout);

    m_historyList = new QListWidget;
    m_historyList->setMaximumHeight(150);
    auto *historyLabel = new QLabel(QStringLiteral("点击历史"));
    historyLabel->setProperty("role", "field-label");
    rightLayout->addWidget(historyLabel);
    rightLayout->addWidget(m_historyList);
    rightLayout->addStretch(1);

    // Timestamp continuity is now an always-on background monitor shown in the
    // top status bar (see TimestampMonitorWorker); no per-page controls needed.

    connect(btnBatchSelect, &QPushButton::clicked, this, &ChannelMapPanel::onBatchModifySelect);
    connect(btnBatchUnselect, &QPushButton::clicked, this, &ChannelMapPanel::onBatchModifyUnselect);
    connect(btnSelectAll, &QPushButton::clicked, this, &ChannelMapPanel::onSelectAll);
    connect(btnClearAll, &QPushButton::clicked, m_channelState, &ChannelAddressState::clear);
    connect(btnApplyGlobal, &QPushButton::clicked, this, &ChannelMapPanel::onApplyGlobalChannels);
    connect(btnZoomOut, &QPushButton::clicked, this, [this]() { m_mapView->zoomOut(); });
    connect(btnZoomIn, &QPushButton::clicked, this, [this]() { m_mapView->zoomIn(); });
    connect(btnZoomFit, &QPushButton::clicked, this, [this]() { m_mapView->fitToChip(); });
    connect(m_zoomSlider, &QSlider::valueChanged, this, [this](int value) { m_mapView->setZoomPercent(value); });
    connect(m_routedOnlyCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_mapView->setRoutedOnly(checked);
        scheduleSavePanelConfig();
    });
    connect(m_blockRangeEdit, &QLineEdit::textChanged, this, [this](const QString &) { scheduleSavePanelConfig(); });
    connect(m_globalEdit, &QLineEdit::textChanged, this, [this](const QString &) { scheduleSavePanelConfig(); });
    for (QCheckBox *check : m_localChecks) {
        connect(check, &QCheckBox::toggled, this, [this](bool) { scheduleSavePanelConfig(); });
    }
    m_mapView->setOnPairSelectionChanged([this](const QSet<QPair<int, int>> &pairs) { onMapPairsChanged(pairs); });
    m_mapView->setOnElectrodeClicked([this](const QVariantMap &info) { onElectrodeClicked(info); });
    m_mapView->setOnZoomChanged([this](int zoomPercent) { onZoomChanged(zoomPercent); });

    connect(m_channelState, &ChannelAddressState::selectionChanged, this, &ChannelMapPanel::syncUiFromState);
    loadPanelConfig();
    syncUiFromState();
}

QSet<int> ChannelMapPanel::parseIntRanges(const QString &text, int minimum, int maximum, bool *ok, QString *error) const {
    QSet<int> result;
    *ok = true;
    error->clear();

    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return result;
    }

    const QStringList parts = trimmed.split(',', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString token = part.trimmed();
        if (token.isEmpty()) {
            continue;
        }

        if (token.contains('-')) {
            const QStringList ab = token.split('-', Qt::KeepEmptyParts);
            if (ab.size() != 2) {
                *ok = false;
                *error = QStringLiteral("区间格式错误: %1").arg(token);
                return {};
            }

            bool okA = false;
            bool okB = false;
            int a = ab[0].trimmed().toInt(&okA);
            int b = ab[1].trimmed().toInt(&okB);
            if (!okA || !okB) {
                *ok = false;
                *error = QStringLiteral("区间数字错误: %1").arg(token);
                return {};
            }
            if (a > b) {
                std::swap(a, b);
            }
            for (int v = a; v <= b; ++v) {
                if (v >= minimum && v <= maximum) {
                    result.insert(v);
                }
            }
        } else {
            bool okV = false;
            const int v = token.toInt(&okV);
            if (!okV) {
                *ok = false;
                *error = QStringLiteral("数字格式错误: %1").arg(token);
                return {};
            }
            if (v >= minimum && v <= maximum) {
                result.insert(v);
            }
        }
    }

    return result;
}

void ChannelMapPanel::onZoomChanged(int zoomPercent) {
    m_zoomLabel->setText(QStringLiteral("%1%").arg(zoomPercent));
    m_zoomSlider->blockSignals(true);
    m_zoomSlider->setValue(zoomPercent);
    m_zoomSlider->blockSignals(false);
    scheduleSavePanelConfig();
}

void ChannelMapPanel::onMapPairsChanged(const QSet<QPair<int, int>> &pairs) {
    m_channelState->setSelectedPairs(pairs);
}

void ChannelMapPanel::onElectrodeClicked(const QVariantMap &info) {
    const int block = info.value(QStringLiteral("block"), -1).toInt();
    const int local = info.value(QStringLiteral("local_channel"), 0).toInt();
    const int localEle = info.value(QStringLiteral("local_electrode"), -1).toInt();
    const int global = info.value(QStringLiteral("global_channel"), -1).toInt();
    const int electrode = info.value(QStringLiteral("electrode_global"), -1).toInt();
    const QString addressBits = info.value(QStringLiteral("address_bits")).toString();

    m_blockLabel->setText(block >= 0 ? QString::number(block) : QStringLiteral("-"));
    m_localLabel->setText(QString::number(local, 2).rightJustified(2, QLatin1Char('0')));
    m_localEleLabel->setText(localEle >= 0 ? QString::number(localEle) : QStringLiteral("-"));
    m_globalLabel->setText(global >= 0 ? QString::number(global) : QStringLiteral("-"));
    m_electrodeLabel->setText(electrode >= 0 ? QString::number(electrode) : QStringLiteral("-"));
    m_spiBitsLabel->setText(addressBits.isEmpty() ? QStringLiteral("-") : addressBits);

    const QString line = QStringLiteral("B%1 | L%2 | E%3 | G%4 | %5")
                             .arg(block, 2, 10, QLatin1Char('0'))
                             .arg(QString::number(local, 2).rightJustified(2, QLatin1Char('0')))
                             .arg(localEle)
                             .arg(global, 3, 10, QLatin1Char('0'))
                             .arg(addressBits);
    m_historyList->insertItem(0, line);
    while (m_historyList->count() > 300) {
        delete m_historyList->takeItem(m_historyList->count() - 1);
    }
}

void ChannelMapPanel::onBatchModifySelect() {
    batchModify(true);
}

void ChannelMapPanel::onBatchModifyUnselect() {
    batchModify(false);
}

void ChannelMapPanel::batchModify(bool makeSelected) {
    bool ok = false;
    QString error;
    const QSet<int> blocks = parseIntRanges(m_blockRangeEdit->text(), 0, 63, &ok, &error);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("输入错误"), QStringLiteral("Block范围解析失败: %1").arg(error));
        return;
    }
    if (blocks.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("输入错误"), QStringLiteral("Block范围为空"));
        return;
    }

    QSet<int> locals;
    for (int i = 0; i < m_localChecks.size(); ++i) {
        if (m_localChecks[i]->isChecked()) {
            locals.insert(i);
        }
    }
    if (locals.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("输入错误"), QStringLiteral("请至少勾选一个本地通道"));
        return;
    }

    QSet<QPair<int, int>> selected;
    const QVector<QPair<int, int>> current = m_channelState->selectedPairs();
    for (const QPair<int, int> &pair : current) {
        selected.insert(pair);
    }

    for (int b : blocks) {
        for (int l : locals) {
            if (makeSelected) {
                selected.insert({b, l});
            } else {
                selected.remove({b, l});
            }
        }
    }

    m_channelState->setSelectedPairs(selected);
}

void ChannelMapPanel::onSelectAll() {
    QSet<QPair<int, int>> selected;
    for (int b = 0; b < 64; ++b) {
        for (int l = 0; l < 4; ++l) {
            selected.insert({b, l});
        }
    }
    m_channelState->setSelectedPairs(selected);
}

void ChannelMapPanel::onApplyGlobalChannels() {
    bool ok = false;
    QString error;
    const QSet<int> channels = parseIntRanges(m_globalEdit->text(), 0, 255, &ok, &error);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("输入错误"), QStringLiteral("全局通道解析失败: %1").arg(error));
        return;
    }

    QSet<QPair<int, int>> selected;
    const QVector<QPair<int, int>> current = m_channelState->selectedPairs();
    for (const QPair<int, int> &pair : current) {
        selected.insert(pair);
    }
    for (int ch : channels) {
        selected.insert({ch / 4, ch % 4});
    }
    m_channelState->setSelectedPairs(selected);
}

void ChannelMapPanel::shutdown()
{
    if (m_saveDebounceTimer.isActive()) {
        m_saveDebounceTimer.stop();
        savePanelConfig();
    }
    if (m_shadowSaveTimer.isActive()) {
        m_shadowSaveTimer.stop();
        saveShadowConfig();
    }
}

void ChannelMapPanel::loadPanelConfig()
{
    if (!m_cfgMgr) {
        return;
    }

    m_loadingConfig = true;
    const ConfigMap cfg = m_cfgMgr->load();

    // Restore the shadow registers first so the batch-config controls and
    // previews start from the last-sent values instead of compile-time
    // defaults (whose read-modify-write would silently rewrite every other
    // field of a register on the hardware).
    const ConfigSection shadow = cfg.value(QStringLiteral("ChannelShadow"));
    if (!shadow.isEmpty()) {
        m_config.restore(shadow.value(QStringLiteral("rec")),
                         shadow.value(QStringLiteral("dac")),
                         shadow.value(QStringLiteral("dac_ct")));
    }

    const ConfigSection sec = cfg.value(QStringLiteral("ChannelMap"));

    m_blockRangeEdit->setText(sec.value(QStringLiteral("block_range"), QStringLiteral("0-63")));
    m_globalEdit->setText(sec.value(QStringLiteral("global_channels")));

    bool ok = false;
    QString error;
    const QSet<int> locals = parseIntRanges(sec.value(QStringLiteral("locals"), QStringLiteral("0,1,2,3")),
                                            0,
                                            3,
                                            &ok,
                                            &error);
    if (ok && !locals.isEmpty()) {
        for (int i = 0; i < m_localChecks.size(); ++i) {
            m_localChecks[i]->setChecked(locals.contains(i));
        }
    }

    const bool routedOnly = sec.value(QStringLiteral("routed_only"), QStringLiteral("1")).toInt() != 0;
    m_routedOnlyCheck->setChecked(routedOnly);
    m_mapView->setRoutedOnly(routedOnly);

    const int zoom = qBound(20, sec.value(QStringLiteral("zoom_percent"), QStringLiteral("100")).toInt(), 500);
    m_mapView->setZoomPercent(zoom);

    const QSet<int> savedChannels =
        parseIntRanges(sec.value(QStringLiteral("selected_globals")), 0, 255, &ok, &error);
    if (ok && !savedChannels.isEmpty()) {
        QSet<QPair<int, int>> pairs;
        for (int channel : savedChannels) {
            pairs.insert({channel / 4, channel % 4});
        }
        m_channelState->setSelectedPairs(pairs);
    }

    m_loadingConfig = false;
}

void ChannelMapPanel::scheduleSavePanelConfig()
{
    if (!m_cfgMgr || m_loadingConfig) {
        return;
    }
    m_saveDebounceTimer.start();
}

void ChannelMapPanel::savePanelConfig() const
{
    if (!m_cfgMgr || m_loadingConfig || !m_blockRangeEdit || !m_globalEdit) {
        return;
    }

    ConfigMap cfg = m_cfgMgr->load();
    ConfigSection &sec = cfg[QStringLiteral("ChannelMap")];
    sec[QStringLiteral("block_range")] = m_blockRangeEdit->text().trimmed();
    sec[QStringLiteral("global_channels")] = m_globalEdit->text().trimmed();
    sec[QStringLiteral("routed_only")] =
        (m_routedOnlyCheck && m_routedOnlyCheck->isChecked()) ? QStringLiteral("1") : QStringLiteral("0");
    sec[QStringLiteral("zoom_percent")] = QString::number(m_zoomSlider ? m_zoomSlider->value() : 100);

    QStringList locals;
    for (int i = 0; i < m_localChecks.size(); ++i) {
        if (m_localChecks[i]->isChecked()) {
            locals << QString::number(i);
        }
    }
    sec[QStringLiteral("locals")] = locals.join(QLatin1Char(','));

    QStringList selectedGlobals;
    const QVector<QPair<int, int>> selectedPairs = m_channelState->selectedPairs();
    selectedGlobals.reserve(selectedPairs.size());
    for (const QPair<int, int> &pair : selectedPairs) {
        selectedGlobals << QString::number(pair.first * 4 + pair.second);
    }
    sec[QStringLiteral("selected_globals")] = selectedGlobals.join(QLatin1Char(','));

    m_cfgMgr->save(cfg);
}

void ChannelMapPanel::syncUiFromState() {
    QSet<QPair<int, int>> selected;
    const QVector<QPair<int, int>> pairs = m_channelState->selectedPairs();
    for (const QPair<int, int> &pair : pairs) {
        selected.insert(pair);
    }
    m_mapView->setSelectedPairs(selected);

    refreshBatchSelectionLabel();
    scheduleSavePanelConfig();
}

void ChannelMapPanel::refreshBatchSelectionLabel() {
    if (!m_cfgSelLabel) return;
    const int n = m_channelState->selectedPairs().size();
    m_cfgSelLabel->setText(n == 0 ? QStringLiteral("未选中通道 — 先在图上选择")
                                  : QStringLiteral("已选 %1 通道，写入即发送 SPI").arg(n));
    m_cfgSelLabel->setProperty("state", n == 0 ? "warn" : "ok");
    m_cfgSelLabel->style()->unpolish(m_cfgSelLabel);
    m_cfgSelLabel->style()->polish(m_cfgSelLabel);
    // Selection changed -> pending edits belong to the old selection; the
    // controls must show the newly selected channels' registers. Coalesced:
    // rubber-band dragging fires this continuously on a 1024-item scene.
    m_cfgDirtyControls.clear();
    scheduleSyncCfgUiFromShadow();
}

int ChannelMapPanel::cfgDelayUs() const {
    return (m_cfgDelaySpin ? m_cfgDelaySpin->value() : 10) * 1000;
}

void ChannelMapPanel::applySpiShadowCommand(const QString &commandBits)
{
    QString bits = commandBits;
    bits.remove(QLatin1Char('_'));
    bits.remove(QLatin1Char(' '));
    if (bits.size() != 32) return;

    bool opcodeOk = false;
    bool blockOk = false;
    bool channelOk = false;
    bool dataOk = false;
    const int opcode = bits.mid(0, 6).toInt(&opcodeOk, 2);
    const int block = bits.mid(6, 8).toInt(&blockOk, 2);
    const int channel = bits.mid(14, 2).toInt(&channelOk, 2);
    const quint16 data = static_cast<quint16>(bits.mid(16, 16).toUInt(&dataOk, 2));
    if (!opcodeOk || !blockOk || !channelOk || !dataOk ||
        block < 0 || block >= ChannelConfigModel::kBlocks ||
        channel < 0 || channel >= ChannelConfigModel::kChPerBlock) {
        return;
    }

    const int index = ChannelConfigModel::channelIndex(block, channel);
    if (opcode == 0x04) {
        m_config.setRec(index, data & 0x0FFF);
    } else if (opcode == 0x06) {
        m_config.setDac(index, data);
    } else if (opcode == 0x1E) {
        m_config.setDacCt(index, data);
    } else {
        return;  // dummy / unrelated opcode
    }

    // A quick command, direct SPI write or batch-config command was actually
    // sent; the shadow above is the only place that records it. Persist it
    // (debounced) and, if the channel is in the current selection, refresh the
    // controls (coalesced — batches apply hundreds of commands in a row).
    scheduleSaveShadowConfig();
    if (m_channelState->selectedPairs().contains({block, channel})) {
        scheduleSyncCfgUiFromShadow();
    }
}

void ChannelMapPanel::scheduleSyncCfgUiFromShadow() {
    if (!m_cfgSyncTimer.isActive()) {
        m_cfgSyncTimer.start();
    }
}

void ChannelMapPanel::scheduleSaveShadowConfig() {
    if (!m_cfgMgr || m_loadingConfig) {
        return;
    }
    // Not restarted while active: a long batch schedules this continuously
    // and a restarting timer would postpone the save for the whole batch —
    // a crash mid-batch would then lose every register already written to
    // hardware. This caps the write rate at once per interval instead.
    if (!m_shadowSaveTimer.isActive()) {
        m_shadowSaveTimer.start();
    }
}

void ChannelMapPanel::saveShadowConfig() const {
    if (!m_cfgMgr) {
        return;
    }
    ConfigMap cfg = m_cfgMgr->load();
    ConfigSection &sec = cfg[QStringLiteral("ChannelShadow")];
    sec[QStringLiteral("rec")] = m_config.serializeRec();
    sec[QStringLiteral("dac")] = m_config.serializeDac();
    sec[QStringLiteral("dac_ct")] = m_config.serializeDacCt();
    m_cfgMgr->save(cfg);
}

quint16 ChannelMapPanel::recValueFor(int idx) const {
    using CM = ChannelConfigModel;
    quint16 v = m_config.rec(idx);  // start from this channel's shadow
    v = applyIfSet(v, m_recGain, CM::RecGain, &CM::withRecField);
    v = applyIfSet(v, m_recHighpass, CM::RecHighpass, &CM::withRecField);
    v = applyIfSet(v, m_recReference, CM::RecReference, &CM::withRecField);
    v = applyIfSet(v, m_recLowLp, CM::RecLowLP, &CM::withRecField);
    v = applyIfSet(v, m_recHighPower, CM::RecHighPower, &CM::withRecField);
    v = applyIfSet(v, m_recTrimImp, CM::RecTrimImp, &CM::withRecField);
    v = applyIfSet(v, m_recOff, CM::RecOff, &CM::withRecField);
    v = applyIfSet(v, m_recRstN, CM::RecRstN, &CM::withRecField);
    return v;
}

quint16 ChannelMapPanel::dacValueFor(int idx) const {
    using CM = ChannelConfigModel;
    quint16 v = m_config.dac(idx);
    // Amplitude spin uses kMixedCode (-1) via specialValueText for "mixed".
    if (m_dacAmpSpin && m_dacAmpSpin->value() != kMixedCode) {
        v = CM::withDacField(v, CM::DacAmplitude, m_dacAmpSpin->value());
    }
    v = applyIfSet(v, m_dacPolarity, CM::DacPolarity, &CM::withDacField);
    v = applyIfSet(v, m_dacStep, CM::DacStep, &CM::withDacField);
    v = applyIfSet(v, m_dacComp, CM::DacCompensate, &CM::withDacField);
    v = applyIfSet(v, m_dacElectrode, CM::DacElectrode, &CM::withDacField);
    v = applyIfSet(v, m_dacOffStim, CM::DacOffStim, &CM::withDacField);
    return v;
}

quint16 ChannelMapPanel::ctValueFor(int idx) const {
    using CM = ChannelConfigModel;
    quint16 v = m_config.dacCt(idx);
    v = applyIfSet(v, m_ctTimePos, CM::CtTimePos, &CM::withCtField);
    v = applyIfSet(v, m_ctTimeNeg, CM::CtTimeNeg, &CM::withCtField);
    v = applyIfSet(v, m_ctGlobalFreq, CM::CtGlobalFreq, &CM::withCtField);
    v = applyIfSet(v, m_ctLocalFreq, CM::CtLocalFreq, &CM::withCtField);
    v = applyIfSet(v, m_ctPolFirst, CM::CtPolFirst, &CM::withCtField);
    v = applyIfSet(v, m_ctAmpX20, CM::CtAmpX20, &CM::withCtField);
    v = applyIfSet(v, m_ctOnOff, CM::CtOnOff, &CM::withCtField);
    return v;
}

void ChannelMapPanel::syncCfgUiFromShadow() {
    const QVector<QPair<int, int>> pairs = m_channelState->selectedPairs();
    if (pairs.isEmpty()) {
        updateCfgPreviews();
        return;
    }

    // For each field: if every selected channel agrees, show that value;
    // otherwise show "— 混合 —" so a write won't invent a uniform value.
    using CM = ChannelConfigModel;
    auto readBackCombo = [&](QComboBox *cb, auto getter, auto field) {
        // A control the operator edited since the last write keeps showing
        // their intent; only clean controls track the shadow truth.
        if (!cb || m_cfgDirtyControls.contains(cb)) return;
        const int first = (m_config.*getter)(
            CM::channelIndex(pairs.first().first, pairs.first().second), field);
        bool uniform = true;
        for (const QPair<int, int> &p : pairs) {
            if ((m_config.*getter)(CM::channelIndex(p.first, p.second), field) != first) {
                uniform = false;
                break;
            }
        }
        setComboCode(cb, uniform ? first : kMixedCode);
    };

    readBackCombo(m_recGain, &CM::recField, CM::RecGain);
    readBackCombo(m_recHighpass, &CM::recField, CM::RecHighpass);
    readBackCombo(m_recReference, &CM::recField, CM::RecReference);
    readBackCombo(m_recLowLp, &CM::recField, CM::RecLowLP);
    readBackCombo(m_recHighPower, &CM::recField, CM::RecHighPower);
    readBackCombo(m_recTrimImp, &CM::recField, CM::RecTrimImp);
    readBackCombo(m_recOff, &CM::recField, CM::RecOff);
    readBackCombo(m_recRstN, &CM::recField, CM::RecRstN);

    readBackCombo(m_dacPolarity, &CM::dacField, CM::DacPolarity);
    readBackCombo(m_dacStep, &CM::dacField, CM::DacStep);
    readBackCombo(m_dacComp, &CM::dacField, CM::DacCompensate);
    readBackCombo(m_dacElectrode, &CM::dacField, CM::DacElectrode);
    readBackCombo(m_dacOffStim, &CM::dacField, CM::DacOffStim);

    readBackCombo(m_ctTimePos, &CM::ctField, CM::CtTimePos);
    readBackCombo(m_ctTimeNeg, &CM::ctField, CM::CtTimeNeg);
    readBackCombo(m_ctGlobalFreq, &CM::ctField, CM::CtGlobalFreq);
    readBackCombo(m_ctLocalFreq, &CM::ctField, CM::CtLocalFreq);
    readBackCombo(m_ctPolFirst, &CM::ctField, CM::CtPolFirst);
    readBackCombo(m_ctAmpX20, &CM::ctField, CM::CtAmpX20);
    readBackCombo(m_ctOnOff, &CM::ctField, CM::CtOnOff);

    if (m_dacAmpSpin && !m_cfgDirtyControls.contains(m_dacAmpSpin)) {
        const int first =
            m_config.dacField(CM::channelIndex(pairs.first().first, pairs.first().second),
                              CM::DacAmplitude);
        bool uniform = true;
        for (const QPair<int, int> &p : pairs) {
            if (m_config.dacField(CM::channelIndex(p.first, p.second), CM::DacAmplitude) != first) {
                uniform = false;
                break;
            }
        }
        const QSignalBlocker blocker(m_dacAmpSpin);
        m_dacAmpSpin->setValue(uniform ? first : kMixedCode);
    }

    updateCfgPreviews();
}

void ChannelMapPanel::setSpiBusy(bool busy) {
    if (m_btnWriteCurrent) m_btnWriteCurrent->setEnabled(!busy);
    if (m_btnWriteAll) m_btnWriteAll->setEnabled(!busy);
}

void ChannelMapPanel::updateCfgTabIndicators() {
    if (!m_cfgTabs) return;
    auto mixed = [](const QComboBox *cb) {
        return cb && cb->currentData().toInt() == kMixedCode;
    };
    const bool recMixed = mixed(m_recGain) || mixed(m_recHighpass) || mixed(m_recReference) ||
                          mixed(m_recLowLp) || mixed(m_recHighPower) || mixed(m_recTrimImp) ||
                          mixed(m_recOff) || mixed(m_recRstN);
    const bool dacMixed = (m_dacAmpSpin && m_dacAmpSpin->value() == kMixedCode) ||
                          mixed(m_dacPolarity) || mixed(m_dacStep) || mixed(m_dacComp) ||
                          mixed(m_dacElectrode) || mixed(m_dacOffStim);
    const bool ctMixed = mixed(m_ctTimePos) || mixed(m_ctTimeNeg) || mixed(m_ctGlobalFreq) ||
                         mixed(m_ctLocalFreq) || mixed(m_ctPolFirst) || mixed(m_ctAmpX20) ||
                         mixed(m_ctOnOff);
    const struct {
        int tab;
        bool isMixed;
        const char16_t *base;
    } tabs[] = {
        {0, recMixed, u"记录 REC"},
        {1, dacMixed, u"刺激 DAC"},
        {2, ctMixed, u"刺激时序 CT"},
    };
    for (const auto &t : tabs) {
        if (t.tab >= m_cfgTabs->count()) continue;
        const QString base = QString::fromUtf16(t.base);
        m_cfgTabs->setTabText(t.tab, t.isMixed ? base + QStringLiteral(" •") : base);
    }
}

// The apply* slots deliberately do NOT touch the shadow registers: sending is
// asynchronous and may fail mid-batch. Each command that is actually sent
// comes back through spiCommandApplied -> applySpiShadowCommand, which is the
// single place the shadow (and its persisted copy) is updated. The controls
// keep showing the operator's intent; the shadow keeps the last-sent truth.
// After a write, that section's controls are no longer "dirty": the sent
// values ARE the intent, so the feedback loop may sync them again.

void ChannelMapPanel::applyRecSection() {
    const QVector<QPair<int, int>> pairs = m_channelState->selectedPairs();
    if (pairs.isEmpty()) {
        refreshBatchSelectionLabel();
        return;
    }
    QStringList cmds;
    cmds.reserve(pairs.size());
    for (const QPair<int, int> &p : pairs) {
        const int idx = ChannelConfigModel::channelIndex(p.first, p.second);
        cmds << ChannelConfigModel::buildRecCommand(p.first, p.second, recValueFor(idx));
    }
    for (const QObject *cb : {static_cast<const QObject *>(m_recGain), static_cast<const QObject *>(m_recHighpass),
                              static_cast<const QObject *>(m_recReference), static_cast<const QObject *>(m_recLowLp),
                              static_cast<const QObject *>(m_recHighPower), static_cast<const QObject *>(m_recTrimImp),
                              static_cast<const QObject *>(m_recOff), static_cast<const QObject *>(m_recRstN)}) {
        m_cfgDirtyControls.remove(cb);
    }
    emit spiBatchRequested(QStringLiteral("批量REC"), cmds, cfgDelayUs());
}

void ChannelMapPanel::applyDacSection() {
    const QVector<QPair<int, int>> pairs = m_channelState->selectedPairs();
    if (pairs.isEmpty()) {
        refreshBatchSelectionLabel();
        return;
    }
    QStringList cmds;
    cmds.reserve(pairs.size());
    for (const QPair<int, int> &p : pairs) {
        const int idx = ChannelConfigModel::channelIndex(p.first, p.second);
        cmds << ChannelConfigModel::buildDacCommand(p.first, p.second, dacValueFor(idx));
    }
    for (const QObject *cb : {static_cast<const QObject *>(m_dacAmpSpin), static_cast<const QObject *>(m_dacPolarity),
                              static_cast<const QObject *>(m_dacStep), static_cast<const QObject *>(m_dacComp),
                              static_cast<const QObject *>(m_dacElectrode),
                              static_cast<const QObject *>(m_dacOffStim)}) {
        m_cfgDirtyControls.remove(cb);
    }
    emit spiBatchRequested(QStringLiteral("批量DAC刺激"), cmds, cfgDelayUs());
}

void ChannelMapPanel::applyCtSection() {
    const QVector<QPair<int, int>> pairs = m_channelState->selectedPairs();
    if (pairs.isEmpty()) {
        refreshBatchSelectionLabel();
        return;
    }
    QStringList cmds;
    cmds.reserve(pairs.size());
    for (const QPair<int, int> &p : pairs) {
        const int idx = ChannelConfigModel::channelIndex(p.first, p.second);
        cmds << ChannelConfigModel::buildCtCommand(p.first, p.second, ctValueFor(idx));
    }
    for (const QObject *cb : {static_cast<const QObject *>(m_ctTimePos), static_cast<const QObject *>(m_ctTimeNeg),
                              static_cast<const QObject *>(m_ctGlobalFreq), static_cast<const QObject *>(m_ctLocalFreq),
                              static_cast<const QObject *>(m_ctPolFirst), static_cast<const QObject *>(m_ctAmpX20),
                              static_cast<const QObject *>(m_ctOnOff)}) {
        m_cfgDirtyControls.remove(cb);
    }
    emit spiBatchRequested(QStringLiteral("批量刺激时序"), cmds, cfgDelayUs());
}

void ChannelMapPanel::applyAllSections() {
    const QVector<QPair<int, int>> pairs = m_channelState->selectedPairs();
    if (pairs.isEmpty()) {
        refreshBatchSelectionLabel();
        return;
    }
    QStringList cmds;
    cmds.reserve(pairs.size() * 3);
    for (const QPair<int, int> &p : pairs) {
        const int idx = ChannelConfigModel::channelIndex(p.first, p.second);
        cmds << ChannelConfigModel::buildRecCommand(p.first, p.second, recValueFor(idx));
    }
    for (const QPair<int, int> &p : pairs) {
        const int idx = ChannelConfigModel::channelIndex(p.first, p.second);
        cmds << ChannelConfigModel::buildDacCommand(p.first, p.second, dacValueFor(idx));
    }
    for (const QPair<int, int> &p : pairs) {
        const int idx = ChannelConfigModel::channelIndex(p.first, p.second);
        cmds << ChannelConfigModel::buildCtCommand(p.first, p.second, ctValueFor(idx));
    }
    m_cfgDirtyControls.clear();
    emit spiBatchRequested(QStringLiteral("批量全部配置"), cmds, cfgDelayUs());
}

void ChannelMapPanel::updateCfgPreviews() {
    if (!m_recPreview || !m_dacPreview || !m_ctPreview) return;
    int block = 0;
    int ch = 0;
    const QVector<QPair<int, int>> pairs = m_channelState->selectedPairs();
    if (!pairs.isEmpty()) {
        block = pairs.first().first;
        ch = pairs.first().second;
    }
    // 32-char command split as opcode(6) block(8) ch(2) data(16).
    auto pretty = [](const QString &cmd) {
        return cmd.left(6) + QLatin1Char(' ') + cmd.mid(6, 8) + QLatin1Char(' ') +
               cmd.mid(14, 2) + QLatin1Char(' ') + cmd.mid(16);
    };
    auto prettyDac = [](const QString &cmd) {
        // opcode block outerCH | OFF_STIM DAC_ELE STEP COMP POL AMPLITUDE
        return cmd.left(6) + QLatin1Char(' ') + cmd.mid(6, 8) + QLatin1Char(' ') +
               cmd.mid(14, 2) + QStringLiteral(" | ") + cmd.mid(16, 1) + QLatin1Char(' ') +
               cmd.mid(17, 2) + QLatin1Char(' ') + cmd.mid(19, 1) + QLatin1Char(' ') +
               cmd.mid(20, 1) + QLatin1Char(' ') + cmd.mid(21, 2) + QLatin1Char(' ') +
               cmd.mid(23, 9);
    };
    // Preview the command the first selected channel would actually get, i.e.
    // the read-modify-write result, not a value assembled from the UI alone.
    const int idx = ChannelConfigModel::channelIndex(block, ch);
    const quint16 rv = recValueFor(idx);
    const quint16 dv = dacValueFor(idx);
    const quint16 cv = ctValueFor(idx);
    m_recPreview->setText(QStringLiteral("0x%1  %2")
                              .arg(rv, 3, 16, QLatin1Char('0'))
                              .arg(pretty(ChannelConfigModel::buildRecCommand(block, ch, rv))));
    m_dacPreview->setText(QStringLiteral("0x%1  %2")
                              .arg(dv, 4, 16, QLatin1Char('0'))
                              .arg(prettyDac(ChannelConfigModel::buildDacCommand(block, ch, dv))));
    m_ctPreview->setText(QStringLiteral("0x%1  %2")
                             .arg(cv, 4, 16, QLatin1Char('0'))
                             .arg(pretty(ChannelConfigModel::buildCtCommand(block, ch, cv))));

    if (m_dacCurrentHint && m_dacAmpSpin && m_dacStep && m_ctAmpX20) {
        const int stepCode = m_dacStep->currentData().toInt();
        const int x20Code = m_ctAmpX20->currentData().toInt();
        if (m_dacAmpSpin->value() == kMixedCode || stepCode == kMixedCode ||
            x20Code == kMixedCode) {
            m_dacCurrentHint->setText(QString::fromUtf8(kMixedText));
        } else {
            const double stepNa = stepCode ? 200.0 : 4.0;
            const double x20 = x20Code ? 20.0 : 1.0;
            const double na = m_dacAmpSpin->value() * stepNa * x20;
            QString txt;
            if (na >= 1e6)
                txt = QStringLiteral("≈ %1 mA").arg(na / 1e6, 0, 'f', 3);
            else if (na >= 1e3)
                txt = QStringLiteral("≈ %1 µA").arg(na / 1e3, 0, 'f', 2);
            else
                txt = QStringLiteral("≈ %1 nA").arg(na, 0, 'f', 0);
            m_dacCurrentHint->setText(txt);
        }
    }

    // Keep the per-tab mixed dot in step with whatever just changed the
    // controls — operator edits land here too, not only shadow syncs.
    updateCfgTabIndicators();
}

void ChannelMapPanel::buildBatchConfigGroup(QVBoxLayout *rightLayout) {
    auto *grp = new QGroupBox(QStringLiteral("批量通道配置"));
    auto *v = new QVBoxLayout(grp);
    v->setContentsMargins(8, 10, 8, 8);
    v->setSpacing(6);

    m_cfgSelLabel = new QLabel(QStringLiteral("未选中通道 — 先在图上选择"));
    m_cfgSelLabel->setProperty("role", "status-line");
    m_cfgSelLabel->setProperty("state", "warn");
    m_cfgSelLabel->setWordWrap(true);
    v->addWidget(m_cfgSelLabel);

    // One tab per physical register instead of three stacked sections: the
    // column stays within one screen and the action bar below never scrolls
    // out of view.
    m_cfgTabs = new QTabWidget;
    m_cfgTabs->setObjectName(QStringLiteral("cfgTabs"));
    m_cfgTabs->setDocumentMode(true);
    v->addWidget(m_cfgTabs);

    const QString binSep = QStringLiteral(" · ");
    auto bin = [](int value, int width) {
        return QString::number(value, 2).rightJustified(width, QLatin1Char('0'));
    };
    // Coded combo: visible text = physical meaning (+ register code for
    // multi-bit fields), item userData = the code written into the register.
    struct Opt {
        int code;
        QString label;
    };
    auto codedCombo = [&](std::initializer_list<Opt> opts, int defaultCode, int bits = 0) {
        auto *combo = new QComboBox;
        for (const Opt &o : opts) {
            QString text = o.label;
            if (bits > 0) text += binSep + bin(o.code, bits);
            combo->addItem(text, o.code);
            if (o.code == defaultCode) combo->setCurrentIndex(combo->count() - 1);
        }
        // Programmatic updates go through setComboCode (signal-blocked), so a
        // fired signal here is an operator edit: mark it dirty so the async
        // applied-command feedback won't overwrite it before it is written.
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo](int) {
            m_cfgDirtyControls.insert(combo);
            updateCfgPreviews();
        });
        return combo;
    };
    QGridLayout *grid = nullptr;
    auto newTab = [&](const QString &title, const QString &opcodeHint) {
        auto *page = new QWidget;
        auto *pv = new QVBoxLayout(page);
        pv->setContentsMargins(8, 10, 8, 8);
        pv->setSpacing(6);
        grid = new QGridLayout;
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(6);
        // Fixed, right-aligned label columns; both control columns stretch
        // equally so the combos line up across rows.
        grid->setColumnMinimumWidth(0, 56);
        grid->setColumnMinimumWidth(3, 56);
        grid->setColumnStretch(1, 1);
        grid->setColumnMinimumWidth(2, 10);
        grid->setColumnStretch(4, 1);
        pv->addLayout(grid);
        pv->addStretch(1);
        const int tab = m_cfgTabs->addTab(page, title);
        m_cfgTabs->setTabToolTip(tab, opcodeHint);
        return pv;
    };
    auto addField = [](QGridLayout *g, int row, int side, const QString &label, QWidget *w) {
        const int c = side == 0 ? 0 : 3;
        auto *l = new QLabel(label);
        l->setProperty("role", "field-label");
        g->addWidget(l, row, c, Qt::AlignRight | Qt::AlignVCenter);
        g->addWidget(w, row, c + 1);
    };
    auto makePreview = [&](QVBoxLayout *pv) {
        auto *l = new QLabel;
        l->setProperty("role", "cmd-preview");
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setToolTip(QStringLiteral("将发送的 32 位命令 (首个选中通道): 操作码6 + Block8 + 通道2 + 数据16"));
        pv->addWidget(l);
        return l;
    };

    {
        auto *pv = newTab(QStringLiteral("记录 REC"),
                          QStringLiteral("写 REC 寄存器 · 操作码 0x04"));
        auto *g = grid;
        m_recGain = codedCombo({{1, QStringLiteral("60×")}, {0, QStringLiteral("180×")}}, 1);
        m_recHighpass = codedCombo({{1, QStringLiteral("1 Hz")}, {0, QStringLiteral("200 Hz")}}, 1);
        m_recReference = codedCombo({{0, QStringLiteral("VSS")}, {1, QStringLiteral("VREF")}}, 0);
        m_recLowLp = codedCombo({{0, QStringLiteral("高带宽")}, {1, QStringLiteral("1 kHz")}}, 0);
        m_recHighPower = codedCombo({{0, QStringLiteral("关")}, {1, QStringLiteral("开")}}, 0);
        m_recTrimImp = codedCombo({{0, QStringLiteral("0")},
                                   {1, QStringLiteral("1")},
                                   {2, QStringLiteral("2")},
                                   {3, QStringLiteral("3")},
                                   {4, QStringLiteral("4")},
                                   {5, QStringLiteral("5")},
                                   {6, QStringLiteral("6")},
                                   {7, QStringLiteral("7")}},
                                  0, 3);
        m_recOff = codedCombo({{0, QStringLiteral("工作")}, {1, QStringLiteral("关断")}}, 0);
        m_recRstN = codedCombo({{0, QStringLiteral("0")}, {1, QStringLiteral("1")}}, 0);
        m_recRstN->setToolTip(QStringLiteral("RST_N 复位控制位"));
        addField(g, 0, 0, QStringLiteral("增益"), m_recGain);
        addField(g, 0, 1, QStringLiteral("高通"), m_recHighpass);
        addField(g, 1, 0, QStringLiteral("参考"), m_recReference);
        addField(g, 1, 1, QStringLiteral("低通"), m_recLowLp);
        addField(g, 2, 0, QStringLiteral("高功耗"), m_recHighPower);
        addField(g, 2, 1, QStringLiteral("阻抗微调"), m_recTrimImp);
        addField(g, 3, 0, QStringLiteral("通道关断"), m_recOff);
        addField(g, 3, 1, QStringLiteral("RST_N"), m_recRstN);
        m_recPreview = makePreview(pv);
    }

    {
        auto *pv = newTab(QStringLiteral("刺激 DAC"),
                          QStringLiteral("写 STIM/DAC 寄存器 · 操作码 0x06"));
        auto *g = grid;
        m_dacAmpSpin = new QSpinBox;
        // -1 is the "mixed" sentinel (shown via specialValueText, skipped on
        // write); real codes are 0-511.
        m_dacAmpSpin->setRange(kMixedCode, 511);
        m_dacAmpSpin->setSpecialValueText(QString::fromUtf8(kMixedText));
        m_dacAmpSpin->setValue(0);
        m_dacAmpSpin->setToolTip(QStringLiteral("9 位幅度码 (0-511)，输出电流 = 码值 × 步进 × (×20)"));
        connect(m_dacAmpSpin, &QSpinBox::valueChanged, this, [this](int) {
            m_cfgDirtyControls.insert(m_dacAmpSpin);
            updateCfgPreviews();
        });
        m_dacCurrentHint = new QLabel(QStringLiteral("≈ 0 nA"));
        m_dacCurrentHint->setProperty("role", "data-value");
        m_dacPolarity = codedCombo({{0, QStringLiteral("输出0")},
                                    {1, QStringLiteral("负相")},
                                    {2, QStringLiteral("正相")},
                                    {3, QStringLiteral("断开")}},
                                   0, 2);
        m_dacStep = codedCombo({{0, QStringLiteral("4 nA/步")}, {1, QStringLiteral("200 nA/步")}}, 0);
        m_dacComp = codedCombo({{0, QStringLiteral("关")}, {1, QStringLiteral("开")}}, 0);
        m_dacElectrode = codedCombo({{0, QStringLiteral("电极 0")},
                                     {1, QStringLiteral("电极 1")},
                                     {2, QStringLiteral("电极 2")},
                                     {3, QStringLiteral("电极 3")}},
                                    0, 2);
        m_dacElectrode->setToolTip(QStringLiteral(
            "DAC 数据字 [14:13]：在当前外层通道对应的四个电极中选择刺激输出"));
        m_dacOffStim = codedCombo({{1, QStringLiteral("禁用")}, {0, QStringLiteral("使能")}}, 1);
        addField(g, 0, 0, QStringLiteral("幅度码"), m_dacAmpSpin);
        addField(g, 0, 1, QStringLiteral("输出电流"), m_dacCurrentHint);
        addField(g, 1, 0, QStringLiteral("极性"), m_dacPolarity);
        addField(g, 1, 1, QStringLiteral("步进"), m_dacStep);
        addField(g, 2, 0, QStringLiteral("补偿"), m_dacComp);
        addField(g, 2, 1, QStringLiteral("刺激输出"), m_dacOffStim);
        addField(g, 3, 0, QStringLiteral("DAC 电极"), m_dacElectrode);
        m_dacPreview = makePreview(pv);
        m_dacPreview->setToolTip(QStringLiteral(
            "STIM/DAC 32 位命令：OP6 + Block8 + 外层通道2 | "
            "OFF_STIM1 + DAC电极2 + 步进1 + 补偿1 + 极性2 + 幅度9"));
    }

    {
        auto *pv = newTab(QStringLiteral("刺激时序 CT"),
                          QStringLiteral("写 STIM_CT 寄存器 · 操作码 0x1E"));
        auto *g = grid;
        const std::initializer_list<Opt> timeOpts = {
            {0, QStringLiteral("25 µs")},  {1, QStringLiteral("50 µs")},
            {2, QStringLiteral("100 µs")}, {3, QStringLiteral("200 µs")},
            {4, QStringLiteral("400 µs")}, {5, QStringLiteral("800 µs")},
            {6, QStringLiteral("1.6 ms")}, {7, QStringLiteral("3.2 ms")},
            {8, QStringLiteral("6.4 ms")}};
        m_ctTimePos = codedCombo(timeOpts, 0, 4);
        m_ctTimeNeg = codedCombo(timeOpts, 0, 4);
        m_ctGlobalFreq = codedCombo({{0, QStringLiteral("16 Hz")},
                                     {1, QStringLiteral("80 Hz")},
                                     {2, QStringLiteral("250 Hz")},
                                     {3, QStringLiteral("500 Hz")},
                                     {4, QStringLiteral("2 kHz")},
                                     {5, QStringLiteral("5 kHz")},
                                     {6, QStringLiteral("10 kHz")},
                                     {7, QStringLiteral("20 kHz")}},
                                    0, 3);
        m_ctLocalFreq = codedCombo({{0, QStringLiteral("÷2")},
                                    {1, QStringLiteral("÷4")},
                                    {2, QStringLiteral("÷8")},
                                    {3, QStringLiteral("÷16")}},
                                   0, 2);
        m_ctPolFirst = codedCombo({{0, QStringLiteral("负相先")}, {1, QStringLiteral("正相先")}}, 0);
        m_ctAmpX20 = codedCombo({{0, QStringLiteral("关")}, {1, QStringLiteral("开 (×20)")}}, 0);
        m_ctOnOff = codedCombo({{0, QStringLiteral("关")}, {1, QStringLiteral("开")}}, 0);
        addField(g, 0, 0, QStringLiteral("正相脉宽"), m_ctTimePos);
        addField(g, 0, 1, QStringLiteral("负相脉宽"), m_ctTimeNeg);
        addField(g, 1, 0, QStringLiteral("全局频率"), m_ctGlobalFreq);
        addField(g, 1, 1, QStringLiteral("局部分频"), m_ctLocalFreq);
        addField(g, 2, 0, QStringLiteral("相序"), m_ctPolFirst);
        addField(g, 2, 1, QStringLiteral("电流×20"), m_ctAmpX20);
        addField(g, 3, 0, QStringLiteral("时序开关"), m_ctOnOff);
        m_ctPreview = makePreview(pv);
    }

    // Persistent action bar: send options + the write buttons stay visible
    // regardless of which tab is open. 全部写入 is the single primary action.
    auto *actionBar = new QHBoxLayout;
    actionBar->setSpacing(6);
    auto *delayLbl = new QLabel(QStringLiteral("延时"));
    delayLbl->setProperty("role", "field-label");
    actionBar->addWidget(delayLbl);
    m_cfgDelaySpin = new QSpinBox;
    m_cfgDelaySpin->setRange(0, 5000);
    m_cfgDelaySpin->setValue(10);
    m_cfgDelaySpin->setSuffix(QStringLiteral(" ms"));
    m_cfgDelaySpin->setToolTip(QStringLiteral(
        "相邻 SPI 命令之间的延时；每条写命令后会自动发送一条 dummy(全0)"));
    actionBar->addWidget(m_cfgDelaySpin);
    actionBar->addStretch(1);
    m_btnWriteCurrent = new QPushButton(QStringLiteral("写入当前页"));
    m_btnWriteCurrent->setProperty("variant", "secondary");
    m_btnWriteCurrent->setToolTip(QStringLiteral("只把当前选项卡的寄存器写入所有选中通道"));
    connect(m_btnWriteCurrent, &QPushButton::clicked, this, [this]() {
        switch (m_cfgTabs ? m_cfgTabs->currentIndex() : 0) {
        case 0: applyRecSection(); break;
        case 1: applyDacSection(); break;
        default: applyCtSection(); break;
        }
    });
    actionBar->addWidget(m_btnWriteCurrent);
    m_btnWriteAll = new QPushButton(QStringLiteral("全部写入"));
    m_btnWriteAll->setProperty("variant", "primary");
    m_btnWriteAll->setMinimumWidth(88);
    m_btnWriteAll->setToolTip(QStringLiteral("对所有选中通道依次写入 REC、DAC、DAC_CT 三个寄存器"));
    connect(m_btnWriteAll, &QPushButton::clicked, this, &ChannelMapPanel::applyAllSections);
    actionBar->addWidget(m_btnWriteAll);
    v->addLayout(actionBar);

    rightLayout->addWidget(grp);
    updateCfgPreviews();
}

void ChannelMapPanel::setMapTheme(const QMap<QString, QString> &palette) {
    m_mapView->setThemePalette(palette);
}

void ChannelMapPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    // Self-heal the splitter: if an early narrow layout squeezed the config
    // column below its minimum, later growth all goes to the map (stretch
    // factors 1/0) and the column stays a sliver forever. The splitter's
    // child layout only settles after this event, so inspect it on the next
    // event-loop turn — sizes() read here would still report the old values.
    QTimer::singleShot(0, this, [this]() {
        if (!m_splitter || m_splitter->count() < 2) {
            return;
        }
        const QList<int> sizes = m_splitter->sizes();
        const int rightMin = m_splitter->widget(1)->minimumWidth();
        const int leftMin = qMax(200, m_splitter->widget(0)->minimumSizeHint().width());
        constexpr int kRightDesignWidth = 520;
        // Heal as soon as both panes can coexist at their minimums; give the
        // right pane its design width when there is room, otherwise whatever
        // fits above its minimum.
        if (sizes.size() == 2 && sizes[1] < rightMin &&
            m_splitter->width() > leftMin + rightMin + 40) {
            const int rightW = qBound(rightMin, m_splitter->width() - leftMin - 40,
                                      kRightDesignWidth);
            m_splitter->setSizes({m_splitter->width() - rightW, rightW});
        }
    });
}

}  // namespace ccv2
