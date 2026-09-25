#include "cajui_setup.h"
#include "cajui_text.h"
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace cajui {
namespace {
// HTML on top of the bounded text builder; any truncation invalidates the page.
class Html : public TextBuffer {
public:
    using TextBuffer::TextBuffer;
    // Escapes text for element content and quoted attribute values.
    void text(const char* value) {
        for (const char* c = value ? value : ""; *c && ok(); ++c) {
            switch (*c) {
            case '&': format("&amp;"); break;
            case '<': format("&lt;"); break;
            case '>': format("&gt;"); break;
            case '"': format("&quot;"); break;
            case '\'': format("&#39;"); break;
            default: format("%c", *c);
            }
        }
    }
    // Percent-encodes a query parameter value.
    void query(const char* value) {
        for (const char* c = value; *c && ok(); ++c) {
            const bool plain = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') ||
                               (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '-' || *c == '_';
            if (plain)
                format("%c", *c);
            else
                format("%%%02X", unsigned(uint8_t(*c)));
        }
    }
};
// Opens a POST form carrying the session token; `live` forms are submitted in place.
void form(Html& page, const char* action, const char* token, bool live = false) {
    page.format("<form method=\"post\" action=\"%s\"%s><input type=\"hidden\" name=\"token\" "
                "value=\"",
                action, live ? " data-live-form" : "");
    page.text(token);
    page.format("\">");
}
bool copy(char* output, size_t capacity, const char* value) {
    const size_t length = std::strlen(value);
    if (length >= capacity) return false;
    std::memcpy(output, value, length + 1);
    return true;
}
void head(Html& page, const char* title) {
    page.format("<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>");
    page.text(title);
    page.format(
        "</title><style>body{font:16px system-ui,sans-serif;margin:0 auto;padding:16px;"
        "max-width:560px;background:#faf9f5;color:#24382f}section{background:#fff;"
        "border:1px solid #ddd;border-radius:8px;padding:12px 16px;margin:12px 0}"
        "label{display:block;margin:8px 0 2px}input{box-sizing:border-box;width:100%%;"
        "padding:10px;font-size:16px}button{margin-top:12px;padding:10px 16px;"
        "font-size:16px}table{width:100%%;border-collapse:collapse}td,th{text-align:left;"
        "padding:6px 4px;border-bottom:1px solid #eee;font-size:14px}.notice{background:"
        "#fff4d6;padding:10px;border-radius:6px}small{color:#55665c}.spin{display:inline-block;"
        "width:12px;height:12px;margin-right:6px;border:2px solid #cfd8d3;border-top-color:"
        "#24382f;border-radius:50%%;animation:spin 1s linear infinite}@keyframes spin{to{"
        "transform:rotate(360deg)}}</style>");
}
const char* stateName(Enrollment state) {
    switch (state) {
    case Enrollment::Prepared: return "prepared";
    case Enrollment::Active: return "active";
    case Enrollment::Revoked: return "revoked";
    case Enrollment::Empty: break;
    }
    return "empty";
}
void status(Html& page, const SetupView& v) {
    page.format("<section><h2>Status</h2><p>Wi-Fi: ");
    switch (v.wifi) {
    case WifiState::Connected:
        page.format("connected to <b>");
        page.text(v.wifiSsid);
        page.format("</b> (");
        page.text(v.address);
        page.format(")");
        break;
    case WifiState::Connecting:
        page.format("connecting to <b>");
        page.text(v.wifiSsid);
        page.format("</b>&hellip;");
        break;
    case WifiState::Failed: page.format("<b>could not connect</b>; check the password"); break;
    case WifiState::Idle: page.format("not configured"); break;
    }
    if (v.wifiTrialFailed)
        page.format("<br><b>The new Wi-Fi settings did not connect.</b> Nothing was saved; check "
                    "the network and password.");
    page.format("<br>Broker: %s", v.brokerOnline ? "<b>online</b>" : "offline");
    if (v.staged && v.staged->host[0]) {
        page.format(" (");
        page.text(v.staged->host);
        page.format(":%u, user ", unsigned(v.staged->port));
        page.text(v.staged->username);
        page.format(")");
    }
    page.format("<br>Samples waiting to be forwarded: %u</p>", unsigned(v.queued));
    if (v.staged && !v.saved)
        page.format("<p><small>Changes are staged. They are saved when both Wi-Fi and broker "
                    "are complete.</small></p>");
    page.format("<p><a href=\"/\">Refresh</a></p></section>");
}
void wifiForm(Html& page, const SetupView& v) {
    page.format("<section><h2>Wi-Fi</h2>");
    form(page, "/wifi", v.token);
    page.format(
        "<label for=\"ssid\">Network (2.4 GHz)</label>"
        "<input id=\"ssid\" name=\"ssid\" list=\"networks\" maxlength=\"32\" required value=\"");
    page.text(v.staged ? v.staged->ssid : "");
    page.format("\"><datalist id=\"networks\">");
    for (size_t i = 0; i < v.networkCount; ++i) {
        page.format("<option value=\"");
        page.text(v.networks[i].ssid);
        page.format("\">%d dBm</option>", v.networks[i].rssi);
    }
    page.format(
        "</datalist><label for=\"wifipass\">Password</label><input id=\"wifipass\" "
        "name=\"password\" type=\"password\" maxlength=\"64\" autocomplete=\"off\" "
        "placeholder=\"Leave empty to keep the saved one\"><button>Connect</button></form>");
    if (v.scanning) {
        page.format("<p><small>Scanning for networks&hellip;</small></p>");
    } else {
        page.format("<p><small>%u networks found%s.</small></p>",
                    unsigned(v.networkCount + v.networksOmitted),
                    v.networksOmitted ? ", not all listed" : "");
        form(page, "/scan", v.token);
        page.format("<button>Scan again</button></form>");
    }
    page.format("</section>");
}
void brokerForm(Html& page, const SetupView& v) {
    const char* host = v.prefillHost ? v.prefillHost : v.staged ? v.staged->host : "";
    const unsigned port = v.prefillHost ? v.prefillPort : v.staged ? v.staged->port : 0;
    page.format("<section><h2>MQTT broker</h2>");
    if (v.searching) page.format("<p><small>Searching the network&hellip;</small></p>");
    for (size_t i = 0; i < v.brokerCount; ++i) {
        page.format("<p>Found <b>");
        page.text(v.brokers[i].host);
        page.format(":%u</b> <a href=\"/?host=", unsigned(v.brokers[i].port));
        page.query(v.brokers[i].host);
        page.format("&amp;port=%u\">Use</a></p>", unsigned(v.brokers[i].port));
    }
    if (!v.searching && !v.brokerCount)
        page.format("<p><small>No broker announced on this network. Enter its address below."
                    "</small></p>");
    if (v.wifi == WifiState::Connected) {
        form(page, "/discover", v.token);
        page.format("<button>Search again</button></form>");
    }
    form(page, "/broker", v.token);
    page.format("<label for=\"host\">Address</label>"
                "<input id=\"host\" name=\"host\" maxlength=\"64\" required value=\"");
    page.text(host);
    page.format("\"><label for=\"port\">Port</label><input id=\"port\" name=\"port\" "
                "inputmode=\"numeric\" maxlength=\"5\" required value=\"");
    if (port) page.format("%u", port);
    page.format("\"><label for=\"user\">Username</label><input id=\"user\" name=\"username\" "
                "maxlength=\"64\" required autocapitalize=\"none\" value=\"");
    page.text(v.staged ? v.staged->username : "");
    page.format("\"><label for=\"mqttpass\">Password</label><input id=\"mqttpass\" "
                "name=\"password\" type=\"password\" maxlength=\"64\" autocomplete=\"off\" "
                "placeholder=\"Empty keeps the saved one for the same broker\"><button>Save "
                "broker</button>"
                "</form><p><small>The username is also the source ID of the samples. Plain MQTT: "
                "use a trusted network.</small></p></section>");
}
// Inner content of the transmitters section; also served alone for live updates.
void transmitterList(Html& page, const SetupView& v) {
    page.format("<h2>Transmitters</h2>");
    if (!v.transmitterCount) {
        page.format("<p>No transmitter enrolled.</p>");
    } else {
        page.format("<table><tr><th>Node</th><th>State</th><th>Last sample</th><th></th></tr>");
        for (size_t i = 0; i < v.transmitterCount; ++i) {
            const auto& t = v.transmitters[i];
            page.format("<tr><td>%016" PRIx64 "</td><td>%s</td><td>", t.node, stateName(t.state));
            // A re-paired node shows two active rows until its first sample under the new key.
            if (t.received)
                page.format("%" PRIu64 "</td><td>", t.received);
            else
                page.format("none yet</td><td>");
            if (t.state == Enrollment::Active) {
                form(page, "/revoke", v.token);
                page.format("<input type=\"hidden\" name=\"node\" value=\"%016" PRIx64
                            "\"><input type=\"hidden\" name=\"generation\" value=\"%016" PRIx64
                            "\"><button>Revoke</button></form>",
                            t.node, t.generation);
            }
            page.format("</td></tr>");
        }
        page.format("</table>");
    }
    const PairingView* pairing = v.pairing;
    if (!pairing) {
        page.format("<p><small>New transmitters are enrolled over USB.</small></p>");
        return;
    }
    page.format("<h3>Add a transmitter</h3>");
    if (!pairing->open) {
        form(page, "/pair/open", v.token, true);
        page.format("<button>Search for transmitters (2 minutes)</button></form>");
    } else {
        // data-live keeps the page script refreshing this section while the window is open.
        page.format("<p data-live><span class=\"spin\"></span>Searching, %u s left. Hold the PRG "
                    "button of the transmitter for 3 seconds; its LED blinks fast while it asks to "
                    "join.</p>",
                    unsigned(pairing->remainingSeconds));
        if (!pairing->count)
            page.format("<p><small>No transmitter asking to join yet.</small></p>");
        for (size_t i = 0; i < pairing->count && i < MaxPairingCandidates; ++i) {
            if (pairing->conflict[i]) {
                page.format("<p>%016" PRIx64 " <small>Two devices answered with this ID. Stop "
                            "searching, keep only your transmitter in pairing mode and search "
                            "again.</small></p>",
                            pairing->nodes[i]);
                continue;
            }
            form(page, "/pair/add", v.token, true);
            page.format("<p>%016" PRIx64 " <small>(%d dBm)</small> <input type=\"hidden\" "
                        "name=\"node\" value=\"%016" PRIx64 "\"><button>Add</button></p></form>",
                        pairing->nodes[i], int(pairing->rssi[i]), pairing->nodes[i]);
        }
        if (pairing->offered)
            page.format("<p><span class=\"spin\"></span>Waiting for %016" PRIx64
                        " to confirm&hellip;</p>",
                        pairing->offered);
        form(page, "/pair/stop", v.token, true);
        page.format("<button>Stop searching</button></form>");
    }
    if (pairing->paired)
        page.format("<p class=\"notice\">Transmitter %016" PRIx64 " paired. Its previous key, if "
                    "any, keeps working until it sends with the new one.</p>",
                    pairing->paired);
    page.format("<p><small>Add only a transmitter you just put in pairing mode, and keep it close. "
                "Pairing is not protected against an attacker in radio range during the search."
                "</small></p>");
}
// Refreshes the transmitters section every second while it is live, and submits the pairing
// forms in place. Without JavaScript the forms reload the page as before.
constexpr char LiveScript[] =
    "<script>(function(){var box=document.getElementById('transmitters');if(!box||!window.fetch)"
    "return;function load(){fetch('/transmitters',{cache:'no-store'}).then(function(r){return "
    "r.ok?r.text():null}).then(function(t){if(t!==null)box.innerHTML=t}).catch(function(){})}"
    "box.addEventListener('submit',function(e){var f=e.target;if(!f.hasAttribute('data-live-"
    "form'))return;e.preventDefault();var b=f.querySelector('button');if(b)b.disabled=true;"
    "fetch(f.action,{method:'POST',body:new URLSearchParams(new FormData(f))}).then(load,load)});"
    "setInterval(function(){if(box.querySelector('[data-live]'))load()},1000)})();</script>";
void transmitters(Html& page, const SetupView& v) {
    page.format("<section id=\"transmitters\">");
    transmitterList(page, v);
    page.format("</section>");
    if (v.pairing) page.format("%s", LiveScript);
}
} // namespace

void SetupSession::open(uint32_t now, const uint8_t (&random)[TokenBytes]) {
    static const char digits[] = "0123456789abcdef";
    constexpr uint8_t Nibble = 0xf;
    for (size_t i = 0; i < TokenBytes; ++i) {
        token_[2 * i] = digits[random[i] >> 4];
        token_[2 * i + 1] = digits[random[i] & Nibble];
    }
    token_[2 * TokenBytes] = 0;
    openedAt_ = lastActivity_ = now;
    active_ = true;
}
void SetupSession::close() {
    active_ = false;
    for (auto& c : token_) c = 0;
}
void SetupSession::touch(uint32_t now) {
    if (active_) lastActivity_ = now;
}
bool SetupSession::expired(uint32_t now) const {
    return active_ &&
           (uint32_t(now - lastActivity_) >= IdleMs || uint32_t(now - openedAt_) >= MaxMs);
}
bool SetupSession::validToken(const char* candidate) const {
    if (!active_ || !candidate) return false;
    uint8_t difference = 0;
    size_t i = 0;
    for (; i < 2 * TokenBytes && candidate[i]; ++i) difference |= uint8_t(candidate[i] ^ token_[i]);
    return i == 2 * TokenBytes && !candidate[i] && !difference;
}
bool allowedHost(const char* host, const char* address) {
    if (!host || !address || !*address) return false;
    const size_t length = std::strlen(address);
    return !std::strncmp(host, address, length) &&
           (!host[length] || !std::strcmp(host + length, ":80"));
}
bool allowedOrigin(const char* origin, const char* address) {
    static const char scheme[] = "http://";
    if (!origin || !*origin) return true;
    if (std::strncmp(origin, scheme, sizeof(scheme) - 1) != 0) return false;
    return allowedHost(origin + sizeof(scheme) - 1, address);
}
const char* noticeText(Notice notice) {
    switch (notice) {
    case Notice::WifiUnchanged: return "Wi-Fi settings unchanged.";
    case Notice::WifiTrying:
        return "Connecting to the new network. The settings are saved once it connects and the "
               "broker section is complete.";
    case Notice::WifiStaged: return "Connecting. Complete the broker section to save.";
    case Notice::WifiFailed:
        return "Could not connect with the new Wi-Fi settings; the saved ones were kept.";
    case Notice::BrokerSaved: return "Broker saved. Forwarding restarts with these settings.";
    case Notice::BrokerStaged: return "Broker staged. It is saved once the Wi-Fi section connects.";
    case Notice::SaveFailed: return "Could not save the settings.";
    case Notice::Revoked: return "Transmitter revoked.";
    case Notice::RevokeFailed: return "Could not revoke.";
    case Notice::UnknownTransmitter: return "Unknown transmitter.";
    case Notice::PairingUnavailable: return "Radio pairing is not available.";
    case Notice::PairingOpened: return "Searching for transmitters for 2 minutes.";
    case Notice::PairingStopped: return "Stopped searching.";
    case Notice::OfferSent: return "Offer sent. The transmitter confirms on its next request.";
    case Notice::AddFailed: return "Could not add this transmitter; search again.";
    case Notice::AddConflict:
        return "Two devices answered with this ID. Stop, keep only your transmitter in pairing "
               "mode and search again.";
    case Notice::SsidInvalid:
    case Notice::WifiPasswordInvalid:
    case Notice::HostInvalid:
    case Notice::PortInvalid:
    case Notice::UsernameInvalid:
    case Notice::MqttPasswordInvalid:
        return describe(SetupError(unsigned(notice) - unsigned(Notice::SsidInvalid) + 1));
    case Notice::None:
    case Notice::Count: break;
    }
    return nullptr;
}
static_assert(unsigned(Notice::MqttPasswordInvalid) - unsigned(Notice::SsidInvalid) ==
                  unsigned(SetupError::MqttPassword) - unsigned(SetupError::Ssid),
              "Notices for setup errors must follow SetupError");
Notice noticeFor(SetupError error) {
    return error == SetupError::None ? Notice::None
                                     : Notice(unsigned(Notice::SsidInvalid) + unsigned(error) -
                                              unsigned(SetupError::Ssid));
}
Notice parseNotice(const char* value) {
    constexpr size_t Digits = 2;
    constexpr unsigned Decimal = 10;
    if (!value || !*value || std::strlen(value) > Digits) return Notice::None;
    unsigned code = 0;
    for (const char* c = value; *c; ++c) {
        if (*c < '0' || *c > '9') return Notice::None;
        code = code * Decimal + unsigned(*c - '0');
    }
    return code < unsigned(Notice::Count) ? Notice(code) : Notice::None;
}
bool LongPress::update(bool pressed, uint32_t now) {
    if (!pressed) {
        down_ = fired_ = false;
        return false;
    }
    if (!down_) {
        down_ = true;
        since_ = now;
    }
    if (fired_ || uint32_t(now - since_) < hold_) return false;
    fired_ = true;
    return true;
}
bool setupSsid(uint64_t device, char* output, size_t capacity) {
    if (!output || !capacity) return false;
    const int written = std::snprintf(output, capacity, "Cajui-%04X", unsigned(device & 0xffffu));
    return written > 0 && size_t(written) < capacity;
}
bool wifiQr(const char* ssid, char* output, size_t capacity) {
    if (!ssid || !*ssid || !output || !capacity) return false;
    Html code(output, capacity);
    code.format("WIFI:T:nopass;S:");
    for (const char* c = ssid; *c; ++c) {
        if (std::strchr("\\;,:\"", *c)) code.format("\\");
        code.format("%c", *c);
    }
    code.format(";;");
    return code.ok();
}
SetupError stageWifi(UplinkConfig& pending, const char* ssid, const char* password) {
    UplinkConfig next = pending;
    if (!ssid || !copy(next.ssid, sizeof(next.ssid), ssid) || !*ssid) return SetupError::Ssid;
    const bool keep = password && !*password && !std::strcmp(pending.ssid, ssid);
    if (!keep && (!password || !copy(next.wifiPassword, sizeof(next.wifiPassword), password) ||
                  std::strlen(password) < MinWifiPassword))
        return SetupError::WifiPassword;
    pending = next;
    wipe(next);
    return SetupError::None;
}
SetupError stageBroker(UplinkConfig& pending, const char* host, const char* port,
                       const char* username, const char* password) {
    UplinkConfig next = pending;
    if (!validHost(host) || !copy(next.host, sizeof(next.host), host)) return SetupError::Host;
    if (!parsePort(port, next.port)) return SetupError::Port;
    if (!username || !validIdentity(username) ||
        !copy(next.username, sizeof(next.username), username))
        return SetupError::Username;
    // The saved password goes only to the broker it was entered for: changing the host or
    // port without re-entering it would let anyone on the setup page collect it.
    const bool keep = password && !*password && pending.password[0] &&
                      !std::strcmp(pending.username, username) &&
                      !std::strcmp(pending.host, next.host) && pending.port == next.port;
    if (!keep && (!password || !*password || !copy(next.password, sizeof(next.password), password)))
        return SetupError::MqttPassword;
    pending = next;
    wipe(next);
    return SetupError::None;
}
const char* describe(SetupError error) {
    switch (error) {
    case SetupError::None: return "Saved.";
    case SetupError::Ssid: return "Enter a network name of up to 32 bytes.";
    case SetupError::WifiPassword: return "The Wi-Fi password needs 8 to 64 characters.";
    case SetupError::Host: return "Enter an IPv4 address or host name.";
    case SetupError::Port: return "The port must be a number from 1 to 65535.";
    case SetupError::Username:
        return "The username uses letters, digits, '.', '_', ':' or '-', up to 64 characters.";
    case SetupError::MqttPassword: return "Enter the broker password (up to 64 characters).";
    }
    return "Invalid input.";
}
namespace {
bool renderPage(const SetupView& v, char* output, size_t capacity) {
    Html page(output, capacity);
    head(page, "Cajuí receiver setup");
    page.format("<h1>Cajuí receiver</h1><p><small>Device %016" PRIx64
                ". Setup closes after 10 minutes without activity, and 30 minutes after "
                "opening at the latest.</small></p>",
                v.device);
    if (v.notice) {
        page.format("<p class=\"notice\">");
        page.text(v.notice);
        page.format("</p>");
    }
    status(page, v);
    wifiForm(page, v);
    brokerForm(page, v);
    transmitters(page, v);
    form(page, "/close", v.token);
    page.format("<button>Close setup</button></form></html>");
    return page.ok();
}
} // namespace
bool renderSetup(const SetupView& v, char* output, size_t capacity) {
    if (!output || !capacity) return false;
    SetupView fitted = v;
    while (!renderPage(fitted, output, capacity)) {
        if (fitted.networkCount) {
            --fitted.networkCount;
            ++fitted.networksOmitted;
        } else if (fitted.brokerCount) {
            --fitted.brokerCount;
        } else {
            return false;
        }
    }
    return true;
}
bool renderTransmitters(const SetupView& v, char* output, size_t capacity) {
    if (!output || !capacity) return false;
    Html page(output, capacity);
    transmitterList(page, v);
    return page.ok();
}
bool renderRevoke(uint64_t node, uint64_t generation, const char* token, char* output,
                  size_t capacity) {
    if (!output || !capacity || !token) return false;
    Html page(output, capacity);
    head(page, "Revoke transmitter");
    page.format("<h1>Revoke transmitter %016" PRIx64 "?</h1><p>The receiver will reject its "
                "samples. Samples already queued are still forwarded. To use it again, pair it "
                "by radio or enroll it over USB; either gives it a new key.</p>",
                node);
    form(page, "/revoke", token);
    page.format("<input type=\"hidden\" name=\"node\" value=\"%016" PRIx64 "\"><input "
                "type=\"hidden\" name=\"generation\" value=\"%016" PRIx64 "\"><input "
                "type=\"hidden\" name=\"confirm\" value=\"1\"><button>Revoke</button></form>"
                "<p><a href=\"/\">Cancel</a></p></html>",
                node, generation);
    return page.ok();
}
bool renderClosed(char* output, size_t capacity) {
    if (!output || !capacity) return false;
    Html page(output, capacity);
    head(page, "Setup closed");
    page.format("<h1>Setup closed</h1><p>The setup network is turning off. Hold the button "
                "again to reopen it.</p></html>");
    return page.ok();
}
} // namespace cajui
