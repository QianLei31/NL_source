// P0 coverage: the global TdmContext notifies on real changes only, and the
// session manifest round-trips the TDM fields while treating pre-TDM manifests
// as "unknown" (so replay/export don't silently assume a phase).

#include "core/tdm_context.h"
#include "io/session_manifest.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>

using namespace ccv2;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // --- TdmContext: change signal fires only on an actual state change ---
    {
        TdmContext ctx;
        if (ctx.enabled() || !ctx.evenFirst()) {
            std::cerr << "TdmContext defaults wrong" << std::endl;
            return 1;
        }
        int changes = 0;
        QObject::connect(&ctx, &TdmContext::changed,
                         [&changes](bool, bool) { ++changes; });
        ctx.setEnabled(true);              // change -> 1
        ctx.setEnabled(true);              // no-op
        ctx.setEvenFirst(false);           // change -> 2
        ctx.setState(true, false);         // no-op
        ctx.setState(false, true);         // change -> 3
        if (changes != 3) {
            std::cerr << "TdmContext emitted " << changes
                      << " changes, expected 3" << std::endl;
            return 2;
        }
        if (ctx.enabled() || !ctx.evenFirst()) {
            std::cerr << "TdmContext final state wrong" << std::endl;
            return 3;
        }
    }

    // --- SessionManifest: TDM fields survive a write/read round-trip ---
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::cerr << "temp dir failed" << std::endl;
        return 4;
    }
    {
        SessionManifestData data;
        data.metadata.sampleRate = 20000.0;
        data.metadata.tdmKnown = true;
        data.metadata.tdmEnabled = true;
        data.metadata.tdmEvenFirst = false;
        data.parts = {{QStringLiteral("ADC_DATA_001.bin"), 4096, 16}};
        data.totalFrames = 16;
        data.complete = true;
        QString err;
        if (!SessionManifest::write(dir.path(), data, &err)) {
            std::cerr << "manifest write failed: " << err.toStdString() << std::endl;
            return 5;
        }
        SessionManifestData readBack;
        if (!SessionManifest::read(dir.path(), &readBack, &err)) {
            std::cerr << "manifest read failed: " << err.toStdString() << std::endl;
            return 6;
        }
        if (!readBack.metadata.tdmKnown || !readBack.metadata.tdmEnabled ||
            readBack.metadata.tdmEvenFirst) {
            std::cerr << "manifest TDM fields did not round-trip" << std::endl;
            return 7;
        }
    }

    // --- A manifest without TDM keys reads back as tdmKnown=false ---
    {
        const QString legacy = QDir(dir.path()).filePath(QStringLiteral("session.json"));
        QFile f(legacy);
        if (!f.open(QIODevice::ReadOnly)) {
            std::cerr << "reopen manifest failed" << std::endl;
            return 8;
        }
        QByteArray json = f.readAll();
        f.close();
        // Strip the TDM keys to emulate an older manifest.
        json.replace("\"tdm_enabled\": true,", "");
        json.replace("\"tdm_pair_02\": false,", "");
        json.replace("\"tdm_even_first\": false,", "");
        QFile w(legacy);
        if (!w.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            w.write(json) != json.size()) {
            std::cerr << "rewrite legacy manifest failed" << std::endl;
            return 9;
        }
        w.close();
        SessionManifestData readBack;
        QString err;
        if (!SessionManifest::read(dir.path(), &readBack, &err)) {
            std::cerr << "legacy manifest read failed: " << err.toStdString() << std::endl;
            return 10;
        }
        if (readBack.metadata.tdmKnown) {
            std::cerr << "legacy manifest should report tdmKnown=false" << std::endl;
            return 11;
        }
        if (!readBack.metadata.tdmEvenFirst) {
            std::cerr << "legacy manifest phase default should be evenFirst=true" << std::endl;
            return 12;
        }
    }

    std::cout << "tdm_context_smoke ok" << std::endl;
    return 0;
}
