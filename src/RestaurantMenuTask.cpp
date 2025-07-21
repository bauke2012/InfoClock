#include "RestaurantMenuTask.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include "tasks_utils.h"
#include "utils.h"
#include "time.h"
#include "config.h"
#include <vector>
#include <set>

// HTML for status page
static const char menuStatusPage[] PROGMEM = R"_(
<table>
<tr><th>Restaurant Menu</th></tr>
<tr><td class="l">Last refresh:</td><td>$timestamp$</td></tr>
<tr><td class="l">Restaurant:</td><td>$restaurant$</td></tr>
<tr><td class="l">Switch hour:</td><td>$switchhour$</td></tr>
<tr><td class="l">Menu date:</td><td>$menudate$</td></tr>
<tr><td class="l">Menu:</td><td>$menu$</td></tr>
</table>
</body>
<script>setTimeout(function(){window.location.reload(1);}, 15000);</script>
</html>
)_";

FlashStream menuStatusPageFS(menuStatusPage);

#define MAX_DISHES 10
#define MENU_FETCH_INTERVAL_S 900_s
#define DISPLAY_PERIOD 0.025_s

namespace Tasks {

static const struct {
    int code;
    const char* id;
} restaurants[] = {
    {3, "33-restaurant-r3"},
    {2, "21-restaurant-r2"},
    {1, "13-restaurant-r1"}
};
static constexpr size_t NUM_RESTAURANTS = sizeof(restaurants) / sizeof(restaurants[0]);

inline String codeToId(int code) {
    for (size_t i = 0; i < NUM_RESTAURANTS; ++i)
        if (restaurants[i].code == code)
            return restaurants[i].id;
    return restaurants[0].id;
}
inline int codeSanitize(int code) {
    for (size_t i = 0; i < NUM_RESTAURANTS; ++i)
        if (restaurants[i].code == code)
            return code;
    return restaurants[0].code;
}

String normalizeFrenchText(const String& in) {
    String out;
    out.reserve(in.length());

    for (size_t i = 0; i < in.length(); ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0xC3) { // ASCII or simple Latin-1
            out += in[i];
            continue;
        }

        // Multi-byte UTF-8
        if (c == 0xC3 && i+1 < in.length()) {
            unsigned char d = (unsigned char)in[i+1];
            switch (d) {
                case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: out += 'a'; break; // àâäãáå
                case 0xA7: out += 'c'; break; // ç
                case 0xA8: case 0xA9: case 0xAA: case 0xAB: out += 'e'; break; // èéêë
                case 0xAC: case 0xAD: case 0xAE: case 0xAF: out += 'i'; break; // ìíîï
                case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: out += 'o'; break; // òóôõö
                case 0xB9: case 0xBA: case 0xBB: case 0xBC: out += 'u'; break; // ùúûü
                case 0xBF: out += 'y'; break; // ÿ
                case 0x80: case 0x82: case 0x83: case 0x84: case 0x85: out += 'A'; break; // ÀÂÃÄÅ
                case 0x87: out += 'C'; break; // Ç
                case 0x88: case 0x89: case 0x8A: case 0x8B: out += 'E'; break; // ÈÉÊË
                case 0x8C: case 0x8D: case 0x8E: case 0x8F: out += 'I'; break; // ÌÍÎÏ
                case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: out += 'O'; break; // ÒÓÔÕÖ
                case 0x99: case 0x9A: case 0x9B: case 0x9C: out += 'U'; break; // ÙÚÛÜ
                case 0x9F: out += 'Y'; break; // Ÿ
                default: out += in[i]; out += in[i+1];
            }
            i++;
            continue;
        }
        // Œ, œ (C5 92, C5 93)
        if (c == 0xC5 && i+1 < in.length()) {
            unsigned char d = (unsigned char)in[i+1];
            if (d == 0x92) { out += "OE"; i++; continue; }
            if (d == 0x93) { out += "oe"; i++; continue; }
        }
        // ’ (curly apostrophe)
        if (c == 0xE2 && i + 2 < in.length()) {
            if ((unsigned char)in[i+1] == 0x80 && (unsigned char)in[i+2] == 0x99) {
                out += "'"; i += 2; continue;
            }
        }
        out += in[i];
    }
    return out;
}

static String trimmedKeyWords(const String& dish, int maxWords = 3) {
    const char* stopwords[] = { "aux","de","et","avec","à","le","la","du","des","en","au","sur","pour","les","un","une","d'","l'","with","and","of","in","for","the","to","on","at","from","by","an","a" };
    const size_t nStops = sizeof(stopwords) / sizeof(stopwords[0]);
    String out; int found = 0; size_t start = 0;
    while (found < maxWords && start < dish.length()) {
        size_t end = dish.indexOf(' ', start); if (end == (size_t)-1)end = dish.length();
        String word=dish.substring(start,end); word.trim();
        word.replace(",", ""); word.replace(".", ""); word.replace(";", ""); word.replace("/", "");
        word.replace(":", ""); word.replace("-", ""); word.replace("(", ""); word.replace(")", "");
        bool isStop = false;
        for (size_t j = 0; j < nStops; ++j)
            if (word.equalsIgnoreCase(stopwords[j])) { isStop = true; break; }
        if (!isStop && word.length() > 0) { if (!out.isEmpty())out += ' '; out += word; found++; }
        start = end + 1;
    }
    return out;
}

RestaurantMenuTask::RestaurantMenuTask() {
    addRegularMessage({this, [this]() {return getMenuString(); }, DISPLAY_PERIOD, 1, true});
    registerPage("menu", "Restaurant menu", [this](ESP8266WebServer& ws) {handleStatusPage(ws);});
}

String RestaurantMenuTask::makeMenuDateString(time_t base) const {
    char buf[11];
    struct tm* t = localtime(&base);
    strftime(buf, sizeof(buf), "%Y-%m-%d", t);
    return String(buf);
}

void RestaurantMenuTask::updateMenuSwitchHourFromConfig() {
    int val = readConfig(F("menuSwitchHour")).toInt();
    if (val >= 0 && val <= 23)
        menuSwitchHour = val;
    else
        menuSwitchHour = 14;
}

void RestaurantMenuTask::run() {
    updateMenuSwitchHourFromConfig();

    int code = readConfig(F("restaurant")).toInt();
    restaurantCode = codeSanitize(code);
    restaurantId = codeToId(restaurantCode);

    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    int hour = t->tm_hour;

    String activeMenuDate;
    if (hour >= menuSwitchHour) {
        now += 24 * 60 * 60;
    }
    activeMenuDate = makeMenuDateString(now);

    bool menuBoundary = (lastFetchedMenuDate != activeMenuDate);
    bool hourBoundary = (lastFetchHour != hour);
    if (menuBoundary || hourBoundary) {
        lastFetchedMenuDate = activeMenuDate;
        lastFetchHour = hour;
        fetchMenu(activeMenuDate);
    }

    sleep(MENU_FETCH_INTERVAL_S);
}

void RestaurantMenuTask::fetchMenu(const String& dateStr) {
    dishes.clear();
    std::set<String> seen;
    String url = "https://api.mynovae.ch/en/api/v2/salepoints/" + restaurantId + "/menus/" + dateStr;

    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    http.begin(client, url); http.useHTTP10(true);
    http.addHeader("Novae-Codes", novaeKey);
    http.addHeader("Accept", "application/json");
    http.addHeader("X-Requested-With", "xmlhttprequest");

    int httpCode = -1, attempts = 3;
    while (attempts-- && httpCode == -1) {
        httpCode = http.GET();
        if (httpCode == -1) delay(2000);
    }

    if (httpCode == 200) {
        WiFiClient* stream = http.getStreamPtr();
        while (stream->available()) {
            char c = stream->peek();
            if (c == '[') { stream->read(); break; }
            if (isspace(c)) stream->read(); else break;
        }
        while (stream->available()) {
            char c = stream->peek();
            if (isspace(c) || c == ',') { stream->read(); continue; }
            if (c == ']') { stream->read(); break; }
            if (c != '{') { stream->read(); continue; }

            StaticJsonDocument<768> doc;
            StaticJsonDocument<64> filter;
            filter["title"] = true;
            filter["model"]["service"] = true;

            DeserializationError err = deserializeJson(doc, *stream, DeserializationOption::Filter(filter));

            if (!err) {
                // filter lunch menu only "midi"
                const char* service = doc["model"]["service"];
                if (!service) continue;
                String serviceStr(service);
                serviceStr.toLowerCase();
                if (serviceStr != "midi") continue;

                JsonObject title = doc["title"];
                String dish;
                if (title.containsKey("en") && strlen(title["en"])) dish = String(title["en"].as<const char*>());
                else if (title.containsKey("fr") && strlen(title["fr"])) dish = String(title["fr"].as<const char*>());
                if (dish.length()) {
                    String tmp = trimmedKeyWords(normalizeFrenchText(dish), 3);
                    if (tmp.length() && seen.find(tmp) == seen.end()) {
                        seen.insert(tmp); 
                        dishes.push_back(tmp); 
                    }
                }
            } else {
                int depth = 0;
                while (stream->available()) {
                    char cc = stream->read();
                    if (cc == '{') depth++;
                    if (cc == '}') { if (depth == 0) break; depth--; }
                }
            }
        }

        String allDishes;
        for (auto& dish : dishes) {
            if (!allDishes.isEmpty()) allDishes += " | ";
            allDishes += dish;
        }
        cachedMenuDate = dateStr;
        cachedMenuLine = "R" + String(restaurantCode) + " menu: " + allDishes;

        // Update status page timestamp
        lastStatusTimestamp = getDateTime();
    }
    http.end();
}

String RestaurantMenuTask::getMenuString() const {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    int hour = t->tm_hour;
    time_t wantTime = now;
    bool showTomorrow = false;
    if (hour >= menuSwitchHour) {
        wantTime += 24 * 60 * 60;
        showTomorrow = true;
    }
    String wantedDate = makeMenuDateString(wantTime);
    if (cachedMenuDate != wantedDate || cachedMenuLine.isEmpty())
        return "[menu?]";
    String labelPrefix = showTomorrow ? "Tomorrow's " : "Today's ";
    return labelPrefix + cachedMenuLine;
}

void RestaurantMenuTask::handleStatusPage(ESP8266WebServer& webServer) {
    StringStream ss(2048);
    macroStringReplace(pageHeaderFS, constString(F("Restaurant Menu")), ss);

    std::map<String, String> m = {
        {F("timestamp"), lastStatusTimestamp},
        {F("restaurant"), restaurantId},
        {F("switchhour"), String(menuSwitchHour)},
        {F("menudate"), cachedMenuDate},
        {F("menu"), cachedMenuLine}
    };

    macroStringReplace(menuStatusPageFS, mapLookup(m), ss);
    webServer.send(200, textHtml, ss.buffer);
}

}