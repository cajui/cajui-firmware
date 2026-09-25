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
    page.format(
        "<section><h2>Wi-Fi</h2><form method=\"post\" action=\"/wifi\">"
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
    if (v.scanning)
        page.format("<p><small>Scanning for networks&hellip;</small></p>");
    else
        page.format("<p><small>%u networks found%s. <a href=\"/scan\">Scan again</a></small></p>",
                    unsigned(v.networkCount + v.networksOmitted),
                    v.networksOmitted ? ", not all listed" : "");
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
    if (v.wifi == WifiState::Connected)
        page.format("<p><small><a href=\"/discover\">Search again</a></small></p>");
    page.format("<form method=\"post\" action=\"/broker\"><label for=\"host\">Address</label>"
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
            page.format("<tr><td>%016" PRIx64 "</td><td>%s</td><td>%" PRIu64 "</td><td>", t.node,
                        stateName(t.state), t.received);
            if (t.state == Enrollment::Active)
                page.format("<form method=\"post\" action=\"/revoke\"><input type=\"hidden\" "
                            "name=\"node\" value=\"%016" PRIx64 "\"><input type=\"hidden\" "
                            "name=\"generation\" value=\"%016" PRIx64 "\"><button>Revoke</button>"
                            "</form>",
                            t.node, t.generation);
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
        page.format("<form method=\"post\" action=\"/pair/open\" data-live-form><button>Search for "
                    "transmitters (2 minutes)</button></form>");
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
            page.format("<form method=\"post\" action=\"/pair/add\" data-live-form><p>%016" PRIx64
                        " <small>(%d dBm)</small> <input type=\"hidden\" name=\"node\" "
                        "value=\"%016" PRIx64 "\"><button>Add</button></p></form>",
                        pairing->nodes[i], int(pairing->rssi[i]), pairing->nodes[i]);
        }
        if (pairing->offered)
            page.format("<p><span class=\"spin\"></span>Waiting for %016" PRIx64
                        " to confirm&hellip;</p>",
                        pairing->offered);
        page.format("<form method=\"post\" action=\"/pair/stop\" data-live-form><button>Stop "
                    "searching</button></form>");
    }
    if (pairing->paired)
        page.format("<p class=\"notice\">Transmitter %016" PRIx64 " paired.</p>", pairing->paired);
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
                ". Setup closes after 10 minutes without activity.</small></p>",
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
    page.format("<form method=\"post\" action=\"/close\"><button>Close setup</button></form>"
                "</html>");
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
bool renderRevoke(uint64_t node, uint64_t generation, char* output, size_t capacity) {
    if (!output || !capacity) return false;
    Html page(output, capacity);
    head(page, "Revoke transmitter");
    page.format("<h1>Revoke transmitter %016" PRIx64 "?</h1><p>The receiver will reject its "
                "samples. Samples already queued are still forwarded. It can only be enrolled "
                "again with a new key over USB.</p><form method=\"post\" action=\"/revoke\">"
                "<input type=\"hidden\" name=\"node\" value=\"%016" PRIx64 "\"><input "
                "type=\"hidden\" name=\"generation\" value=\"%016" PRIx64 "\"><input "
                "type=\"hidden\" name=\"confirm\" value=\"1\"><button>Revoke</button></form>"
                "<p><a href=\"/\">Cancel</a></p></html>",
                node, node, generation);
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
