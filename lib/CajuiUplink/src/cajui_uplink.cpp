#include "cajui_uplink.h"
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace cajui {
namespace {
constexpr uint8_t UplinkMagic[4] = {'C', 'J', 'U', 'P'};
constexpr uint8_t UplinkVersion = 1;
constexpr uint32_t Crc32Polynomial = 0xedb88320u; // Reflected IEEE 802.3, as in snapshots.
constexpr uint32_t MaxExpectedInterval = 604800;  // Central accepts 1 second to 7 days.
constexpr int32_t MilliPerUnit = 1000;
uint32_t checksum(const uint8_t* data, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (Crc32Polynomial & (0u - (crc & 1u)));
    }
    return ~crc;
}
bool alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
size_t bounded(const char* text, size_t capacity) {
    size_t length = 0;
    while (length <= capacity && text[length]) ++length;
    return length;
}
bool validHost(const char* host) {
    const size_t length = bounded(host, HostCapacity);
    if (!length || length > HostCapacity) return false;
    for (size_t i = 0; i < length; ++i)
        if (!alnum(host[i]) && host[i] != '.' && host[i] != '-') return false;
    return true;
}
bool validSecret(const char* text, size_t minimum, size_t capacity) {
    const size_t length = bounded(text, capacity);
    return length >= minimum && length <= capacity;
}
const char* metricName(uint16_t metric) {
    switch (metric) {
    case 1: return "temperature";
    case 2: return "humidity";
    default: return nullptr;
    }
}
const char* unitName(uint8_t unit) {
    switch (unit) {
    case 1: return "degC";
    case 2: return "%";
    default: return nullptr;
    }
}
const char* statusName(Status status) {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::Error: return "error";
    case Status::Skipped: return "skipped";
    }
    return nullptr;
}
// Appends formatted text; any truncation makes the whole document invalid.
class Text {
public:
    Text(char* output, size_t capacity) : output_(output), capacity_(capacity) {}
    // NOLINTNEXTLINE(cert-dcl50-cpp): bounded printf-style helper over vsnprintf.
    void add(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        if (!ok_) return;
        va_list arguments;
        va_start(arguments, format);
        const int written = std::vsnprintf(output_ + used_, capacity_ - used_, format, arguments);
        va_end(arguments);
        if (written < 0 || size_t(written) >= capacity_ - used_)
            ok_ = false;
        else
            used_ += size_t(written);
    }
    bool ok() const { return ok_; }
    size_t size() const { return used_; }

private:
    char* output_;
    size_t capacity_, used_ = 0;
    bool ok_ = true;
};
void addReading(Text& text, const Reading& reading) {
    text.add("{\"sensor_id\":\"sensor-%u\",", unsigned(reading.sensor));
    const char* metric = metricName(reading.metric);
    if (metric)
        text.add("\"metric\":\"%s\",", metric);
    else
        text.add("\"metric\":\"metric-%u\",", unsigned(reading.metric));
    if (reading.status == Status::Ok) {
        const int64_t value = reading.milliValue;
        const uint64_t magnitude = value < 0 ? uint64_t(-value) : uint64_t(value);
        text.add("\"value\":%s%" PRIu64 ".%03" PRIu64 ",", value < 0 ? "-" : "",
                 magnitude / MilliPerUnit, magnitude % MilliPerUnit);
    }
    const char* unit = unitName(reading.unit);
    if (unit)
        text.add("\"unit\":\"%s\",", unit);
    else
        text.add("\"unit\":\"unit-%u\",", unsigned(reading.unit));
    text.add("\"status\":\"%s\"}", statusName(reading.status));
}
struct Writer {
    uint8_t* p;
    void text(const char* value) {
        const size_t length = std::strlen(value);
        *p++ = uint8_t(length);
        std::memcpy(p, value, length);
        p += length;
    }
};
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool text(char* value, size_t capacity) {
        if (p >= end) return false;
        const size_t length = *p++;
        if (length > capacity || size_t(end - p) < length) return false;
        std::memcpy(value, p, length);
        value[length] = 0;
        p += length;
        return std::strlen(value) == length; // Embedded NUL bytes are rejected.
    }
};
} // namespace

bool validIdentity(const char* text) {
    const size_t length = bounded(text, UsernameCapacity);
    if (!length || length > UsernameCapacity || !alnum(text[0])) return false;
    for (size_t i = 1; i < length; ++i)
        if (!alnum(text[i]) && text[i] != '.' && text[i] != '_' && text[i] != ':' && text[i] != '-')
            return false;
    return true;
}
bool validUplink(const UplinkConfig& c) {
    return validSecret(c.ssid, 1, SsidCapacity) &&
           validSecret(c.wifiPassword, MinWifiPassword, WifiPasswordCapacity) &&
           validHost(c.host) && c.port && validIdentity(c.username) &&
           validSecret(c.password, 1, MqttPasswordCapacity);
}
void wipe(UplinkConfig& c) {
    // Volatile writes so the compiler cannot drop the clearing of dead secrets.
    volatile auto* bytes = reinterpret_cast<volatile uint8_t*>(&c);
    for (size_t i = 0; i < sizeof(c); ++i) bytes[i] = 0;
}
bool saveUplink(AtomicBlob& blob, const UplinkConfig& c) {
    if (!validUplink(c)) return false;
    uint8_t bytes[UplinkBlobCapacity]{};
    Writer w{bytes};
    std::memcpy(w.p, UplinkMagic, sizeof(UplinkMagic));
    w.p += sizeof(UplinkMagic);
    *w.p++ = UplinkVersion;
    for (const char* value : {c.ssid, c.wifiPassword, c.host, c.username, c.password})
        w.text(value);
    *w.p++ = uint8_t(c.port >> 8);
    *w.p++ = uint8_t(c.port);
    const size_t payload = size_t(w.p - bytes);
    const uint32_t crc = checksum(bytes, payload);
    for (int i = 3; i >= 0; --i) *w.p++ = uint8_t(crc >> (i * 8));
    const bool saved = blob.replace(bytes, payload + 4);
    volatile uint8_t* clear = bytes;
    for (size_t i = 0; i < sizeof(bytes); ++i) clear[i] = 0;
    return saved;
}
ReadResult loadUplink(AtomicBlob& blob, UplinkConfig& c) {
    wipe(c);
    uint8_t bytes[UplinkBlobCapacity]{};
    size_t size = 0;
    const auto read = blob.read(bytes, sizeof(bytes), size);
    ReadResult result = read;
    if (read == ReadResult::Ok) result = ReadResult::Error;
    if (read == ReadResult::Ok && size >= MinUplinkSize && size <= sizeof(bytes)) {
        uint32_t stored = 0;
        for (size_t i = 0; i < 4; ++i) stored = (stored << 8) | bytes[size - 4 + i];
        Reader r{bytes + sizeof(UplinkMagic) + 1, bytes + size - 4};
        if (std::memcmp(bytes, UplinkMagic, sizeof(UplinkMagic)) == 0 &&
            bytes[sizeof(UplinkMagic)] == UplinkVersion && stored == checksum(bytes, size - 4) &&
            r.text(c.ssid, SsidCapacity) && r.text(c.wifiPassword, WifiPasswordCapacity) &&
            r.text(c.host, HostCapacity) && r.text(c.username, UsernameCapacity) &&
            r.text(c.password, MqttPasswordCapacity) && r.end - r.p == 2) {
            c.port = uint16_t((r.p[0] << 8) | r.p[1]);
            if (validUplink(c)) result = ReadResult::Ok;
        }
    }
    volatile uint8_t* clear = bytes;
    for (size_t i = 0; i < sizeof(bytes); ++i) clear[i] = 0;
    if (result != ReadResult::Ok) wipe(c);
    return result;
}

bool formatTopic(const char* source, uint64_t device, char* output, size_t capacity) {
    if (!source || !validIdentity(source) || !output || !capacity) return false;
    Text text(output, capacity);
    text.add("telemetry/v1/%s/%016" PRIx64 "/samples", source, device);
    return text.ok();
}
bool formatSample(const char* source, const QueuedSample& sample, char* output, size_t capacity,
                  size_t& size) {
    size = 0;
    const auto& data = sample.data;
    if (!source || !validIdentity(source) || !output || !capacity || !data.count ||
        data.count > MaxReadings || !data.nextSeconds)
        return false;
    for (size_t i = 0; i < data.count; ++i)
        if (!statusName(data.readings[i].status)) return false;
    const uint32_t interval =
        data.nextSeconds > MaxExpectedInterval ? MaxExpectedInterval : data.nextSeconds;
    Text text(output, capacity);
    text.add("{\"version\":1,\"source_id\":\"%s\",\"device_id\":\"%016" PRIx64
             "\",\"sample_id\":\"%016" PRIx64 ".%" PRIu64
             "\",\"expected_interval_seconds\":%" PRIu32 ",\"readings\":[",
             source, sample.node, sample.generation, sample.counter, interval);
    for (size_t i = 0; i < data.count; ++i) {
        if (i) text.add(",");
        addReading(text, data.readings[i]);
    }
    text.add("]}");
    if (!text.ok()) return false;
    size = text.size();
    return true;
}

Forwarder::Forwarder(Publisher& publisher, Clock& clock, PersistentStore& store, const char* source)
    : publisher_(publisher), clock_(clock), store_(store) {
    if (source && validIdentity(source))
        std::memcpy(source_, source, std::strlen(source) + 1);
    else
        state_ = ForwardState::Failed;
}
void Forwarder::retryLater(uint32_t now) {
    delayed_ = true;
    retryAt_ = now + RetryDelayMs;
}
void Forwarder::poll(bool quiet) {
    if (state_ == ForwardState::Failed || !quiet) return;
    const uint32_t now = clock_.nowMs();
    if (state_ == ForwardState::Waiting) {
        int id = 0;
        while (publisher_.acknowledged(id)) {
            if (id != message_) continue; // A late PUBACK from an abandoned attempt.
            const Result result = store_.forwarded(node_, generation_, counter_);
            if (result == Result::StorageError) {
                state_ = ForwardState::Failed;
                return;
            }
            if (result == Result::Ok) ++forwarded_;
            state_ = ForwardState::Idle;
            return;
        }
        if (!publisher_.connected() || uint32_t(now - sentAt_) >= AckTimeoutMs) {
            ++retries_;
            state_ = ForwardState::Idle;
            retryLater(now);
        }
        return;
    }
    if (delayed_ && int32_t(now - retryAt_) < 0) return;
    delayed_ = false;
    if (!publisher_.connected()) return;
    QueuedSample sample{};
    if (!store_.peek(sample)) {
        if (!store_.healthy()) state_ = ForwardState::Failed;
        return;
    }
    size_t size = 0;
    if (!formatTopic(source_, sample.node, topic_, sizeof(topic_)) ||
        !formatSample(source_, sample, payload_, sizeof(payload_), size)) {
        state_ = ForwardState::Failed;
        return;
    }
    const int id = publisher_.publish(topic_, payload_, size);
    if (id <= 0) {
        retryLater(now);
        return;
    }
    node_ = sample.node;
    generation_ = sample.generation;
    counter_ = sample.counter;
    message_ = id;
    sentAt_ = now;
    state_ = ForwardState::Waiting;
}
} // namespace cajui
