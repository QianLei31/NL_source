#pragma once
#include <QString>
#include <QJsonObject>
#include <QJsonArray>

namespace ccv2 {

// Schema version
constexpr int kSchemaVersionV1 = 1;

// JSON field name constants for metadata.json
namespace MetaKey {
    inline constexpr const char* SchemaVersion   = "schema_version";
    inline constexpr const char* Software        = "software";
    inline constexpr const char* SessionId       = "session_id";
    inline constexpr const char* StartTime       = "start_time";
    inline constexpr const char* ChannelCount    = "channel_count";
    inline constexpr const char* Channels        = "channels";
    inline constexpr const char* ChannelMap      = "channel_map";
    inline constexpr const char* SamplingRate    = "sampling_rate";
    inline constexpr const char* AdcBits         = "adc_bits";
    inline constexpr const char* VRef            = "v_ref";
    inline constexpr const char* Unit            = "unit";
    inline constexpr const char* PacketLossRate  = "packet_loss_rate";
    inline constexpr const char* DurationS       = "duration_s";
    inline constexpr const char* FftPoints       = "fft_points";
    inline constexpr const char* WindowFunction  = "window_function";
    inline constexpr const char* Notes           = "notes";
    inline constexpr const char* ConfigFile      = "config_file";
}

// Channel map entry keys
namespace MapKey {
    inline constexpr const char* GlobalChannel   = "global_channel";
    inline constexpr const char* Block           = "block";
    inline constexpr const char* LocalChannel    = "local_channel";
    inline constexpr const char* ElectrodeId     = "electrode_id";
    inline constexpr const char* AdcId           = "adc_id";
    inline constexpr const char* StimId          = "stim_id";
    inline constexpr const char* SpiAddr         = "spi_addr";
    inline constexpr const char* DataLane        = "data_lane";
}

// Software name constant
inline const QString kSoftwareName = QStringLiteral("Neural Signal Command Center V6");

/// Validate a metadata JSON object against schema v1.
/// Returns true if valid; on failure sets *err to a human-readable message.
inline bool validateMetadataV1(const QJsonObject &obj, QString *err = nullptr)
{
    auto fail = [&](const QString &msg) -> bool {
        if (err) *err = msg;
        return false;
    };

    // Check schema_version
    if (!obj.contains(MetaKey::SchemaVersion))
        return fail(QStringLiteral("Missing field: schema_version"));
    if (obj[MetaKey::SchemaVersion].toInt(-1) != kSchemaVersionV1)
        return fail(QStringLiteral("Unsupported schema_version (expected 1)"));

    // Required string fields
    for (const char* key : {MetaKey::Software, MetaKey::SessionId, MetaKey::StartTime, MetaKey::Unit}) {
        if (!obj.contains(key) || !obj[key].isString())
            return fail(QStringLiteral("Missing or invalid string field: %1").arg(key));
    }

    // Required numeric fields
    for (const char* key : {MetaKey::ChannelCount, MetaKey::SamplingRate, MetaKey::AdcBits}) {
        if (!obj.contains(key) || !obj[key].isDouble())
            return fail(QStringLiteral("Missing or invalid numeric field: %1").arg(key));
    }

    // v_ref can be double
    if (!obj.contains(MetaKey::VRef) || !obj[MetaKey::VRef].isDouble())
        return fail(QStringLiteral("Missing or invalid numeric field: v_ref"));

    // packet_loss_rate
    if (!obj.contains(MetaKey::PacketLossRate) || !obj[MetaKey::PacketLossRate].isDouble())
        return fail(QStringLiteral("Missing or invalid numeric field: packet_loss_rate"));

    // channels array
    if (!obj.contains(MetaKey::Channels) || !obj[MetaKey::Channels].isArray())
        return fail(QStringLiteral("Missing or invalid array field: channels"));

    // channel_map array
    if (!obj.contains(MetaKey::ChannelMap) || !obj[MetaKey::ChannelMap].isArray())
        return fail(QStringLiteral("Missing or invalid array field: channel_map"));

    return true;
}

} // namespace ccv2
