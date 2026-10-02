// End-to-end processing checks: real shell, public Qt controls, synthetic ADC
// bytes through its ordinary replay loader. No private-member access, injected
// stores, test-only detector path, or truth copied into application output.
#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QHostAddress>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QTcpServer>
#include <QTabWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
#include <QFormLayout>
#include <QGridLayout>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <QSet>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

#include "config/config_manager.h"
#include "core/constants.h"
#include "core/network_state.h"
#include "io/session_manifest.h"
#include "core/tdm_context.h"
#include "service/session_hub.h"
#include "service/dummy_stream_server.h"
#include "ui/channel_map_panel.h"
#include "ui/spi_control_panel.h"
#include "ui/command_center_main_window.h"
#include "ui/realtime_plot_panel.h"
#include "ui/analyzer_panel.h"
#include "ui/sweep_plot_panel.h"
#include "ui/spike_panel.h"
#include "ui/waveform_widget.h"
#include "ui/widgets/activity_map_view.h"
#include "ui/widgets/stack_waveform_view.h"
#include "ui/widgets/sweep_waveform_view.h"
#include "ui/widgets/spike_detail_window.h"
#include "ui/widgets/spike_grid_view.h"
#include "ui/widgets/spike_sorting_widget.h"
#include "ui/widgets/session_toolbar.h"

namespace {
constexpr int kFrames = 16000;
constexpr double kFs = 20000.0;
int checks = 0;
void require(bool ok, const QString &what) {
    if (!ok) { std::cerr << "ASSERTION FAILED: " << what.toStdString() << std::endl; throw std::runtime_error(what.toStdString()); }
    ++checks;
    std::cout << "PASS " << what.toStdString() << std::endl;
}
void pump(int ms) {
    QElapsedTimer t; t.start();
    do { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2); }
    while (t.elapsed() < ms);
}
bool until(const std::function<bool()> &predicate, int ms = 5000) {
    QElapsedTimer t; t.start();
    while (!predicate() && t.elapsed() < ms) pump(10);
    return predicate();
}
template<class T> T *named(QObject *root, const char *name) {
    auto *w = root->findChild<T *>(QString::fromLatin1(name));
    if (!w) throw std::runtime_error(std::string("Missing control ") + name);
    return w;
}
QPushButton *button(QObject *root, const QString &text, bool tip = false) {
    for (auto *b : root->findChildren<QPushButton *>())
        if ((tip ? b->toolTip() : b->text()) == text) return b;
    throw std::runtime_error(("Missing button " + text).toStdString());
}
QCheckBox *check(QObject *root, const QString &text) {
    for (auto *b : root->findChildren<QCheckBox *>()) if (b->text() == text) return b;
    throw std::runtime_error(("Missing checkbox " + text).toStdString());
}
QComboBox *combo(QObject *root, const QString &item) {
    for (auto *c : root->findChildren<QComboBox *>()) if (c->findText(item) >= 0) return c;
    throw std::runtime_error(("Missing combo item " + item).toStdString());
}
template<class T> T *field(QObject *root, const QString &text) {
    for (auto *l : root->findChildren<QLayout *>()) {
        if (auto *f = qobject_cast<QFormLayout *>(l)) {
            for (int r = 0; r < f->rowCount(); ++r) {
                auto *i = f->itemAt(r, QFormLayout::LabelRole);
                auto *label = i ? qobject_cast<QLabel *>(i->widget()) : nullptr;
                auto *v = f->itemAt(r, QFormLayout::FieldRole);
                if (label && label->text() == text && v)
                    if (auto *w = qobject_cast<T *>(v->widget())) return w;
            }
        }
        if (auto *g = qobject_cast<QGridLayout *>(l)) {
            for (int r = 0; r < g->rowCount(); ++r) for (int col = 0; col < g->columnCount()-1; ++col) {
                auto *i = g->itemAtPosition(r, col);
                auto *label = i ? qobject_cast<QLabel *>(i->widget()) : nullptr;
                auto *v = g->itemAtPosition(r, col+1);
                if (label && label->text() == text && v)
                    if (auto *w = qobject_cast<T *>(v->widget())) return w;
            }
        }
    }
    throw std::runtime_error(("Missing field " + text).toStdString());
}
template<class T> T *suffix(QObject *root, const QString &text) {
    for (auto *w : root->findChildren<T *>()) if (w->suffix() == text) return w;
    throw std::runtime_error(("Missing suffix " + text).toStdString());
}
void edit(QLineEdit *w, const QString &text) {
    w->setText(text);
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(w, &key); pump(15);
}
void tab(ccv2::CommandCenterMainWindow &w, const QString &text) {
    button(&w, text)->click(); pump(100);
}
QString tableText(QTableWidget *t, const QString &row, int column = 1) {
    for (int r = 0; r < t->rowCount(); ++r)
        if (t->item(r, 0) && t->item(r, 0)->text() == row)
            return t->item(r, column)->text();
    throw std::runtime_error(("Missing table row " + row).toStdString());
}
double number(const QString &s) {
    bool ok = false;
    const double value = s.section(' ', 0, 0).toDouble(&ok);
    if (!ok || !std::isfinite(value)) { std::cerr << "INVALID NUMERIC DISPLAY: " << s.toStdString() << std::endl; throw std::runtime_error(("Not a finite value: " + s).toStdString()); }
    return value;
}
double volts(const QString &s) {
    double v = number(s);
    if (s.contains("mV")) v *= 1e-3;
    else if (s.contains("uV") || s.contains(QString::fromUtf8("µV"))) v *= 1e-6;
    return v;
}
QByteArray fixture() {
    QByteArray bytes(kFrames * ccv2::kFrameBytes, Qt::Uninitialized);
    for (int f = 0; f < kFrames; ++f) for (int ch = 0; ch < ccv2::kChannelsTotal; ++ch) {
        double v = 2048;
        if (ch == 0 || ch == 2) v += 40 * std::sin(2 * 3.14159265358979323846 * f / 20.0);
        if (ch == 1 || ch == 3) {
            const double x = (f % 500) - 160;
            v -= 30 * std::exp(-x*x/10.0);
            v += 10 * std::exp(-(x-7)*(x-7)/20.0);
        }
        const quint32 raw = (quint32(f) << ccv2::kTimestampShift) | quint32(std::lround(v));
        qToLittleEndian<quint32>(raw, reinterpret_cast<uchar *>(bytes.data() + f * ccv2::kFrameBytes + ch * 4));
    }
    return bytes;
}
// A real QFileDialog is opened by the real button. Only select/accept its
// ordinary public UI; message boxes are acknowledged without altering outputs.
void fileAction(QPushButton *b, const QString &path, bool cancel = false) {
    bool saw = false;
    QPointer<QFileDialog> prepared;
    QElapsedTimer timeout; timeout.start();
    QTimer handler;
    QObject::connect(&handler, &QTimer::timeout, [&]() {
        for (auto *top : QApplication::topLevelWidgets()) {
            if (auto *d = qobject_cast<QFileDialog *>(top); d && d->isVisible()) {
                saw = true;
                if (cancel) d->reject();
                else if (timeout.elapsed()>6000) d->reject();
                else if(prepared!=d) { prepared=d; d->setDirectory(QFileInfo(path).absolutePath()); d->selectFile(QFileInfo(path).fileName()); }
                else {
                    if(auto *name=d->findChild<QLineEdit *>("fileNameEdit")) name->setText(QFileInfo(path).fileName());
                    static_cast<QDialog *>(d)->accept();
                }
            }
            if (auto *m = qobject_cast<QMessageBox *>(top); m && m->isVisible()) m->accept();
        }
    });
    handler.start(20); b->click(); pump(40); handler.stop();
    require(saw, "actual file chooser exercised");
}
void mouse(QWidget *w, QEvent::Type type, const QPoint &p, Qt::MouseButton b, Qt::MouseButtons held, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent e(type, QPointF(p), QPointF(w->mapToGlobal(p)), b, held, mods);
    QApplication::sendEvent(w, &e);
}
QPoint heatCell(ccv2::ActivityMapView *w, int col, int row) {
    // The view's public 16x16 physical grid has a 24px title and 48px legend.
    const int side=qMax(1,qMin(w->width()-8,w->height()-72));
    const double left=(w->width()-side)*.5+4, size=side-8;
    return QPoint(qRound(left+(col+.5)*size/16),qRound(24+(row+.5)*size/16));
}
bool warningAction(const std::function<void()> &action) {
    bool seen=false; QTimer timer;
    QObject::connect(&timer,&QTimer::timeout,[&]{for(auto *w:QApplication::topLevelWidgets()) if(auto *m=qobject_cast<QMessageBox *>(w);m&&m->isVisible()){seen=true;m->accept();}});
    timer.start(20); action(); pump(30); timer.stop(); return seen;
}
void writeLongTone(const QString &path, int frames) {
    QFile file(path); require(file.open(QIODevice::WriteOnly),"long FFT fixture writable");
    // Chunked generation bounds memory; every value remains ADC fixture input.
    for(int start=0;start<frames;start+=4096) {
        const int count=qMin(4096,frames-start); QByteArray bytes(count*ccv2::kFrameBytes,Qt::Uninitialized);
        for(int i=0;i<count;++i) {
            const int frame=start+i; const quint32 ts=quint32(frame)<<ccv2::kTimestampShift;
            const int tone=int(std::lround(2048+40*std::sin(2*3.14159265358979323846*frame/20.0)));
            for(int ch=0;ch<256;++ch) qToLittleEndian<quint32>(ts|quint32(ch==0||ch==2?tone:2048),reinterpret_cast<uchar *>(bytes.data()+i*ccv2::kFrameBytes+4*ch));
        }
        if(file.write(bytes)!=bytes.size()) throw std::runtime_error("long FFT fixture write failed");
    }
    file.close(); require(QFileInfo(path).size()==qint64(frames)*ccv2::kFrameBytes,"long FFT fixture frame-aligned size");
}
void clickAt(QWidget *w, QPoint p) {
    mouse(w, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
    mouse(w, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton); pump(30);
}

class PaintCounter final : public QObject {
public:
    int paints = 0;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint) ++paints;
        return false;
    }
};
void finishSpinEdit(QAbstractSpinBox *spin) {
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(spin, &key); pump(30);
}
// Small independent field-edit scenario; included in the full regression and
// available alone so these controls do not require regenerating the large FFT.
void parameterControls(ccv2::CommandCenterMainWindow &window, ccv2::ConfigManager &cfg,
                       const QString &path) {
    auto *hub=window.findChild<ccv2::SessionHub *>();
    auto *an=window.findChild<ccv2::AnalyzerPanel *>();
    auto *sw=window.findChild<ccv2::SweepPlotPanel *>();
    auto *toolbar=window.findChild<ccv2::SessionToolbar *>();
    auto *anFs=field<QDoubleSpinBox>(an,"采样率 fs");
    auto *anRefresh=field<QDoubleSpinBox>(an,"刷新率");
    auto *anPoints=field<QSpinBox>(an,"波形点数");
    auto *swFs=field<QDoubleSpinBox>(sw,"采样率");
    auto *swRefresh=field<QSpinBox>(sw,"刷新");
    auto *play=named<QPushButton>(toolbar,"transportPlay");
    const auto run=[&] {
        if(hub->state()==ccv2::SessionHub::State::ReplayPlaying) play->click();
        button(toolbar,"跳到开头",true)->click(); pump(40); play->click();
        require(until([&]{return hub->currentFrameIndex()==kFrames;},10000),"parameter-control replay consumes all input frames");
        pump(300);
    };
    const auto closeReplay=[&] {
        named<QPushButton>(toolbar,"fileChipClose")->click();
        require(until([&]{return hub->state()==ccv2::SessionHub::State::Idle;}),"parameter-control unload returns Idle");
        pump(60);
    };
    const auto measurePaints=[&](QWidget *widget,int ms) {
        PaintCounter counter; widget->installEventFilter(&counter); pump(ms);
        widget->removeEventFilter(&counter); return counter.paints;
    };
    QTableWidget *timeTable=nullptr,*fftTable=nullptr;
    for(auto *t:an->findChildren<QTableWidget *>()) {if(t->columnCount()==2)timeTable=t;if(t->columnCount()==3)fftTable=t;}
    require(timeTable&&fftTable,"parameter-control statistics widgets found");
    tab(window,"分析器");
    require(hub->state()==ccv2::SessionHub::State::Idle&&anFs->isEnabled(),"Analyzer idle sampling-rate editor is enabled");
    anFs->setValue(10000); anRefresh->setValue(5); anPoints->setValue(1024); finishSpinEdit(anPoints);
    require(until([&]{const auto s=cfg.load().value("Signal");return s.value("sampling_rate")=="10000"&&s.value("refresh_hz")=="5"&&s.value("wave_points")=="1024";}),"Analyzer fs, refresh and wave-point edits persist exact values");
    require(an->configuredLiveSampleRate()==10000,"Analyzer idle fs edit updates global source fallback");
    button(toolbar,"回放")->click(); fileAction(button(toolbar,"打开文件…"),path);
    require(hub->isReplaying()&&hub->sampleRate()==10000&&!anFs->isEnabled()&&anFs->value()==10000,"bare replay uses edited 10 kHz fallback and locks Analyzer fs to source");
    run();
    require(until([&]{return tableText(timeTable,"Samples")=="1024..1024";}),"Analyzer 1024-point edit changes both real output sample counts");
    require(std::abs(volts(tableText(timeTable,"Mean(avg)"))-.9)<.001&&std::abs(number(tableText(fftTable,"Fin"))-500)<10,"edited replay rate gives expected 500 Hz tone and 0.9 V mean");
    auto *wave=an->findChildren<ccv2::WaveformWidget *>().first();
    const int slowAnalyzer=measurePaints(wave,800);
    anRefresh->setValue(50); pump(120); const int fastAnalyzer=measurePaints(wave,800);
    std::cout<<"MEASURE Analyzer redraws 5Hz="<<slowAnalyzer<<" 50Hz="<<fastAnalyzer<<std::endl;
    require(slowAnalyzer>=2&&fastAnalyzer>slowAnalyzer*2,"Analyzer refresh edit changes actual waveform repaint cadence");
    anPoints->setValue(768); finishSpinEdit(anPoints); run();
    require(until([&]{return tableText(timeTable,"Samples")=="768..768";}),"Analyzer wave-point editingFinished rebuilds a 768-sample data view");
    require(std::abs(volts(tableText(timeTable,"AC RMS(avg)"))-(40*1.8/4096/std::sqrt(2.0)))<.0004,"edited point window retains independently expected sine RMS");
    auto *overview=button(an,"256通道概览"); overview->click(); pump(80);
    require(!anRefresh->isEnabled()&&!anPoints->isEnabled()&&anRefresh->value()==.5&&anPoints->value()==1024&&!anFs->isEnabled(),"Analyzer overview locks refresh and points while source fs remains locked");
    require(until([&]{const auto s=cfg.load().value("Signal");return s.value("refresh_hz")=="50"&&s.value("wave_points")=="768";}),"overview persists ordinary user refresh and points instead of temporary limits");
    overview->click(); pump(80);
    require(anRefresh->isEnabled()&&anPoints->isEnabled()&&anRefresh->value()==50&&anPoints->value()==768&&!anFs->isEnabled(),"overview exit restores edited controls and retains source fs lock");
    closeReplay();
    require(anFs->isEnabled()&&anFs->value()==10000,"Analyzer unload restores editable configured sampling rate");
    anFs->setValue(20000); anRefresh->setValue(10); anPoints->setValue(4096); finishSpinEdit(anPoints);
    require(until([&]{return cfg.load().value("Signal").value("sampling_rate")=="20000";}),"Analyzer rate restored to 20 kHz for subsequent replay");
    tab(window,"实时spike");
    require(!swFs->isEnabled(),"hub-bound Sweep fs is a readout even while idle");
    require(cfg.load().value("Sweep").value("sampling_rate")=="12000","shared Sweep readout preserves the legacy configured rate");
    swRefresh->setValue(10);
    require(until([&]{return cfg.load().value("Sweep").value("refresh_hz")=="10";}),"Sweep refresh edit persists 10 Hz");
    fileAction(button(toolbar,"打开文件…"),path);
    require(hub->sampleRate()==20000&&swFs->value()==20000,"Sweep source activation uses global 20 kHz rather than stale local rate");
    run();
    auto *map=sw->findChild<ccv2::ActivityMapView *>();
    require(map&&map->maxRms()>.010&&map->maxRms()<.014,"Sweep parameter scenario processes expected sine RMS");
    const int slowSweep=measurePaints(map,800);
    swRefresh->setValue(60); pump(120); const int fastSweep=measurePaints(map,800);
    std::cout<<"MEASURE Sweep redraws 10Hz="<<slowSweep<<" 60Hz="<<fastSweep<<std::endl;
    require(slowSweep>=3&&fastSweep>slowSweep*2,"Sweep refresh edit changes actual heatmap repaint cadence");
    require(until([&]{return cfg.load().value("Sweep").value("refresh_hz")=="60";}),"Sweep 60 Hz refresh persists while source rate remains 20 kHz");
    require(!swFs->isEnabled()&&swFs->value()==hub->sampleRate(),"Sweep source-controlled fs cannot diverge from the active replay rate");
    swRefresh->setValue(40); closeReplay();
    // Short manifest-backed inputs exercise source rates outside the old
    // integer 2k..500k editor bounds. Metadata is fixture input, not an oracle
    // written into an application output.
    const double oldSpan=field<QDoubleSpinBox>(sw,"时间窗")->value();
    auto *hp=field<QSpinBox>(sw,"高通"), *lp=field<QSpinBox>(sw,"Spike低通");
    const int oldHp=hp->value(), oldLp=lp->value();
    constexpr int rateFrames=2048;
    for(double rate: {1000.0,20000.125,1000000.0}) {
        QTemporaryDir rateDir; require(rateDir.isValid(),"source-rate fixture directory available");
        QFile source(path), rateFile(rateDir.filePath("rate-tone.bin"));
        require(source.open(QIODevice::ReadOnly)&&rateFile.open(QIODevice::WriteOnly),"source-rate fixture input opened");
        const auto rateBytes=source.read(rateFrames*ccv2::kFrameBytes);
        require(rateFile.write(rateBytes)==rateFrames*ccv2::kFrameBytes,"short source-rate fixture written"); rateFile.close();
        ccv2::SessionManifestData manifest;
        manifest.metadata.source="synthetic_qa"; manifest.metadata.sampleRate=rate;
        manifest.parts.push_back({"rate-tone.bin",rateBytes.size(),rateFrames});
        manifest.totalBytes=rateBytes.size(); manifest.totalFrames=rateFrames; manifest.complete=true;
        QString error;
        require(ccv2::SessionManifest::write(rateDir.path(),manifest,&error),"source-rate manifest written: "+error);
        const double span=rate>500000?.2:4.0;
        field<QDoubleSpinBox>(sw,"时间窗")->setValue(span);
        fileAction(button(toolbar,"打开文件…"),rateFile.fileName());
        require(hub->sampleRate()==rate&&!swFs->isEnabled()&&swFs->value()==rate,"Sweep readout preserves exact manifest source rate "+QString::number(rate,'g',17));
        require(lp->maximum()==int(std::floor(rate*.45))&&hp->maximum()==lp->maximum()-50,"Sweep frequency limits use actual manifest rate "+QString::number(rate,'g',17));
        require(an->configuredLiveSampleRate()==20000,"manifest rate does not overwrite configured source fallback");
        play->click(); require(until([&]{return hub->currentFrameIndex()==rateFrames;},5000),"rate-boundary replay consumes all 2048 source frames"); pump(250);
        auto *view=sw->findChild<ccv2::SweepWaveformView *>(); const auto image=view->grab().toImage();
        int cursor=-1;
        for(int x=0;x<image.width();++x) {
            int red=0; for(int y=0;y<image.height();++y) if(image.pixelColor(x,y)==QColor(0xf8,0x51,0x49))++red;
            if(red>image.height()/2) {cursor=x;break;}
        }
        // Independent timebase expectation: N/fs seconds traverses N/(fs*span)
        // of the plot width, wrapping after one span. Layout margins are 52/10.
        const int plotWidth=view->width()-62;
        const int expected=52+(int(std::floor(rateFrames*plotWidth/(rate*span)))%plotWidth);
        std::cout<<"MEASURE Sweep source="<<rate<<" cursor="<<cursor<<" expected="<<expected<<std::endl;
        require(cursor>=0&&std::abs(cursor-expected)<=1,"Sweep cursor timebase follows exact source rate "+QString::number(rate,'g',17));
        require(cfg.load().value("Sweep").value("sampling_rate")=="12000","source-driven Sweep state leaves legacy rate untouched");
        closeReplay();
    }
    // Return to the ordinary source before the main processing regression.
    fileAction(button(toolbar,"打开文件…"),path);
    require(hub->sampleRate()==20000&&swFs->value()==20000&&!swFs->isEnabled(),"Sweep readout resynchronizes to normal 20 kHz source");
    field<QDoubleSpinBox>(sw,"时间窗")->setValue(oldSpan); hp->setValue(oldHp); lp->setValue(oldLp); closeReplay();
    {
        ccv2::NetworkState standaloneNetwork("127.0.0.1",1,1);
        ccv2::SweepPlotPanel standalone(&cfg,&standaloneNetwork); standalone.show(); pump(30);
        auto *legacyFs=field<QDoubleSpinBox>(&standalone,"采样率");
        require(legacyFs->isEnabled()&&legacyFs->decimals()==0&&legacyFs->minimum()==2000&&legacyFs->maximum()==500000&&legacyFs->value()==12000,"unbound Sweep retains its editable integer-rate contract");
        legacyFs->setValue(12345); finishSpinEdit(legacyFs);
        require(until([&]{return cfg.load().value("Sweep").value("sampling_rate")=="12345";}),"unbound Sweep rate edit persists independently");
        legacyFs->setValue(12000); standalone.shutdown(); standalone.close();
    }
    tab(window,"通道图");
}

}
int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("NeuralLabDummyQa");
    QCoreApplication::setApplicationName("DummyProcessingUiE2E");
    QTemporaryDir temp;
    if (!temp.isValid()) return 2;
    qputenv("XDG_CONFIG_HOME", temp.filePath("config").toUtf8());
    try {
        const QString path = temp.filePath("known-synthetic-adc.bin");
        QFile f(path); require(f.open(QIODevice::WriteOnly), "fixture writable");
        const auto data = fixture(); require(f.write(data) == data.size(), "fixture written"); f.close();
        QTcpServer reserve; require(reserve.listen(QHostAddress::LocalHost,0), "allocate loopback Dummy port");
        const int dummyPort=reserve.serverPort(); reserve.close();
        ccv2::DummyStreamServer dummy; QString dummyError;
        require(dummy.start(dummyPort,dummyPort,&dummyError), "built-in Dummy control server starts: "+dummyError);
        ccv2::ConfigManager cfg;
        auto settings = cfg.load();
        settings["Network"]["host"] = "127.0.0.1";
        settings["Network"]["port"] = QString::number(dummyPort);
        settings["Network"]["data_port"] = QString::number(dummyPort);
        settings["Session"]["tdm_enabled"] = "0";
        settings["UI"]["theme"] = "dark";
        settings["Realtime"]["channels"] = "0,2";
        settings["Realtime"]["wave_points"] = "1200";
        settings["Analyzer"]["channels"] = "0,2";
        settings["Sweep"]["sampling_rate"] = "12000";
        settings["Signal"]["sampling_rate"] = "20000";
        settings["Signal"]["wave_points"] = "4096";
        settings["Signal"]["fft_enabled"] = "1";
        settings["Signal"]["fft_points"] = "2048";
        settings["Signal"]["fft_bandwidth_hz"] = "10000";
        settings["SpikePanel"]["threshold_units"] = "input_referred";
        settings["SpikePanel"]["input_gain"] = "60";
        settings["SpikePanel"]["threshold_mode"] = "1";
        settings["SpikePanel"]["threshold_value"] = "30";
        settings["SpikePanel"]["retain"] = "200";
        require(cfg.save(settings), "isolated configuration saved");
        ccv2::CommandCenterMainWindow window;
        struct CloseOnExit { QWidget &w; ~CloseOnExit() { w.close(); pump(30); } } closeOnExit{window};
        window.resize(1600, 1000); window.show(); pump(100);
        auto *hub = window.findChild<ccv2::SessionHub *>();
        auto *rt = window.findChild<ccv2::RealtimePlotPanel *>();
        auto *an = window.findChild<ccv2::AnalyzerPanel *>();
        auto *sw = window.findChild<ccv2::SweepPlotPanel *>();
        auto *sp = window.findChild<ccv2::SpikePanel *>();
        auto *toolbar = window.findChild<ccv2::SessionToolbar *>();
        require(hub && rt && an && sw && sp && toolbar, "real shell constructs all processing pages");
        if(!app.arguments().contains("--map-only")) parameterControls(window,cfg,path);
        if(app.arguments().contains("--parameter-controls-only")) { std::cout << "dummy_processing_ui_e2e parameter-controls-only ok checks=" << checks << std::endl; return 0; }
        // CHANNEL MAP: real selection -> serialized SPI queue -> Dummy model ->
        // applied-command shadow feedback. Simulation is not board readback.
        auto *channels=window.findChild<ccv2::ChannelMapPanel *>();
        auto *address=window.findChild<ccv2::ChannelAddressState *>();
        auto *spi=window.findChild<ccv2::SpiControlPanel *>();
        require(channels&&address&&spi,"real map and hardware-control panels exist");
        tab(window,"通道图"); button(channels,"全选")->click();
        require(address->selectedGlobalChannels().size()==256,"map select-all selects 256 ADC channels");
        button(channels,"清空")->click(); require(address->selectedGlobalChannels().isEmpty(),"map clear removes all selected channels");
        QLineEdit *global=nullptr,*blocks=nullptr;
        for(auto *e:channels->findChildren<QLineEdit *>()) { if(e->placeholderText()=="0-31,64,128-140") global=e; else if(e->maximumWidth()==110) blocks=e; }
        require(global&&blocks,"map range controls found");
        edit(blocks,"0"); button(channels,"批量选中")->click();
        require(address->selectedGlobalChannels()==QVector<int>({0,1,2,3}),"map block/local batch selection maps to ADC IDs");
        button(channels,"批量取消")->click(); require(address->selectedGlobalChannels().isEmpty(),"map batch unselect removes same IDs");
        edit(blocks,"0-1"); check(channels,"00")->setChecked(false); check(channels,"10")->setChecked(false);
        button(channels,"批量选中")->click(); require(address->selectedGlobalChannels()==QVector<int>({1,3,5,7}),"local checkbox subset selects exact block/local ADC IDs");
        const auto selectedBeforeBad=address->selectedGlobalChannels(); edit(blocks,"bad-range");
        require(warningAction([&]{button(channels,"批量选中")->click();})&&address->selectedGlobalChannels()==selectedBeforeBad,"invalid block range warns without changing selection");
        edit(global,"0,not-a-channel"); require(warningAction([&]{button(channels,"应用选择")->click();})&&address->selectedGlobalChannels()==selectedBeforeBad,"invalid global range warns without changing selection");
        button(channels,"清空")->click(); check(channels,"00")->setChecked(true); check(channels,"10")->setChecked(true); edit(blocks,"0");
        edit(global,"0,1"); button(channels,"应用选择")->click();
        require(address->selectedGlobalChannels()==QVector<int>({0,1}),"map explicit global selection applied");
        auto *cfgTabs=named<QTabWidget>(channels,"cfgTabs");
        auto *writeCurrent=button(channels,"写入当前页");
        combo(cfgTabs->widget(0),"180×")->setCurrentText("180×");
        combo(cfgTabs->widget(0),"VREF")->setCurrentText("VREF");
        writeCurrent->click(); require(!writeCurrent->isEnabled(),"batch queue disables map writes while busy");
        require(until([&]{return writeCurrent->isEnabled()&&dummy.protocolState().rec[0]==0x41&&dummy.protocolState().rec[1]==0x41;}),"map REC batch reaches Dummy register state");
        cfgTabs->setCurrentIndex(1); field<QSpinBox>(cfgTabs->widget(1),"幅度码")->setValue(7); writeCurrent->click();
        require(until([&]{return writeCurrent->isEnabled()&&dummy.protocolState().dac[0]==0x8007&&dummy.protocolState().dac[1]==0x8007;}),"map DAC batch updates selected simulated registers");
        cfgTabs->setCurrentIndex(2); auto *pos=field<QComboBox>(cfgTabs->widget(2),"正相脉宽"); pos->setCurrentIndex(pos->findData(2)); writeCurrent->click();
        require(until([&]{return writeCurrent->isEnabled()&&dummy.protocolState().dacCt[0]==0x20&&dummy.protocolState().dacCt[1]==0x20;}),"map CT batch updates simulated timing registers");
        const auto spiBefore=dummy.protocolState().spiCommands; button(channels,"全部写入")->click();
        require(until([&]{return writeCurrent->isEnabled()&&dummy.protocolState().spiCommands>=spiBefore+12;}),"map write-all sends 3 registers x 2 channels plus dummy words");
        require(until([&]{ return !cfg.load().value("ChannelShadow").value("rec").isEmpty(); }), "shadow debounce persisted register data");
        const auto shadow=cfg.load().value("ChannelShadow");
        require(shadow.value("rec").left(8).toUpper()=="00410041"&&shadow.value("dac").left(8).toUpper()=="80078007"&&shadow.value("dac_ct").left(8).toUpper()=="00200020","successful SPI writes persist exact GUI shadow values");
        edit(global,"0,2"); button(channels,"应用选择")->click(); pump(50);
        require(until([&]{return combo(cfgTabs->widget(0),"180×")->currentData().toInt()==-1;}),"mixed per-channel REC fields remain explicitly mixed");
        auto *zoom=channels->findChild<QSlider *>(); require(zoom!=nullptr,"map zoom control present");
        const int zoomBefore=zoom->value(); button(channels,"+")->click(); require(zoom->value()>zoomBefore,"map zoom-in control changes zoom");
        button(channels,"-")->click(); button(channels,"适应窗口")->click(); require(zoom->value()==100,"map fit restores 100 percent fit scale");
        auto *electrodeView=channels->findChild<QGraphicsView *>(); require(electrodeView&&electrodeView->scene(),"actual electrode graphics view found");
        const auto electrodeCount=[&]{int n=0;for(auto *item:electrodeView->scene()->items())if(item->flags()&QGraphicsItem::ItemIsSelectable)++n;return n;};
        check(channels,"仅显示512引出ELE")->setChecked(false); pump(40); require(electrodeCount()==1024,"full map contains 1024 physical electrodes");
        check(channels,"仅显示512引出ELE")->setChecked(true); pump(40); require(electrodeCount()==512,"routed-only map contains 512 electrodes");
        check(channels,"仅显示512引出ELE")->setChecked(false); button(channels,"适应窗口")->click(); pump(80);
        auto *history=channels->findChild<QListWidget *>(); const int historyBefore=history->count(); bool clickedElectrode=false;
        for(auto *item:electrodeView->scene()->items()) if(item->flags()&QGraphicsItem::ItemIsSelectable) {
            const QPoint at=electrodeView->mapFromScene(item->sceneBoundingRect().center());
            if(electrodeView->viewport()->rect().contains(at)) { clickAt(electrodeView->viewport(),at); clickedElectrode=true; break; }
        }
        require(clickedElectrode&&history->count()==historyBefore+1&&address->selectedGlobalChannels().size()==1,"electrode mouse click updates channel selection and history");
        button(channels,"清空")->click();
        QWidget *mapViewport=electrodeView->viewport(); const QPoint bandStart(8,8), bandEnd(mapViewport->width()/2,mapViewport->height()/2);
        mouse(mapViewport,QEvent::MouseButtonPress,bandStart,Qt::LeftButton,Qt::LeftButton);
        mouse(mapViewport,QEvent::MouseMove,bandStart+QPoint(30,30),Qt::NoButton,Qt::LeftButton);
        mouse(mapViewport,QEvent::MouseMove,bandEnd,Qt::NoButton,Qt::LeftButton); pump(30);
        const QRect selectionRect=electrodeView->rubberBandRect();
        require(selectionRect.width()>30&&selectionRect.height()>30,"map drag creates an actual rubber-band selection rectangle");
        QSet<int> expectedBandChannels;
        for(auto *item:electrodeView->scene()->items(electrodeView->mapToScene(selectionRect),Qt::IntersectsItemShape)) if(item->flags()&QGraphicsItem::ItemIsSelectable) {
            const auto match=QRegularExpression("Global=(\\d+)").match(item->toolTip());
            if(match.hasMatch()) expectedBandChannels.insert(match.captured(1).toInt());
        }
        mouse(mapViewport,QEvent::MouseButtonRelease,bandEnd,Qt::LeftButton,Qt::NoButton); pump(50);
        QSet<int> actualBandChannels; for(int ch:address->selectedGlobalChannels()) actualBandChannels.insert(ch);
        require(expectedBandChannels.size()>1&&actualBandChannels==expectedBandChannels,"map rubber-band selection yields exact intersected ADC channel set");
        const int ctrlZoomBefore=zoom->value(); const double scaleBefore=electrodeView->transform().m11(); const QPoint wheelPoint=mapViewport->rect().center();
        QWheelEvent wheelUp(QPointF(wheelPoint),QPointF(mapViewport->mapToGlobal(wheelPoint)),QPoint(),QPoint(0,120),Qt::NoButton,Qt::ControlModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(mapViewport,&wheelUp); pump(30);
        require(zoom->value()==ctrlZoomBefore+10&&electrodeView->transform().m11()>scaleBefore,"Ctrl+wheel up changes zoom by 10 and scales actual electrode geometry");
        QWheelEvent wheelDown(QPointF(wheelPoint),QPointF(mapViewport->mapToGlobal(wheelPoint)),QPoint(),QPoint(0,-120),Qt::NoButton,Qt::ControlModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(mapViewport,&wheelDown); pump(30);
        require(zoom->value()==ctrlZoomBefore&&std::abs(electrodeView->transform().m11()-scaleBefore)<1e-9,"Ctrl+wheel down restores exact zoom and geometry scale");
        // Exercise every editable physical register field through the map UI.
        // Expected words are written independently from the documented bit map.
        button(channels,"清空")->click(); edit(global,"0,1"); button(channels,"应用选择")->click(); pump(120);
        const auto setCode=[&](QWidget *page,const QString &name,int value) { auto *c=field<QComboBox>(page,name); const int i=c->findData(value); require(i>=0,"register field accepts documented code: "+name); c->setCurrentIndex(i); };
        cfgTabs->setCurrentIndex(0);
        for(const auto &v:QVector<QPair<QString,int>>{{"增益",0},{"高通",0},{"参考",1},{"低通",1},{"高功耗",1},{"阻抗微调",7},{"通道关断",1},{"RST_N",1}}) setCode(cfgTabs->widget(0),v.first,v.second);
        writeCurrent->click(); require(until([&]{return writeCurrent->isEnabled()&&dummy.protocolState().rec[0]==0x0f93&&dummy.protocolState().rec[1]==0x0f93;}),"all REC field bits reach exact simulated 0x0F93 register");
        cfgTabs->setCurrentIndex(1); field<QSpinBox>(cfgTabs->widget(1),"幅度码")->setValue(341);
        for(const auto &v:QVector<QPair<QString,int>>{{"极性",2},{"步进",1},{"补偿",1},{"刺激输出",0},{"DAC 电极",3}}) setCode(cfgTabs->widget(1),v.first,v.second);
        writeCurrent->click(); require(until([&]{return writeCurrent->isEnabled()&&dummy.protocolState().dac[0]==0x7d55&&dummy.protocolState().dac[1]==0x7d55;}),"all DAC field bits reach exact simulated 0x7D55 register");
        cfgTabs->setCurrentIndex(2);
        for(const auto &v:QVector<QPair<QString,int>>{{"正相脉宽",7},{"负相脉宽",8},{"全局频率",5},{"局部分频",3},{"相序",1},{"电流×20",1},{"时序开关",1}}) setCode(cfgTabs->widget(2),v.first,v.second);
        writeCurrent->click(); require(until([&]{return writeCurrent->isEnabled()&&dummy.protocolState().dacCt[0]==0xfd78&&dummy.protocolState().dacCt[1]==0xfd78;}),"all CT field bits reach exact simulated 0xFD78 register");
        // Quick-command editor is modal: Save, Cancel and Restore Defaults all
        // run through the actual dialog buttons and persistent config.
        auto editQuick=[&](int mode) {
            bool handled=false; QTimer timer;
            QObject::connect(&timer,&QTimer::timeout,[&]{
                if(handled)return;
                for(auto *top:QApplication::topLevelWidgets()) if(auto *d=qobject_cast<QDialog *>(top);d&&d->isVisible()&&d->windowTitle()=="编辑快速命令") {
                    handled=true; auto *editor=d->findChild<QPlainTextEdit *>(); auto *box=d->findChild<QDialogButtonBox *>();
                    if(mode==0) editor->setPlainText("QA REC3 = "+QString::number(0x10030061u,2).rightJustified(32,'0')+","+QString::number(0x10020062u,2).rightJustified(32,'0')+"\nQA alias = @gain_all_high");
                    else button(d,"恢复默认")->click();
                    box->button(mode==1?QDialogButtonBox::Cancel:QDialogButtonBox::Save)->click();
                }
            }); timer.start(20); button(spi,"编辑快速命令")->click(); timer.stop(); require(handled,"quick-command editor dialog exercised");
        };
        editQuick(0); require(cfg.load().value("SpiQuickCommands").value("name_0").contains("QA REC3"),"quick-command Save persists custom button");
        button(spi,"QA REC3")->click(); require(until([&]{return writeCurrent->isEnabled()&&dummy.protocolState().rec[3]==0x61&&dummy.protocolState().rec[2]==0x62;}),"saved multi-command quick button executes both independent writes");
        button(spi,"QA alias")->click(); require(until([&]{const auto p=dummy.protocolState();return writeCurrent->isEnabled()&&std::all_of(p.rec.begin(),p.rec.end(),[](quint16 v){return v==0x5e;});},15000),"saved quick-command alias updates all 256 simulated REC registers");
        editQuick(1); require(button(spi,"QA REC3")!=nullptr,"reset then Cancel retains existing quick commands");
        editQuick(2); require(!cfg.load().value("SpiQuickCommands").value("name_0").contains("QA REC3"),"reset then Save restores default quick commands");
        bool invalidSubmitted=false, invalidWarning=false, invalidCancelled=false; QTimer invalidTimer;
        const auto beforeInvalid=cfg.load().value("SpiQuickCommands");
        QObject::connect(&invalidTimer,&QTimer::timeout,[&]{
            for(auto *top:QApplication::topLevelWidgets()) if(auto *m=qobject_cast<QMessageBox *>(top);m&&m->isVisible()) { invalidWarning=true; m->accept(); return; }
            for(auto *top:QApplication::topLevelWidgets()) if(auto *d=qobject_cast<QDialog *>(top);d&&d->isVisible()&&d->windowTitle()=="编辑快速命令") {
                auto *box=d->findChild<QDialogButtonBox *>();
                if(invalidWarning) { invalidCancelled=true; box->button(QDialogButtonBox::Cancel)->click(); }
                else if(!invalidSubmitted) { invalidSubmitted=true; d->findChild<QPlainTextEdit *>()->setPlainText("invalid = not-binary"); box->button(QDialogButtonBox::Save)->click(); }
            }
        }); invalidTimer.start(20); button(spi,"编辑快速命令")->click(); invalidTimer.stop();
        require(invalidSubmitted&&invalidWarning&&invalidCancelled&&cfg.load().value("SpiQuickCommands")==beforeInvalid,"malformed quick command is rejected and Cancel preserves config");
        button(channels,"清空")->click(); edit(global,"0,2"); button(channels,"应用选择")->click();
        if(app.arguments().contains("--map-only")) { std::cout << "dummy_processing_ui_e2e map-only ok checks=" << checks << std::endl; return 0; }
        button(toolbar, "回放")->click();
        fileAction(button(toolbar, "打开文件…"), path, true);
        require(hub->state() == ccv2::SessionHub::State::Idle, "cancel replay chooser leaves Idle");
        fileAction(button(toolbar, "打开文件…"), path);
        require(until([&]{return hub->isReplaying();}), "GUI loader connects synthetic source");
        auto *play = named<QPushButton>(toolbar, "transportPlay");
        auto *home = button(toolbar, "跳到开头", true);
        auto run = [&]() {
            if (hub->state() == ccv2::SessionHub::State::ReplayPlaying) play->click();
            home->click(); pump(40); play->click();
            require(until([&]{ return hub->currentFrameIndex() == kFrames; }, 10000), "replay reaches all synthetic frames");
            pump(250);
        };
        // REALTIME: rendered plots and metrics must respond to the real stream.
        tab(window, "实时波形"); run();
        auto *map = rt->findChild<ccv2::ActivityMapView *>();
        require(map && map->maxRms() > .010 && map->maxRms() < .014, "realtime RMS matches 40-code sine (~12.4 mV)");
        auto *viewMode = combo(rt, "Origin");
        viewMode->setCurrentText("Origin"); run(); pump(120);
        auto waves = rt->findChildren<ccv2::WaveformWidget *>();
        require(!waves.isEmpty() && waves.first()->isVisible(), "Origin exposes waveform widget");
        auto *signal = combo(rt, "DC");
        signal->setCurrentText("DC"); pump(100); const auto dc = waves.first()->grab().toImage();
        signal->setCurrentText("AC"); pump(100); const auto ac = waves.first()->grab().toImage();
        require(dc != ac, "DC versus AC changes rendered signal/axis");
        signal->setCurrentText("Highpass"); pump(100);
        require(ac != waves.first()->grab().toImage(), "300 Hz highpass changes rendered signal/axis");
        viewMode->setCurrentText("Stack"); run(); pump(100);
        auto stacks = rt->findChildren<ccv2::StackWaveformView *>();
        require(!stacks.isEmpty() && stacks.first()->isVisible(), "Stack mode exposes stacked trace view");
        auto *scale = combo(rt, "固定Y"); scale->setCurrentText("自适应Y"); pump(100);
        const auto adaptive = stacks.first()->grab().toImage(); scale->setCurrentText("固定Y"); pump(100);
        require(adaptive != stacks.first()->grab().toImage(), "adaptive/fixed stack scaling has rendered effect");
        field<QSpinBox>(rt, "波形点数")->setValue(512); button(rt, "应用X轴")->click();
        field<QSpinBox>(rt, "刷新")->setValue(20);
        edit(field<QLineEdit>(rt, "通道"), "0,1,2,3"); button(rt, "应用通道")->click(); run();
        auto *metric = combo(rt, "AC RMS");
        QVector<QImage> metricImages;
        for (int i=0; i<metric->count(); ++i) { metric->setCurrentIndex(i); pump(30); const auto image = map->grab().toImage(); if (!metricImages.contains(image)) metricImages.append(image); }
        require(metricImages.size() == metric->count(), "all realtime heatmap metric controls render");
        clickAt(map,heatCell(map,3,5)); pump(50);
        const auto neighborhood=field<QLineEdit>(rt,"通道")->text().split(',');
        require(neighborhood.size()==32&&neighborhood.first()=="41"&&neighborhood.last()=="72","realtime heatmap ADC57 click selects exact 32-channel neighborhood");
        button(rt,"从通道图载入")->click(); require(field<QLineEdit>(rt,"通道")->text()=="0,2","realtime loads exact selected channel-map IDs"); run();
        auto *ref = combo(rt, "共平均 (CAR)");
        for (int i=0; i<ref->count(); ++i) { ref->setCurrentIndex(i); run(); require(hub->referenceMode() == ref->currentData().toInt(), "software reference reaches shared pipeline"); }
        ref->setCurrentIndex(0);
        // TDM controls are global; both phase sets must change lane identity.
        auto *tdm = check(rt, "TDM分组显示"); auto *phase = combo(rt, "显示相位 0 / 2");
        tdm->setChecked(true); phase->setCurrentIndex(0); run();
        require(hub->tdmContext()->enabled() && sp->analysisStore()->channels() == 512, "TDM creates 512 analysis lanes");
        require(check(an,"TDM分组显示")->isChecked() && check(sw,"TDM分组显示")->isChecked(), "TDM mirrored across analysis and sweep");
        require(sp->analysisStore()->snapshotLane(0).observedSamples == kFrames/4, "TDM lane exposure is source/4");
        phase->setCurrentIndex(1); run();
        const auto tdmEvents = sp->analysisStore()->snapshotLane(0).events;
        require(!hub->tdmContext()->pair02() && !tdmEvents.isEmpty() && tdmEvents.first().tdmPhase == 1 && tdmEvents.first().sampleStride == 4, "TDM phase B event provenance/stride");
        tdm->setChecked(false); require(sp->analysisStore()->channels() == 256, "TDM off restores ADC lane count");
        // ANALYZER: independent analytic values from the 1 kHz, 40-code sine.
        tab(window, "分析器"); run();
        QTableWidget *timeTable=nullptr, *fftTable=nullptr;
        for (auto *t : an->findChildren<QTableWidget *>()) { if(t->columnCount()==2) timeTable=t; if(t->columnCount()==3) fftTable=t; }
        require(timeTable && fftTable, "visible analyzer statistics tables present");
        require(tableText(timeTable,"输出路数") == "2", "analyzer reports two source channels");
        require(std::abs(volts(tableText(timeTable,"Mean(avg)"))-.9)<.001, "analyzer DC mean equals 2048-code baseline");
        require(std::abs(volts(tableText(timeTable,"AC RMS(avg)"))-(40*1.8/4096/std::sqrt(2.0)))<.0004, "analyzer AC RMS agrees with analytic sine");
        require(std::abs(number(tableText(fftTable,"Fin"))-1000)<15, "FFT dominant frequency recovers 1 kHz");
        auto *anChannels=field<QLineEdit>(an,"通道"); edit(anChannels,"1;3"); run();
        require(tableText(timeTable,"输出路数")=="2"&&field<QComboBox>(an,"统计输出")->findText("CH1")>=0&&field<QComboBox>(an,"统计输出")->findText("CH3")>=0,"analyzer manual channel groups change actual outputs and source choices");
        button(an,"从通道图载入")->click(); run(); require(anChannels->text()=="0,2"&&std::abs(number(tableText(fftTable,"Fin"))-1000)<15,"analyzer map-load restores known tone channels");
        for(auto *plot:an->findChildren<ccv2::WaveformWidget *>()) {
            const auto before=plot->grab().toImage();
            const QPoint a(plot->width()/3,plot->height()/3), b(plot->width()*2/3,plot->height()*2/3);
            mouse(plot,QEvent::MouseButtonPress,a,Qt::RightButton,Qt::RightButton); mouse(plot,QEvent::MouseButtonRelease,b,Qt::RightButton,Qt::NoButton); pump(40);
            require(before!=plot->grab().toImage(),"analyzer waveform/spectrum box zoom changes displayed range");
            mouse(plot,QEvent::MouseButtonDblClick,a,Qt::RightButton,Qt::RightButton); pump(40);
            require(before==plot->grab().toImage(),"analyzer waveform/spectrum double-right-click resets exact view");
        }
        auto *anTdm=check(an,"TDM分组显示"); auto *anPhase=combo(an,"显示相位 0 / 2");
        anTdm->setChecked(true);
        for(int p=0;p<2;++p) { anPhase->setCurrentIndex(p); run(); require(tableText(timeTable,"输出路数")=="4"&&std::abs(number(tableText(fftTable,"Fin"))-1000)<5,"analyzer TDM phase produces four correctly timed outputs"); }
        anTdm->setChecked(false); run();
        auto *statsSource=field<QComboBox>(an,"统计输出"); statsSource->setCurrentIndex(1); pump(150);
        require(statsSource->currentText()=="CH2"&&std::abs(number(tableText(fftTable,"Fin"))-1000)<15,"analyzer statistics source switches to CH2");
        auto *win=combo(an,"Hann");
        for(int i=0;i<win->count();++i) { win->setCurrentIndex(i); pump(200); require(std::abs(number(tableText(fftTable,"Fin"))-1000)<20, "FFT window preserves known frequency: "+win->currentText()); }
        auto *fftN=field<QComboBox>(an,"FFT 点数");
        const int longFrames=262144+4096; const QString longPath=temp.filePath("long-known-tone.bin"); writeLongTone(longPath,longFrames);
        named<QPushButton>(toolbar,"fileChipClose")->click(); fileAction(button(toolbar,"打开文件…"),longPath);
        for(int i=0;i<fftN->count();++i) {
            const int n=fftN->itemData(i).toInt(); fftN->setCurrentIndex(i); home->click(); pump(30); play->click();
            require(until([&]{return hub->currentFrameIndex()>=n+512;},40000),"long replay fills requested FFT window: "+QString::number(n));
            if(hub->state()==ccv2::SessionHub::State::ReplayPlaying)play->click();
            require(until([&]{bool ok=false;const double fin=tableText(fftTable,"Fin").section(' ',0,0).toDouble(&ok);return ok&&std::abs(fin-1000)<=kFs/n+.02;},6000),"every offered FFT size recovers known 1 kHz tone: "+QString::number(n));
        }
        named<QPushButton>(toolbar,"fileChipClose")->click(); fftN->setCurrentIndex(fftN->findData(4096)); fileAction(button(toolbar,"打开文件…"),path); run();
        field<QSpinBox>(an,"DC Bin")->setValue(2); field<QSpinBox>(an,"Sig Bin ±")->setValue(3);
        field<QDoubleSpinBox>(an,"SNDR BW")->setValue(8000); pump(250);
        const double irn60=number(tableText(fftTable,"IRN"));
        field<QDoubleSpinBox>(an,"IRN Gain")->setValue(120); pump(250);
        const double irn120=number(tableText(fftTable,"IRN"));
        require(irn60>0 && std::abs(irn120/irn60-.5)<.05, "input noise halves when IRN gain doubles");
        auto *yMode=combo(an,"功率谱 dB"); yMode->setCurrentIndex(1); pump(150);
        require(yMode->currentData().toString()=="density", "FFT density display selected"); yMode->setCurrentIndex(0);
        button(an,"重置最优值")->click(); pump(200); require(std::isfinite(number(tableText(fftTable,"SNDR"))), "reset-best leaves valid recomputed FFT statistics");
        auto *fftEnable=check(an,"显示/计算 FFT"); fftEnable->setChecked(false); pump(30);
        require(!fftN->isEnabled() && !fftTable->isVisible(), "FFT off disables controls and hides statistics"); fftEnable->setChecked(true);
        auto *overview=button(an,"256通道概览"); overview->click(); run(); pump(2100);
        require(tableText(timeTable,"输出路数")=="256" && !fftEnable->isEnabled() && !fftEnable->isChecked(), "256-channel overview processes array and locks FFT off");
        overview->click(); run(); require(tableText(timeTable,"输出路数")=="2" && fftEnable->isEnabled(), "overview exit restores ordinary two-channel analysis");
        // SWEEP: each compiled band/filter and threshold setting sees data.
        tab(window, "实时spike"); auto *sweep=sw->findChild<ccv2::SweepWaveformView *>();
        edit(field<QLineEdit>(sw,"通道"),"0-3");
        field<QDoubleSpinBox>(sw,"时间窗")->setValue(.5);
        field<QDoubleSpinBox>(sw,"Y满量程")->setValue(.05);
        auto *swMap=sw->findChild<ccv2::ActivityMapView *>(); auto *clickMode=combo(sw,"窗口");
        clickMode->setCurrentText("窗口"); clickAt(swMap,heatCell(swMap,3,5));
        require(sweep->channels()==QVector<int>({55,56,57,58}),"sweep heatmap window click selects exact four-channel neighborhood");
        clickMode->setCurrentText("单通道"); clickAt(swMap,heatCell(swMap,3,5));
        require(sweep->channels()==QVector<int>({55,56,58}),"sweep single-channel click toggles only clicked ADC out");
        clickAt(swMap,heatCell(swMap,3,5)); require(sweep->channels()==QVector<int>({55,56,57,58}),"sweep single-channel click adds ADC back without replacing other lanes");
        auto *swMetric=combo(sw,"RMS"); QVector<QImage> swMetricImages;
        for(int i=0;i<swMetric->count();++i){swMetric->setCurrentIndex(i);pump(30);const auto image=swMap->grab().toImage();if(!swMetricImages.contains(image))swMetricImages.append(image);}
        require(swMetricImages.size()==3,"sweep RMS/P2P/mean metric choices render distinct labeled states");
        edit(field<QLineEdit>(sw,"通道"),"0-3");
        auto *band=combo(sw,"宽带");
        for(int i=0;i<band->count();++i) { band->setCurrentIndex(i); run(); require(sweep && sweep->channels()==QVector<int>({0,1,2,3}), "sweep band renders selected channels: "+band->currentText()); }
        auto *swTdm=check(sw,"TDM分组显示"); auto *swPhase=combo(sw,"显示相位 0 / 2"); swTdm->setChecked(true);
        for(int p=0;p<2;++p) { swPhase->setCurrentIndex(p); run(); QVector<int> expected; for(int c=0;c<4;++c){expected<<c*4+p<<c*4+p+2;} require(sweep->channels()==expected,"sweep TDM physical electrode identities for phase "+QString::number(p)); }
        swTdm->setChecked(false);
        auto *notch=combo(sw,"50 Hz"); for(int i=0;i<notch->count();++i) { notch->setCurrentIndex(i); run(); require(sw->findChild<ccv2::ActivityMapView *>()->maxRms()>0, "sweep notch path retains nonzero metrics: "+notch->currentText()); }
        field<QSpinBox>(sw,"高通")->setValue(300); field<QSpinBox>(sw,"Spike低通")->setValue(5000);
        auto *order=field<QComboBox>(sw,"滤波阶数"); for(int i=0;i<order->count();++i) { order->setCurrentIndex(i); run(); require(!sweep->grab().isNull(), "sweep filter order renders: "+order->currentText()); }
        field<QComboBox>(sw,"阈值模式")->setCurrentIndex(1); field<QDoubleSpinBox>(sw,"阈值")->setValue(.1);
        field<QComboBox>(sw,"极性")->setCurrentIndex(1); run();
        home->click(); play->click(); pump(200); play->click(); pump(150);
        const auto pausedFrame=hub->currentFrameIndex(); const auto frozen=sweep->grab().toImage(); pump(200);
        require(pausedFrame==hub->currentFrameIndex()&&frozen==sweep->grab().toImage(), "global replay pause freezes sweep source and waveform");
        play->click(); require(until([&]{return hub->currentFrameIndex()==kFrames;}), "global replay resume advances sweep source");
        field<QComboBox>(sw,"阈值模式")->setCurrentIndex(0); field<QComboBox>(sw,"极性")->setCurrentIndex(0);
        // SPIKE: detector's real public read-only store, retained data, and labels.
        tab(window,"Spike留存"); run();
        auto snap=sp->analysisStore()->snapshotLane(1);
        require(snap.observedSamples==kFrames, "detector exposure covers all 16000 source samples");
        require(snap.totalDetected>=30 && snap.totalDetected<=34, "detector recovers 32 injected negative spikes");
        require(snap.events.size()==snap.totalDetected && snap.waveforms.size()==snap.events.size()*snap.snippetLength, "event identities and waveform rows paired");
        for(const auto &e:snap.events) require(e.sourceFrame%500>=150 && e.sourceFrame%500<=175 && e.adcChannel==1 && e.inputGain==60, "detected spike timing and source provenance");
        auto *spPause=named<QPushButton>(sp,"spikePauseDisplay"); spPause->click();
        tab(window,"实时波形"); run();
        require(sp->analysisStore()->snapshotLane(1).observedSamples==kFrames && sp->analysisStore()->totalSpikes(1)>=30, "hidden and display-paused Spike page keeps detecting");
        tab(window,"Spike留存"); spPause->click();
        // Select ADC 1 through actual grid mouse input (16 columns fixed for QA).
        for(auto *v:sp->findChildren<QSpinBox *>()) if(v->maximum()==64) v->setValue(16);
        auto *grid=sp->findChild<ccv2::SpikeGridView *>(); pump(100);
        clickAt(grid,QPoint(4+3*((grid->width()-8)/16)/2, 4+((grid->height()-8)/16)/2));
        require(grid->selectedLane()==1, "array mouse click selects ADC 1");
        auto *detail=sp->findChild<ccv2::SpikeDetailWindow *>(); require(detail && detail->isVisible() && detail->lane()==1, "single-lane inspector opens for selection");
        check(detail,"独立阈值")->setChecked(true);
        auto detailSpins=detail->findChildren<QDoubleSpinBox *>(); require(detailSpins.size()>=2,"detail threshold and scale controls exist");
        detailSpins.first()->setValue(60); run();
        require(std::abs(std::abs(sp->analysisStore()->threshold(1))-60e-6)<1e-8,"per-lane threshold override reaches detector");
        auto *detailGrid=detail->findChild<ccv2::SpikeGridView *>();
        const double oldThreshold=detailSpins.first()->value();
        clickAt(detailGrid,QPoint(detailGrid->width()/2,detailGrid->height()*3/4)); run();
        require(detailSpins.first()->value()!=oldThreshold&&detailSpins.first()->value()>10&&std::abs(std::abs(sp->analysisStore()->threshold(1))-detailSpins.first()->value()*1e-6)<1e-8,"detail threshold drag updates real detector override");
        check(detail,"独立阈值")->setChecked(false); run();
        require(std::abs(std::abs(sp->analysisStore()->threshold(1))-30e-6)<1e-8,"override removal restores global threshold");
        detailSpins.last()->setValue(350); detail->findChild<QSpinBox *>()->setValue(40);
        check(detail,"自动量程")->setChecked(true); check(detail,"旧波形淡出")->setChecked(false);
        require(until([&]{const auto section=cfg.load().value("SpikePanel");return section.value("detail_y_scale_uv").toDouble()==350&&section.value("detail_overlay_count").toInt()==40&&section.value("detail_auto_scale")=="1"&&section.value("detail_fade")=="0";}),"inspector scale/overlay/auto/fade persist exact display settings");
        detail->close(); clickAt(grid,QPoint(4+3*((grid->width()-8)/16)/2,4+((grid->height()-8)/16)/2));
        require(detail->isVisible() && sp->findChildren<ccv2::SpikeDetailWindow *>().size()==1,"inspector close/reopen reuses one window");
        require(detailSpins.last()->value()==350&&detail->findChild<QSpinBox *>()->value()==40&&check(detail,"自动量程")->isChecked()&&!check(detail,"旧波形淡出")->isChecked(),"inspector reopen retains exact display options"); detail->hide();
        auto *analysisDetails=named<QWidget>(sp,"spikeAnalysisDetailsPanel"); named<QPushButton>(sp,"spikeAnalysisDetails")->click(); require(analysisDetails->isVisible(),"Spike analysis details expand"); named<QPushButton>(sp,"spikeAnalysisDetails")->click(); require(!analysisDetails->isVisible(),"Spike analysis details collapse");
        require(named<QLabel>(sp,"spikeCoverageSummary")->text().contains("来源有效性未验证"),"Spike quality summary discloses bare-source validity uncertainty");
        named<QPushButton>(sp,"spikeSortingWorkspace")->click(); pump(80);
        auto *sort=sp->findChild<ccv2::SpikeSortingWidget *>(); require(sort&&sort->isVisible(),"candidate workspace opened from real page");
        named<QSpinBox>(sort,"candidateLane")->setValue(1);
        named<QCheckBox>(sort,"candidateFreeze")->setChecked(true); named<QPushButton>(sort,"candidateRefresh")->click(); pump(100);
        home->click(); named<QPushButton>(sort,"candidateRefresh")->click(); pump(60);
        const QString frozenCoverage=named<QLabel>(sort,"candidateCoverage")->text(); play->click();
        require(until([&]{return hub->currentFrameIndex()==kFrames&&sp->analysisStore()->snapshotLane(1).observedSamples==kFrames;}),"same-epoch source advances while candidate snapshot is frozen"); pump(250);
        require(named<QLabel>(sort,"candidateCoverage")->text()==frozenCoverage&&sp->analysisStore()->totalSpikes(1)>=30,"candidate freeze holds snapshot while real detector advances");
        named<QPushButton>(sort,"candidateRefresh")->click();pump(100);require(named<QLabel>(sort,"candidateCoverage")->text()!=frozenCoverage,"manual refresh replaces frozen snapshot with current events");
        home->click(); named<QCheckBox>(sort,"candidateFreeze")->setChecked(false);
        require(until([&]{return named<QLabel>(sort,"candidateCoverage")->text().contains("已分析 0 样本");}),"unfreeze refreshes the new empty timeline automatically");
        play->click(); require(until([&]{return hub->currentFrameIndex()==kFrames&&named<QLabel>(sort,"candidateCoverage")->text().contains("已分析 16000 样本");},7000),"unfrozen candidate view automatically follows arriving detector samples");
        named<QCheckBox>(sort,"candidateFreeze")->setChecked(true);
        auto *qualityDetails=named<QWidget>(sort,"candidateQualityDetailsPanel"); named<QPushButton>(sort,"candidateQualityDetails")->click(); require(qualityDetails->isVisible(),"candidate quality details expand"); named<QPushButton>(sort,"candidateQualityDetails")->click(); require(!qualityDetails->isVisible(),"candidate quality details collapse");
        require(named<QLabel>(sort,"candidateQualitySummary")->text().contains("来源有效性未验证"),"candidate quality summary preserves source validity uncertainty");
        auto *feature=named<QWidget>(sort,"candidateFeaturePlot");
        // Find a rendered unassigned teal point, then use ordinary mouse input.
        const QImage featureImage=feature->grab().toImage(); QPoint dot(-1,-1);
        for(int y=38;y<featureImage.height()-53&&dot.x()<0;++y) for(int x=70;x<featureImage.width()-25;++x) {const QColor c=featureImage.pixelColor(x,y);if(c.green()>c.red()+30&&c.blue()>c.red()+30&&c.green()>100){dot=QPoint(x,y);break;}}
        require(dot.x()>=0,"candidate feature scatter contains a rendered event point"); clickAt(feature,dot);
        named<QSpinBox>(sort,"candidateUnit")->setValue(7); named<QPushButton>(sort,"candidateAssign")->click();
        auto pointSnap=sp->analysisStore()->snapshotLane(1); require(std::count_if(pointSnap.events.cbegin(),pointSnap.events.cend(),[](const auto&e){return e.unitId==7;})==1,"point click labels exactly one retained event");
        // Shift-clicking empty plot space appends nothing and preserves selection.
        const QPoint empty(65,32); mouse(feature,QEvent::MouseButtonPress,empty,Qt::LeftButton,Qt::LeftButton,Qt::ShiftModifier); mouse(feature,QEvent::MouseButtonRelease,empty,Qt::LeftButton,Qt::NoButton,Qt::ShiftModifier);
        require(named<QPushButton>(sort,"candidateAssign")->isEnabled(),"Shift-add empty selection preserves previously selected event");
        named<QSpinBox>(sort,"candidateUnit")->setValue(8); named<QPushButton>(sort,"candidateAssign")->click();
        pointSnap=sp->analysisStore()->snapshotLane(1); require(std::count_if(pointSnap.events.cbegin(),pointSnap.events.cend(),[](const auto&e){return e.unitId==8;})==1,"Shift-add preserves exact event identity for relabeling");
        named<QPushButton>(sort,"candidateClearSelection")->click(); pump(60);
        mouse(feature,QEvent::MouseButtonPress,QPoint(65,32),Qt::LeftButton,Qt::LeftButton);
        mouse(feature,QEvent::MouseButtonRelease,QPoint(feature->width()-21,feature->height()-48),Qt::LeftButton,Qt::NoButton); pump(30);
        require(named<QPushButton>(sort,"candidateAssign")->isEnabled()&&named<QCheckBox>(sort,"candidateFreeze")->isChecked(),"real scatter rectangle selects retained events and freezes snapshot");
        named<QPushButton>(sort,"candidateClearSelection")->click(); require(!named<QPushButton>(sort,"candidateAssign")->isEnabled(),"candidate clear-selection disables assignment");
        named<QPushButton>(sort,"candidateSelectAll")->click(); named<QSpinBox>(sort,"candidateUnit")->setValue(3); named<QPushButton>(sort,"candidateAssign")->click();
        snap=sp->analysisStore()->snapshotLane(1);
        require(!snap.events.isEmpty() && std::all_of(snap.events.cbegin(),snap.events.cend(),[](const auto&e){return e.unitId==3;}),"manual candidate label changes retained real events");
        auto *filter=named<QComboBox>(sort,"candidateFilter"); filter->setCurrentIndex(filter->findData(3)); pump(40);
        named<QPushButton>(sort,"candidateSelectAll")->click(); require(named<QPushButton>(sort,"candidateUnassign")->isEnabled(),"candidate filter exposes matching selected events");
        filter->setCurrentIndex(filter->findData(4)); named<QPushButton>(sort,"candidateSelectAll")->click(); require(!named<QPushButton>(sort,"candidateAssign")->isEnabled(),"empty candidate filter cannot label hidden events");
        filter->setCurrentIndex(filter->findData(3));
        for(auto *c:sort->findChildren<QComboBox *>()) if(c!=filter) for(int i=0;i<c->count();++i) { c->setCurrentIndex(i); pump(10); }
        sort->close(); named<QPushButton>(sp,"spikeSortingWorkspace")->click();
        require(sort->isVisible() && sp->findChildren<ccv2::SpikeSortingWidget *>().size()==1,"candidate workspace close/reopen reuses one window");
        const QString exportPath=temp.filePath("retained-lane.json");
        fileAction(named<QPushButton>(sp,"spikeExportLane"),exportPath,true);
        require(!QFile::exists(exportPath),"cancel export writes no file");
        fileAction(named<QPushButton>(sp,"spikeExportLane"),exportPath);
        QFile exported(exportPath); require(exported.open(QIODevice::ReadOnly),"GUI export creates readable JSON");
        const auto json=QJsonDocument::fromJson(exported.readAll()).object(); const auto events=json["events"].toArray();
        require(json["scope"].toString()=="retained_selected_lane_window_not_complete_recording" && json["lane"].toInt()==1 && events.size()==snap.events.size(),"export scope, lane, retained count are correct");
        require(json["quality"].toObject()["unverified_frames"].toString()==QString::number(kFrames)&&json["quality"].toObject()["incomplete_coverage"].toBool(),"bare replay export explicitly marks unverified source validity");
        require(json["unit_label_semantics"].toString()=="manual_or_waveform_box_candidate_labels_not_validated_neuron_isolation","export identifies manual labels without biological-validity claims");
        require(!events.isEmpty() && events.first().toObject()["candidate_unit_id"].toInt()==3 && events.first().toObject()["waveform_input_volts"].toArray().size()==snap.snippetLength,"export preserves candidate label and paired waveform");
        require(events.first().toObject()["classification_status"].toInt()==int(ccv2::SpikeClassificationStatus::Manual),"export distinguishes manually assigned candidate classification from automated rules");
        named<QPushButton>(sort,"candidateSelectAll")->click(); named<QPushButton>(sort,"candidateUnassign")->click();
        snap = sp->analysisStore()->snapshotLane(1);
        require(std::all_of(snap.events.cbegin(),snap.events.cend(),[](const auto&e){return e.unitId==-1;}),"candidate unassign clears labels");
        sort->hide(); button(sp,"清空留存")->click(); pump(30);
        require(sp->analysisStore()->totalSpikes(1)==0,"clear retained restarts coherent empty detector interval"); run();
        require(sp->analysisStore()->totalSpikes(1)>=30,"detector repopulates after clear/reset");
        const auto peak=[](const ccv2::SpikeLaneSnapshot &s) { double result=0; for(float v:s.waveforms) result=std::max(result,std::abs(double(v))); return result; };
        const double baselinePeak=peak(sp->analysisStore()->snapshotLane(1));
        auto *gain=combo(sp,"60×"); gain->setCurrentText("180×"); run();
        auto gainSnap=sp->analysisStore()->snapshotLane(1);
        require(!gainSnap.events.isEmpty()&&gainSnap.events.first().inputGain==180&&std::abs(peak(gainSnap)/baselinePeak-1.0/3.0)<.03,"Spike gain 180 divides input-referred waveform by three");
        gain->setCurrentText("60×");
        suffix<QDoubleSpinBox>(sp," ms前")->setValue(.6); suffix<QDoubleSpinBox>(sp," ms后")->setValue(1.5);
        suffix<QSpinBox>(sp," 条/通道")->setValue(20); run();
        auto bounded=sp->analysisStore()->snapshotLane(1);
        require(bounded.snippetLength==43&&bounded.events.size()==20&&bounded.totalDetected>=30,"pre/post window and bounded retention are enforced");
        suffix<QDoubleSpinBox>(sp," ms前")->setValue(.4); suffix<QDoubleSpinBox>(sp," ms后")->setValue(1.2); suffix<QSpinBox>(sp," 条/通道")->setValue(200);
        auto *thresholdMode=combo(sp,"RMS 倍数"); thresholdMode->setCurrentIndex(0); run();
        auto rmsSnap=sp->analysisStore()->snapshotLane(1);
        require(rmsSnap.noiseRmsV>0&&std::abs(std::abs(rmsSnap.thresholdV)/rmsSnap.noiseRmsV-4)<.05,"RMS threshold mode uses four times measured noise");
        thresholdMode->setCurrentIndex(1); suffix<QDoubleSpinBox>(sp," µV")->setValue(30);
        auto *spNotch=combo(sp,"陷波 关");
        for(int i=0;i<spNotch->count();++i) { spNotch->setCurrentIndex(i); run(); require(sp->analysisStore()->totalSpikes(1)>0,"Spike notch path detects real input: "+spNotch->currentText()); }
        spNotch->setCurrentIndex(0); suffix<QSpinBox>(sp," Hz")->setValue(300);
        auto *polarity=combo(sp,"负向"); polarity->setCurrentText("正向"); run();
        require(sp->analysisStore()->threshold(1)>0&&sp->analysisStore()->snapshotLane(1).observedSamples==kFrames,"positive polarity uses positive detector threshold");
        polarity->setCurrentText("负向"); suffix<QSpinBox>(sp," Hz")->setValue(250);
        check(sp,"自动量程")->setChecked(true); check(sp,"旧波形淡出")->setChecked(false);
        run(); require(!grid->grab().isNull(),"auto-scaled non-fading Spike array renders retained events");
        check(sp,"自动量程")->setChecked(false); check(sp,"旧波形淡出")->setChecked(true);
        named<QPushButton>(toolbar,"fileChipClose")->click(); pump(100);
        require(!hub->isRunning() && !sp->analysisRunning(),"unload stops source and continuous detector");
        fileAction(button(toolbar,"打开文件…"),path); run();
        require(sp->analysisStore()->totalSpikes(1)>=30,"reopen/reconnect replay repopulates detector");
        window.close(); pump(50);
        std::cout<<"dummy_processing_ui_e2e ok checks="<<checks<<std::endl;
        return 0;
    } catch(const std::exception &e) {
        std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<std::endl;
        return 1;
    }
}
