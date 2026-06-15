#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
using namespace fs;          // WebServer.h references FS unqualified; export it
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "feedmodel.h"
#include "globals.h"
#include "config.h"

// ============================================================
//  FeedMixWebServer
//  Hosts a mobile-optimised web app on ESP32 AP (192.168.4.1)
//  All HTML/CSS/JS served inline — no SD card or SPIFFS needed
//  API endpoints (JSON):
//    GET  /api/status        - scale, active herd, load session
//    GET  /api/herds         - all herds
//    POST /api/herds         - create herd
//    GET  /api/herds/{id}    - single herd
//    POST /api/herds/{id}    - update herd
//    DELETE /api/herds/{id}  - delete herd
//    GET  /api/ingredients   - ingredient library
//    POST /api/ingredients   - update ingredient DM%
//    POST /api/tare          - tare the scale
// ============================================================

// Data is shared via globals.h (g_scaleKg, g_herds, g_ingredients, etc.)

class FeedMixWebServer {
public:
    FeedMixWebServer() : server(WEB_PORT) {}

    void begin() {
        // Start AP
        WiFi.mode(WIFI_AP);
        WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
        delay(100);

        // Mount flash filesystem for the feed log (format on first use)
        if (!LittleFS.begin(true)) {
            Serial.println("[FS] LittleFS mount failed");
        }

        Serial.printf("[WiFi] AP started: SSID=%s  Pass=%s  IP=%s\n",
                      WIFI_SSID, WIFI_PASSWORD, WIFI_IP);

        // Routes
        server.on("/",                  HTTP_GET,    [this]{ handleRoot(); });
        server.on("/api/status",        HTTP_GET,    [this]{ handleStatus(); });
        server.on("/api/herds",         HTTP_GET,    [this]{ handleGetHerds(); });
        server.on("/api/herds",         HTTP_POST,   [this]{ handleCreateHerd(); });
        server.on("/api/ingredients",   HTTP_GET,    [this]{ handleGetIngredients(); });
        server.on("/api/ingredients",   HTTP_POST,   [this]{ handleUpdateIngredients(); });
        server.on("/api/tare",          HTTP_POST,   [this]{ handleTare(); });
        server.on("/api/load/start",     HTTP_POST,   [this]{ handleLoadStart(); });
        server.on("/api/load/activate",  HTTP_POST,   [this]{ handleLoadActivate(); });
        server.on("/api/load/addstep",   HTTP_POST,   [this]{ handleLoadAddStep(); });
        server.on("/api/load/addherd",   HTTP_POST,   [this]{ handleLoadAddHerd(); });
        server.on("/api/load/removeherd",HTTP_POST,   [this]{ handleLoadRemoveHerd(); });
        server.on("/api/settings",       HTTP_POST,   [this]{ handleSetSettings(); });
        server.on("/api/load/confirm",   HTTP_POST,   [this]{ handleLoadConfirm(); });
        server.on("/api/load/finish",    HTTP_POST,   [this]{ handleLoadFinish(); });
        server.on("/api/cows",           HTTP_POST,   [this]{ handleSetCows(); });
        server.on("/api/log/add",        HTTP_POST,   [this]{ handleLogAdd(); });
        server.on("/api/log/query",      HTTP_GET,    [this]{ handleLogQuery(); });
        server.on("/api/log/clear",      HTTP_POST,   [this]{ handleLogClear(); });
        server.onNotFound(             [this]{ handleNotFound(); });

        // Dynamic routes for /api/herds/{id}
        server.on("/api/herd",          HTTP_GET,    [this]{ handleHerdById(); });
        server.on("/api/herd",          HTTP_POST,   [this]{ handleUpdateHerd(); });
        server.on("/api/herd",          HTTP_DELETE, [this]{ handleDeleteHerd(); });

        server.begin();
        Serial.println("[WiFi] Web server started");
    }

    void handle() {
        server.handleClient();
    }

    // ---- CORS + JSON helpers ----
    void sendJSON(int code, const String& json) {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(code, "application/json", json);
    }

    void sendOK() { sendJSON(200, "{\"ok\":true}"); }
    void sendError(const char* msg) {
        String j = "{\"error\":\"";
        j += msg;
        j += "\"}";
        sendJSON(400, j);
    }

    // ============================================================
    //  Root — full single-page mobile web app
    // ============================================================
    void handleRoot() {
        // Served as one large inline string. Split across progmem chunks
        // to avoid stack overflow on large literals.
        String html = F("<!DOCTYPE html><html lang='en'><head>"
            "<meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'>"
            "<title>FeedMix Pro</title>"
            "<style>");
        html += CSS();
        html += F("</style></head><body>"
            "<div id='app'>"
              "<header id='app-header'>"
                "<div class='hdr-mob-wrap'>"
                  "<button class='hdr-arrow' onclick='cycleHerd(-1)'>&#x2039;</button>"
                  "<div class='logo' id='hdr-mob'>&#x1F404; --</div>"
                  "<button class='hdr-arrow' onclick='cycleHerd(1)'>&#x203A;</button>"
                "</div>"
                "<div id='scale-badge' class='badge badge-ok'>Scale OK</div>"
              "</header>"
              "<nav>"
                "<button class='tab active' data-tab='dashboard' onclick='showTab(\"dashboard\")'>Dashboard</button>"
                "<button class='tab' data-tab='loading' onclick='showTab(\"loading\")'>Loading</button>"
                "<button class='tab' data-tab='feedout' onclick='showTab(\"feedout\")'>Feedout</button>"
              "</nav>"
              "<div id='swipe-area'>");

        // Dashboard tab
        html += F("<div id='tab-dashboard' class='tab-content active'>"
            "<button class='settings-btn' onclick='openSettings()'>&#9881; Settings &mdash; Herds &amp; Ingredients</button>"
            "<div class='card'>"
              "<div class='card-title'>Live Scale</div>"
              "<div class='big-num' id='scale-kg'>--</div>"
              "<div class='sub'>kg (wet weight on wagon)</div>"
              "<button class='btn btn-warn' onclick='tare()'>&#x2296; Tare Scale</button>"
            "</div>"
            "<div class='card' id='herd-summary'>"
              "<div class='card-title'>Active Herd</div>"
              "<select id='dash-herd-select' onchange='onDashHerdChange()' style='margin-bottom:10px'></select>"
              "<div class='cows-ctrl'>"
                "<span class='cows-label'>Head count</span>"
                "<button class='step-btn' onclick='changeCows(-10)'>&minus;10</button>"
                "<button class='step-btn' onclick='changeCows(-1)'>&minus;</button>"
                "<span class='cows-val' id='dash-cows'>--</span>"
                "<button class='step-btn' onclick='changeCows(1)'>+</button>"
                "<button class='step-btn' onclick='changeCows(10)'>+10</button>"
              "</div>"
              "<table class='info-table' id='dash-table'></table>"
            "</div>"
            "<div class='card'>"
              "<div class='card-title'>This Load Totals</div>"
              "<div class='metric-row'>"
                "<div class='metric'><div class='metric-val' id='dash-dm'>--</div><div class='metric-lbl'>kg DM</div></div>"
                "<div class='metric'><div class='metric-val green' id='dash-wet'>--</div><div class='metric-lbl'>kg wet (as-fed)</div></div>"
              "</div>"
            "</div>"
          "</div>");

        // (Herds & Ingredients now live inside the Settings overlay, built below)

        // Loading tab
        html += F("<div id='tab-loading' class='tab-content'>"
            "<div class='card' id='total-load-card'>"
              "<div class='card-title'>Total Load &mdash; live at scale</div>"
              "<div class='total-num'><span id='load-total-kg'>0</span>"
                "<span class='total-unit'>/ <span id='load-total-target'>0</span> kg</span></div>"
              "<div class='prog-wrap'>"
                "<div class='prog-bg' style='height:16px'>"
                  "<div class='prog-fill' id='load-total-bar' style='width:0%;height:16px'></div>"
                "</div>"
              "</div>"
              "<div class='total-pct' id='load-total-pct'>0%</div>"
            "</div>"
            "<div class='card'>"
              "<div class='card-title'>Add feed product to load</div>"
              "<div id='add-ing-buttons' class='feed-btn-grid'></div>"
            "</div>"
            "<div class='card'>"
              "<div class='card-title'>Combine another mob's mix</div>"
              "<div id='combine-mob-buttons' class='feed-btn-grid'></div>"
              "<div class='sub' id='combine-hint'></div>"
            "</div>"
            "<div id='load-controls'></div>"
            "<div id='load-status'></div>"
          "</div>");

        // Feedout tab
        html += F("<div id='tab-feedout' class='tab-content'>"
            "<div class='card' id='feedout-card'>"
              "<div class='card-title'>Feeding Out &mdash; live</div>"
              "<div class='total-num'><span id='fo-remaining'>0</span>"
                "<span class='total-unit'>kg left on wagon</span></div>"
              "<div class='prog-wrap'>"
                "<div class='prog-bg' style='height:16px'>"
                  "<div class='prog-fill' id='fo-bar' style='width:0%;height:16px'></div>"
                "</div>"
              "</div>"
              "<div class='total-pct' id='fo-pct'>0% fed out</div>"
              "<div class='metric-row' style='margin-top:12px'>"
                "<div class='metric'><div class='metric-val green' id='fo-dispensed'>0</div><div class='metric-lbl'>kg fed out</div></div>"
              "</div>"
            "</div>"
            // Lane setup
            "<div class='card' id='lane-setup-card'>"
              "<div class='card-title'>Feed lanes</div>"
              "<div class='lane-count' id='lane-count'></div>"
              "<div class='btn-row' style='margin-top:8px'>"
                "<button class='btn btn-ghost' id='mode-even' onclick='setLaneMode(\"even\")'>Even spread</button>"
                "<button class='btn btn-ghost' id='mode-var' onclick='setLaneMode(\"variable\")'>Variable</button>"
              "</div>"
              "<div id='lane-pct-inputs'></div>"
            "</div>"
            // Live lane bars
            "<div class='card' id='lane-bars-card' style='display:none'>"
              "<div class='card-title'>Lane progress</div>"
              "<div id='lane-bars'></div>"
            "</div>"
            "<div id='feedout-controls'></div>"
          "</div>");

        html += F("</div>"); // #swipe-area
        html += F("</div>"); // #app

        // Settings overlay (Herds + Ingredients)
        html += F("<div id='settings' class='settings hidden'>"
            "<div class='settings-header'>"
              "<button class='modal-close' onclick='closeSettings()'>&#x2190;</button>"
              "<span>Settings</span><span></span>"
            "</div>"
            "<div class='settings-subnav'>"
              "<button class='subtab active' id='st-herds' onclick='showSetting(\"herds\")'>Herds</button>"
              "<button class='subtab' id='st-ings' onclick='showSetting(\"ings\")'>Ingredients</button>"
              "<button class='subtab' id='st-log' onclick='showSetting(\"log\")'>Log</button>"
            "</div>"
            "<div id='set-herds' class='set-pane active'>"
              "<label class='check-row'><input type='checkbox' id='set-allow-combine' onchange='toggleCombineSetting()'> Allow combining loads (feed two mobs from one batch)</label>"
              "<div class='toolbar'>"
                "<span class='toolbar-title'>Herds / Mobs</span>"
                "<button class='btn btn-sm' onclick='openNewHerdModal()'>+ New</button>"
              "</div>"
              "<div id='herds-list'></div>"
            "</div>"
            "<div id='set-ings' class='set-pane'>"
              "<div class='info-box'>Edit dry matter % for each ingredient. "
              "<b>Wet = DM kg &divide; (DM% / 100)</b></div>"
              "<div id='ing-list'></div>"
              "<button class='btn btn-block' onclick='saveIngredients()'>&#x2714; Save DM% Changes</button>"
            "</div>"
            "<div id='set-log' class='set-pane'>"
              "<div class='toolbar'><span class='toolbar-title'>Feed Log</span></div>"
              "<div class='card'>"
                "<div class='form-group'><label>From</label><input type='date' id='log-from'></div>"
                "<div class='form-group'><label>To</label><input type='date' id='log-to'></div>"
                "<div class='btn-row'>"
                  "<button class='btn btn-primary' onclick='queryLog()'>Show</button>"
                  "<button class='btn btn-ghost' onclick='logRange(7)'>7 days</button>"
                  "<button class='btn btn-ghost' onclick='logRange(30)'>30 days</button>"
                "</div>"
              "</div>"
              "<div id='log-results'></div>"
              "<button class='btn btn-danger btn-block' onclick='clearLog()' style='margin-top:10px'>Clear Log</button>"
            "</div>"
          "</div>");

        // Modals
        html += F("<div id='modal-overlay' class='modal-overlay hidden' onclick='closeModal()'></div>"
          "<div id='modal' class='modal hidden'>"
            "<div class='modal-header'><span id='modal-title'>Edit Herd</span>"
              "<button class='modal-close' onclick='closeModal()'>&#x2715;</button></div>"
            "<div class='modal-body' id='modal-body'></div>"
          "</div>");

        html += F("<script>");
        html += JS();
        html += F("</script></body></html>");

        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "text/html", html);
    }

    // ============================================================
    //  CSS
    // ============================================================
    String CSS() {
        return F(
            "*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}"
            "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
                 "background:#f0f2f0;color:#1a2e1a;font-size:16px}"
            "#app{max-width:520px;margin:0 auto;min-height:100vh}"
            "header{background:#1a5c2e;color:#fff;padding:14px 16px;"
                   "display:flex;justify-content:space-between;align-items:center}"
            ".logo{font-size:1.1rem;font-weight:600}"
            ".hdr-mob-wrap{display:flex;align-items:center;gap:6px;touch-action:pan-y}"
            ".hdr-arrow{background:rgba(255,255,255,.15);border:none;color:#fff;width:30px;height:30px;"
                       "border-radius:8px;font-size:20px;line-height:1;cursor:pointer;padding:0}"
            ".hdr-arrow:active{background:rgba(255,255,255,.35)}"
            "#app-header{user-select:none}"
            ".badge{padding:4px 10px;border-radius:20px;font-size:12px;font-weight:600}"
            ".badge-ok{background:#2d8a4e;color:#fff}"
            ".badge-err{background:#c0392b;color:#fff}"
            "nav{display:flex;background:#fff;border-bottom:2px solid #e0e0e0;position:sticky;top:0;z-index:10}"
            ".tab{flex:1;padding:12px 4px;border:none;background:transparent;font-size:13px;"
                 "font-weight:500;color:#666;cursor:pointer;border-bottom:3px solid transparent;"
                 "transition:.2s}"
            ".tab.active{color:#1a5c2e;border-bottom:3px solid #1a5c2e}"
            ".tab-content{display:none;padding:12px}"
            ".tab-content.active{display:block}"
            "#swipe-area{touch-action:pan-y}"
            // settings entry button on dashboard
            ".settings-btn{width:100%;padding:11px;margin-bottom:12px;background:#fff;"
                          "border:1px solid #1a5c2e;color:#1a5c2e;border-radius:10px;"
                          "font-size:14px;font-weight:600;cursor:pointer}"
            ".settings-btn:active{background:#1a5c2e;color:#fff}"
            // settings overlay
            ".settings{position:fixed;inset:0;background:#f0f2f0;z-index:150;overflow-y:auto;"
                      "max-width:520px;margin:0 auto}"
            ".settings-header{display:flex;align-items:center;justify-content:space-between;"
                            "background:#1a5c2e;color:#fff;padding:14px 16px;font-size:1.1rem;font-weight:600}"
            ".settings-header .modal-close{color:#fff;font-size:24px}"
            ".settings-subnav{display:flex;background:#fff;border-bottom:2px solid #e0e0e0;"
                            "position:sticky;top:0;z-index:5}"
            ".subtab{flex:1;padding:12px;border:none;background:transparent;font-size:14px;"
                    "font-weight:500;color:#666;cursor:pointer;border-bottom:3px solid transparent}"
            ".subtab.active{color:#1a5c2e;border-bottom:3px solid #1a5c2e}"
            ".set-pane{display:none;padding:12px}"
            ".set-pane.active{display:block}"
            ".check-row{display:flex;align-items:center;gap:10px;background:#fff;padding:12px;"
                      "border-radius:10px;margin-bottom:12px;font-size:14px;font-weight:500;cursor:pointer}"
            ".check-row input{width:20px;height:20px}"
            // feedout
            "#feedout-card{background:#1a3a52;color:#fff}"
            "#feedout-card .card-title{color:#9fc4dd}"
            "#feedout-card .prog-bg{background:rgba(255,255,255,.15)}"
            "#feedout-card .prog-fill{background:#3aa0e0}"
            ".lane-count{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}"
            ".lane-num-btn{padding:12px;border:1px solid #1a5c2e;background:#fff;color:#1a5c2e;"
                         "border-radius:8px;font-size:16px;font-weight:700;cursor:pointer}"
            ".lane-num-btn.active{background:#1a5c2e;color:#fff}"
            "#mode-even.active,#mode-var.active{background:#1a5c2e;color:#fff;border-color:#1a5c2e}"
            ".lane-pct-row{display:flex;align-items:center;gap:8px;margin-top:8px}"
            ".lane-pct-row label{flex:1;font-size:13px;margin:0}"
            ".lane-pct-row input{width:90px}"
            ".lane-bar{margin-bottom:14px}"
            ".lane-bar-head{display:flex;justify-content:space-between;font-size:13px;font-weight:600;margin-bottom:4px}"
            ".lane-bar.active .lane-bar-head{color:#e67e22}"
            ".lane-bar.done{opacity:.55}"
            ".lane-bar .prog-bg{height:18px;background:#e8f0e9}"
            ".lane-bar .prog-fill{height:18px;background:#3aa0e0}"
            ".lane-bar.active .prog-fill{background:#e67e22}"
            ".lane-bar.done .prog-fill{background:#2d8a4e}"
            ".card{background:#fff;border-radius:12px;padding:16px;margin-bottom:12px;"
                  "box-shadow:0 1px 4px rgba(0,0,0,.08)}"
            ".card-title{font-size:12px;font-weight:600;color:#666;text-transform:uppercase;"
                        "letter-spacing:.5px;margin-bottom:10px}"
            ".big-num{font-size:2.4rem;font-weight:700;color:#1a2e1a;line-height:1}"
            ".big-num.green{color:#1a5c2e}"
            ".sub{font-size:13px;color:#888;margin-top:4px;margin-bottom:12px}"
            ".metric-row{display:flex;gap:12px}"
            ".metric{flex:1;background:#f5f7f5;border-radius:8px;padding:12px;text-align:center}"
            ".metric-val{font-size:1.6rem;font-weight:700;color:#1a2e1a}"
            ".metric-val.green{color:#1a5c2e}"
            ".metric-lbl{font-size:11px;color:#888;margin-top:2px}"
            ".btn{display:inline-block;padding:10px 18px;border-radius:8px;border:none;"
                 "font-size:14px;font-weight:600;cursor:pointer;transition:.15s}"
            ".btn:active{transform:scale(.97)}"
            ".btn-primary{background:#1a5c2e;color:#fff}"
            ".btn-warn{background:#e67e22;color:#fff}"
            ".btn-danger{background:#c0392b;color:#fff}"
            ".btn-ghost{background:#f0f2f0;color:#1a2e1a;border:1px solid #ccc}"
            ".btn-sm{padding:7px 14px;font-size:13px}"
            ".btn-block{width:100%;margin-top:8px;background:#1a5c2e;color:#fff;"
                       "padding:13px;border-radius:10px;border:none;font-size:15px;font-weight:600;cursor:pointer}"
            ".toolbar{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}"
            ".toolbar-title{font-size:1rem;font-weight:600}"
            ".herd-card{background:#fff;border-radius:12px;padding:14px;margin-bottom:10px;"
                       "box-shadow:0 1px 4px rgba(0,0,0,.08)}"
            ".herd-name{font-size:1.1rem;font-weight:700;color:#1a2e1a}"
            ".herd-meta{font-size:13px;color:#666;margin:4px 0 10px}"
            ".herd-actions{display:flex;gap:8px;flex-wrap:wrap}"
            ".info-table{width:100%;border-collapse:collapse;font-size:14px;margin-top:8px}"
            ".info-table th{text-align:left;color:#888;font-weight:500;padding:4px 0;font-size:12px}"
            ".info-table td{padding:5px 0;border-top:1px solid #f0f0f0}"
            ".info-table td:last-child{text-align:right;font-weight:600;color:#1a5c2e}"
            ".prog-wrap{margin:8px 0}"
            ".prog-bg{background:#e8f5e9;border-radius:6px;height:10px}"
            ".prog-fill{background:#1a5c2e;height:10px;border-radius:6px;transition:.3s}"
            ".prog-fill.warn{background:#e67e22}"
            ".prog-fill.danger{background:#c0392b}"
            ".ing-row{display:flex;align-items:center;gap:10px;padding:10px 0;"
                     "border-bottom:1px solid #f0f0f0}"
            ".ing-row:last-child{border-bottom:none}"
            ".ing-name{flex:1;font-size:14px;font-weight:500}"
            ".ing-dm-input{width:70px;padding:6px 8px;border:1px solid #ccc;"
                          "border-radius:6px;font-size:14px;text-align:center}"
            ".ing-wet{font-size:12px;color:#1a5c2e;width:80px;text-align:right;font-weight:600}"
            ".info-box{background:#e8f5e9;border-left:3px solid #1a5c2e;padding:10px 12px;"
                      "border-radius:0 8px 8px 0;font-size:13px;margin-bottom:12px;color:#1a3020}"
            ".modal-overlay{position:fixed;inset:0;background:rgba(0,0,0,.5);z-index:200}"
            ".modal{position:fixed;bottom:0;left:0;right:0;max-width:520px;margin:0 auto;"
                   "background:#fff;border-radius:16px 16px 0 0;z-index:201;max-height:90vh;overflow-y:auto}"
            ".modal-header{display:flex;justify-content:space-between;align-items:center;"
                          "padding:16px;border-bottom:1px solid #eee}"
            ".modal-close{background:none;border:none;font-size:20px;cursor:pointer;color:#666}"
            ".modal-body{padding:16px}"
            ".hidden{display:none!important}"
            ".form-group{margin-bottom:14px}"
            "label{display:block;font-size:13px;font-weight:600;color:#444;margin-bottom:5px}"
            "input[type=text],input[type=number],select{"
                "width:100%;padding:10px 12px;border:1px solid #ccc;border-radius:8px;"
                "font-size:15px;background:#fff}"
            ".ration-row{display:flex;align-items:center;gap:8px;padding:8px 0;"
                        "border-bottom:1px solid #f0f0f0}"
            ".ration-row:last-child{border-bottom:none}"
            ".ration-name{flex:1;font-size:13px}"
            ".ration-input{width:75px;padding:6px;border:1px solid #ccc;border-radius:6px;"
                          "font-size:13px;text-align:center}"
            ".ration-wet{font-size:12px;color:#1a5c2e;width:70px;text-align:right}"
            ".muted{color:#888;font-size:14px}"
            ".step-card{background:#f9f9f9;border-radius:8px;padding:12px;margin-bottom:8px;"
                       "border-left:4px solid #ccc}"
            ".step-card.active{border-left-color:#1a5c2e;background:#f0faf4}"
            ".step-card.done{border-left-color:#2d8a4e;opacity:.7}"
            ".step-card.over{border-left-color:#c0392b;background:#fff5f5}"
            "h3{font-size:14px;font-weight:700;margin-bottom:6px}"
            ".toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);"
                   "background:#1a5c2e;color:#fff;padding:10px 20px;border-radius:20px;"
                   "font-size:14px;font-weight:600;z-index:300;opacity:0;transition:.3s}"
            ".toast.show{opacity:1}"
            // headcount stepper
            ".cows-ctrl{display:flex;align-items:center;gap:6px;flex-wrap:wrap;margin-bottom:6px}"
            ".cows-label{font-size:13px;color:#666;flex-basis:100%;margin-bottom:2px}"
            ".step-btn{min-width:42px;padding:8px 6px;border:1px solid #1a5c2e;background:#fff;"
                      "color:#1a5c2e;border-radius:8px;font-size:15px;font-weight:700;cursor:pointer}"
            ".step-btn:active{background:#1a5c2e;color:#fff}"
            ".cows-val{font-size:1.5rem;font-weight:700;min-width:60px;text-align:center;color:#1a2e1a}"
            "#dash-herd-select{width:100%;font-size:1.05rem;font-weight:600;color:#1a5c2e}"
            // total load card
            "#total-load-card{background:#0f3d1e;color:#fff}"
            "#total-load-card .card-title{color:#9fd4b4}"
            ".total-num{font-size:2.6rem;font-weight:700;line-height:1}"
            ".total-unit{font-size:1rem;font-weight:400;opacity:.7}"
            ".total-pct{font-size:1.3rem;font-weight:700;color:#7ee29a;text-align:right;margin-top:4px}"
            "#total-load-card .prog-bg{background:rgba(255,255,255,.15)}"
            "#total-load-card .prog-fill{background:#2ecc71}"
            // activate buttons
            ".btn-activate{width:100%;padding:11px;margin-top:8px;background:#1a5c2e;color:#fff;"
                          "border:none;border-radius:8px;font-size:14px;font-weight:600;cursor:pointer}"
            ".btn-activate.active{background:#e67e22}"
            ".btn-activate:disabled{background:#bbb;cursor:default}"
            ".btn-row{display:flex;gap:8px;margin-top:8px}"
            ".btn-row .btn{flex:1}"
            ".feed-btn-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}"
            ".feed-btn{display:flex;flex-direction:column;align-items:flex-start;gap:2px;"
                      "padding:12px;border:1px solid #1a5c2e;background:#fff;color:#1a2e1a;"
                      "border-radius:10px;cursor:pointer;text-align:left}"
            ".feed-btn:active{background:#1a5c2e;color:#fff}"
            ".feed-btn:active .feed-btn-kg{color:#cdeccf}"
            ".feed-btn-name{font-size:14px;font-weight:600}"
            ".feed-btn-kg{font-size:13px;font-weight:700;color:#1a5c2e}"
            ".feed-btn-state{font-size:11px;color:#888;margin-top:2px}"
            ".feed-btn.filling{background:#e67e22;border-color:#e67e22;color:#fff}"
            ".feed-btn.filling .feed-btn-kg{color:#fff}"
            ".feed-btn.filling .feed-btn-state{color:#fff}"
            ".feed-btn.inload{border-color:#888;background:#f7f7f5}"
            ".feed-btn.done{border-color:#2d8a4e;background:#f0faf4}"
            ".feed-btn.done .feed-btn-state{color:#2d8a4e}"
            ".mob-subhead{font-size:13px;font-weight:700;color:#1a5c2e;margin:10px 0 6px;"
                        "padding-bottom:4px;border-bottom:1px solid #cde3d2}"
        );
    }

    // ============================================================
    //  JavaScript — full SPA logic
    // ============================================================
    String JS() {
        return F(
            "var herds=[],ings=[],activeHerd=0,scaleKg=0,ingEdits={};"
            "var pollInt=null,cowsEditing=false,cowsTimer=null;"
            "var TABS=['dashboard','loading','feedout'];"
            "var curTab='dashboard';"
            // feedout session state (client-side)
            "var foActive=false,foStartKg=0,foFeeds=[],foLogged=false,foMobs=[];"
            "var laneCount=1,laneMode='even',lanePct=[100],laneTargets=[];"

            // ---- Tab navigation ----
            "function showTab(t){"
              "curTab=t;"
              "document.querySelectorAll('.tab-content').forEach(e=>e.classList.remove('active'));"
              "document.querySelectorAll('.tab').forEach(e=>e.classList.remove('active'));"
              "var pane=document.getElementById('tab-'+t);if(pane)pane.classList.add('active');"
              "document.querySelectorAll('.tab').forEach(e=>{"
                "if(e.getAttribute('data-tab')===t)e.classList.add('active');"
              "});"
              "if(t==='loading')renderLoading();"
              "if(t==='feedout')renderFeedout();"
            "}"

            // ---- Swipe between tabs ----
            "function initSwipe(){"
              "var area=document.getElementById('swipe-area');if(!area)return;"
              "var x0=0,y0=0,tracking=false;"
              "area.addEventListener('touchstart',function(e){"
                "if(e.touches.length!==1)return;"
                "x0=e.touches[0].clientX;y0=e.touches[0].clientY;tracking=true;"
              "},{passive:true});"
              "area.addEventListener('touchend',function(e){"
                "if(!tracking)return;tracking=false;"
                "var dx=e.changedTouches[0].clientX-x0;"
                "var dy=e.changedTouches[0].clientY-y0;"
                "if(Math.abs(dx)<60||Math.abs(dx)<Math.abs(dy))return;"  // mostly-horizontal only
                "var i=TABS.indexOf(curTab);"
                "if(dx<0&&i<TABS.length-1)showTab(TABS[i+1]);"
                "else if(dx>0&&i>0)showTab(TABS[i-1]);"
              "},{passive:true});"
            "}"

            // ---- Settings overlay ----
            "function openSettings(){"
              "document.getElementById('settings').classList.remove('hidden');"
              "var cb=document.getElementById('set-allow-combine');"
              "if(cb)cb.checked=allowCombine;"
              "renderHerds();renderIngredients();"
            "}"
            "async function toggleCombineSetting(){"
              "var cb=document.getElementById('set-allow-combine');"
              "allowCombine=cb.checked;"
              "await api('/api/settings?allow_combine='+(allowCombine?1:0),'POST');"
              "showToast('Combining '+(allowCombine?'enabled':'disabled'));"
            "}"
            "function closeSettings(){"
              "document.getElementById('settings').classList.add('hidden');"
              "renderDashHerd();"
            "}"
            "function showSetting(which){"
              "document.getElementById('set-herds').classList.toggle('active',which==='herds');"
              "document.getElementById('set-ings').classList.toggle('active',which==='ings');"
              "document.getElementById('set-log').classList.toggle('active',which==='log');"
              "document.getElementById('st-herds').classList.toggle('active',which==='herds');"
              "document.getElementById('st-ings').classList.toggle('active',which==='ings');"
              "document.getElementById('st-log').classList.toggle('active',which==='log');"
              "if(which==='log'){if(!document.getElementById('log-from').value)logRange(7);else queryLog();}"
            "}"

            // ---- Feed log ----
            "function ymd(d){return d.getFullYear()*10000+(d.getMonth()+1)*100+d.getDate();}"
            "function isoDate(d){return d.toISOString().slice(0,10);}"
            "function logRange(days){"
              "var to=new Date();var from=new Date();from.setDate(from.getDate()-(days-1));"
              "document.getElementById('log-from').value=isoDate(from);"
              "document.getElementById('log-to').value=isoDate(to);"
              "queryLog();"
            "}"
            "async function queryLog(){"
              "var fEl=document.getElementById('log-from'),tEl=document.getElementById('log-to');"
              "if(!fEl.value||!tEl.value){logRange(7);return;}"
              "var from=parseInt(fEl.value.replace(/-/g,''));"
              "var to=parseInt(tEl.value.replace(/-/g,''));"
              "var d=await api('/api/log/query?from='+from+'&to='+to);"
              "var c=document.getElementById('log-results');if(!c)return;"
              "if(!d){c.innerHTML='<p class=muted>Could not load log.</p>';return;}"
              // per-mob rows
              "var mobRows=(d.mobs||[]).sort(function(a,b){return b.kg-a.kg;}).map(function(m){"
                "return '<tr><td>'+m.name+'</td><td>'+m.kg.toFixed(0)+' kg</td></tr>';"
              "}).join('');"
              // per-feed rows with over-target highlight
              "var feedRows=(d.feeds||[]).sort(function(a,b){return b.fed-a.fed;}).map(function(fe){"
                "var over=fe.over||0;"
                "var overTxt=over>0.5?'<span style=\"color:#c0392b;font-weight:700\">+'+over.toFixed(0)+'</span>':"
                  "(over<-0.5?'<span style=\"color:#e67e22\">'+over.toFixed(0)+'</span>':'<span style=\"color:#2d8a4e\">on target</span>');"
                "return '<tr><td>'+fe.name+'</td><td>'+fe.fed.toFixed(0)+' kg</td>'"
                  "+'<td>'+fe.target.toFixed(0)+' kg</td><td>'+overTxt+'</td></tr>';"
              "}).join('');"
              "c.innerHTML='<div class=card>'"
                "+'<div class=card-title>Total fed ('+d.entries+' feed records)</div>'"
                "+'<div class=\"big-num green\">'+(d.total_kg||0).toFixed(0)+' kg</div>'"
                "+(mobRows?'<div class=card-title style=margin-top:12px>By mob</div>'"
                  "+'<table class=info-table><tr><th>Mob</th><th>Fed</th></tr>'+mobRows+'</table>':'')"
                "+(feedRows?'<div class=card-title style=margin-top:12px>By feed product</div>'"
                  "+'<table class=info-table><tr><th>Feed</th><th>Fed</th><th>Target</th><th>Over</th></tr>'+feedRows+'</table>':"
                  "'<p class=muted style=margin-top:8px>No feedout records in this range.</p>')"
                "+'</div>';"
            "}"
            "async function clearLog(){"
              "if(!confirm('Clear all feed log records? This cannot be undone.'))return;"
              "await api('/api/log/clear','POST');"
              "queryLog();showToast('Log cleared');"
            "}"
            // log a completed feedout with per-feed amounts (loaded vs target).
            // 'feeds' = [{name,fed,target}]; over-target shows when fed>target.
            "async function logFeedout(feeds,fallbackKg){"
              "var mob=(herds[activeHerd]?herds[activeHerd].name:'Unknown');"
              "var body={date:ymd(new Date()),mob:mob};"
              "if(feeds&&feeds.length)body.feeds=feeds;"
              "else if(fallbackKg>0)body.kg=Math.round(fallbackKg);"
              "else return;"
              "await api('/api/log/add','POST',body);"
            "}"
            // snapshot the load's per-feed loaded/target amounts for logging
            "function captureLoadFeeds(){"
              "return (loadSteps||[]).map(function(s){"
                "return {name:(ings[s.ingredient_idx]?ings[s.ingredient_idx].name:'Feed'),"
                  "fed:Math.round(s.loaded_wet_kg),target:Math.round(s.target_wet_kg)};"
              "}).filter(function(x){return x.fed>0;});"
            "}"
            // group the load by mob (for combined-mix feedout bars), preserving
            // the order mobs first appear in the load
            "function buildFoMobs(){"
              "var order=[],map={};"
              "(loadSteps||[]).forEach(function(s){"
                "var hi=s.herd_idx;"
                "if(map[hi]===undefined){map[hi]={herd:hi,kg:0};order.push(hi);}"
                "map[hi].kg+=s.target_wet_kg;"
              "});"
              "return order.map(function(hi){"
                "return {herd:hi,name:(herds[hi]?herds[hi].name:('Mob '+hi)),kg:map[hi].kg};"
              "});"
            "}"

            // ---- API helpers ----
            "async function api(path,method,body){"
              "try{"
                "var r=await fetch(path,{method:method||'GET',"
                  "headers:body?{'Content-Type':'application/json'}:{},"
                  "body:body?JSON.stringify(body):undefined});"
                "return await r.json();"
              "}catch(e){showToast('Network error','err');return null;}"
            "}"

            // ---- Poll status every 1s ----
            "async function pollStatus(){"
              "var d=await api('/api/status');"
              "if(!d)return;"
              "scaleKg=d.scale_kg;"
              "activeHerd=d.active_herd;"
              "var el=document.getElementById('scale-kg');"
              "if(el)el.textContent=scaleKg.toFixed(0)+' kg';"
              "var badge=document.getElementById('scale-badge');"
              "if(badge){"
                "badge.textContent=d.scale_ok?'Scale OK':'NO SCALE';"
                "badge.className='badge '+(d.scale_ok?'badge-ok':'badge-err');"
              "}"
              // header shows the active mob name
              "var hm=document.getElementById('hdr-mob');"
              "if(hm&&herds[activeHerd])hm.innerHTML='&#x1F404; '+herds[activeHerd].name;"
              "renderDashHerd();"
              "renderLoadingLive(d);"
              "updateFeedout(d);"
            "}"

            // ---- Load all data ----
            "async function loadAll(){"
              "var h=await api('/api/herds');"
              "if(h)herds=h.herds||[];"
              "var i=await api('/api/ingredients');"
              "if(i)ings=i.ingredients||[];"
              "renderHerds();"
              "renderIngredients();"
              "renderDashHerd();"
            "}"

            // ---- Dashboard herd summary ----
            "function renderDashHerd(){"
              "if(!herds.length)return;"
              "var h=herds[activeHerd]||herds[0];"
              // populate herd selector (only rebuild if option count changed)
              "var sel=document.getElementById('dash-herd-select');"
              "if(sel){"
                "if(sel.options.length!==herds.length){"
                  "sel.innerHTML=herds.map(function(hh,i){"
                    "return '<option value='+i+'>'+hh.name+'</option>';}).join('');"
                "}"
                "sel.value=activeHerd;"
              "}"
              "var cv=document.getElementById('dash-cows');"
              "if(cv&&!cowsEditing)cv.textContent=h.num_cows;"
              "var totalDM=0,totalWet=0;"
              "var rows='';"
              "(h.entries||[]).forEach(function(e){"
                "var ing=ings[e.ingredient_idx]||{name:'?',dm_pct:100};"
                "var dm=e.kg_dm_per_cow*h.num_cows/h.meals_per_day;"
                "var wet=dm/(ing.dm_pct/100);"
                "totalDM+=dm;totalWet+=wet;"
                "rows+='<tr><td>'+ing.name+'</td><td>'+e.kg_dm_per_cow.toFixed(2)+"
                       "' kgDM/cow</td><td>'+wet.toFixed(0)+' kg wet</td></tr>';"
              "});"
              "var t=document.getElementById('dash-table');"
              "if(t)t.innerHTML='<tr><th>Ingredient</th><th>kgDM/cow</th><th>Wet load</th></tr>'+rows;"
              "var dm=document.getElementById('dash-dm');"
              "if(dm)dm.textContent=totalDM.toFixed(0);"
              "var wt=document.getElementById('dash-wet');"
              "if(wt)wt.textContent=totalWet.toFixed(0);"
            "}"

            // ---- Change herd from dashboard ----
            // shared: make herd i active (used by dropdown, header arrows/swipe).
            // If a mix is already loaded, require confirmation before switching.
            "async function setActiveHerdIdx(i){"
              "if(i<0||i>=herds.length||i===activeHerd)return;"
              "if(loadActive){"
                "if(!confirm('A mix is loaded for '+herds[activeHerd].name+'. Switch to '+herds[i].name+' and start a new load?'))return;"
              "}"
              "await api('/api/herd?id='+i,'POST',{set_active:true});"
              "activeHerd=i;"
              "var hm=document.getElementById('hdr-mob');"
              "if(hm&&herds[i])hm.innerHTML='&#x1F404; '+herds[i].name;"
              // rebuild the load for the new mob so its feeds show immediately
              "await api('/api/load/start','POST');"
              "feedBtnSig='';combineSig='';"
              "renderHerds();renderDashHerd();"
              "showToast('Loading for: '+herds[i].name);"
            "}"
            "async function onDashHerdChange(){"
              "var sel=document.getElementById('dash-herd-select');"
              "await setActiveHerdIdx(parseInt(sel.value));"
            "}"
            // cycle herd by dir (-1/+1) with wrap-around
            "async function cycleHerd(dir){"
              "if(!herds.length)return;"
              "var i=(activeHerd+dir+herds.length)%herds.length;"
              "await setActiveHerdIdx(i);"
            "}"
            // swipe the top header to change the mob being loaded for
            "function initHeaderSwipe(){"
              "var hd=document.getElementById('app-header');if(!hd)return;"
              "var x0=0,y0=0,tracking=false;"
              "hd.addEventListener('touchstart',function(e){"
                "if(e.touches.length!==1)return;"
                "x0=e.touches[0].clientX;y0=e.touches[0].clientY;tracking=true;"
              "},{passive:true});"
              "hd.addEventListener('touchend',function(e){"
                "if(!tracking)return;tracking=false;"
                "var dx=e.changedTouches[0].clientX-x0;"
                "var dy=e.changedTouches[0].clientY-y0;"
                "if(Math.abs(dx)<50||Math.abs(dx)<Math.abs(dy))return;"
                "cycleHerd(dx<0?1:-1);"  // swipe left = next mob
              "},{passive:true});"
            "}"

            // ---- Change head count from dashboard ----
            "async function changeCows(delta){"
              "var h=herds[activeHerd];if(!h)return;"
              "cowsEditing=true;"
              "h.num_cows=Math.max(1,Math.min(9999,h.num_cows+delta));"
              "var cv=document.getElementById('dash-cows');"
              "if(cv)cv.textContent=h.num_cows;"
              "renderDashHerd();"
              // debounce the save so rapid taps send one request
              "clearTimeout(cowsTimer);"
              "cowsTimer=setTimeout(function(){"
                "api('/api/cows?id='+activeHerd+'&cows='+h.num_cows,'POST');"
                "showToast('Head count: '+h.num_cows);"
                "cowsEditing=false;"
              "},600);"
            "}"

            // ---- Herds list ----
            "function renderHerds(){"
              "var c=document.getElementById('herds-list');if(!c)return;"
              "if(!herds.length){c.innerHTML='<p class=muted>No herds yet.</p>';return;}"
              "c.innerHTML=herds.map(function(h,i){"
                "var dm=h.entries?h.entries.reduce(function(s,e){return s+e.kg_dm_per_cow;},0):0;"
                "return '<div class=herd-card>'"
                  "+'<div class=herd-name>'+h.name+(i===activeHerd?' <span style=color:#1a5c2e>&#10003; Active</span>':'')+'</div>'"
                  "+'<div class=herd-meta>'+h.num_cows+' cows &bull; '+h.meals_per_day+' meals/day &bull; '+dm.toFixed(2)+' kgDM/cow</div>'"
                  "+'<div class=herd-actions>'"
                    "+'<button class=\"btn btn-ghost btn-sm\" onclick=\"editHerd('+i+')\">&#9998; Edit</button>'"
                    "+'<button class=\"btn btn-primary btn-sm\" onclick=\"setActiveHerd('+i+')\">Set Active</button>'"
                    "+'<button class=\"btn btn-danger btn-sm\" onclick=\"deleteHerd('+i+')\">&#x2715; Delete</button>'"
                  "+'</div>'"
                  "+'</div>';"
              "}).join('');"
            "}"

            // ---- Set active herd ----
            "async function setActiveHerd(i){"
              "herds[i]._set_active=true;"
              "await api('/api/herd?id='+i,'POST',{set_active:true});"
              "activeHerd=i;"
              "renderHerds();renderDashHerd();"
              "showToast('Active herd: '+herds[i].name);"
            "}"

            // ---- Delete herd ----
            "async function deleteHerd(i){"
              "if(!confirm('Delete '+herds[i].name+'?'))return;"
              "await api('/api/herd?id='+i,'DELETE');"
              "herds.splice(i,1);"
              "renderHerds();renderDashHerd();"
              "showToast('Herd deleted');"
            "}"

            // ---- New herd modal ----
            "function openNewHerdModal(){"
              "var body='<div class=form-group><label>Herd name</label>'"
                "+'<input type=text id=m-name placeholder=\"e.g. Milkers A\"></div>'"
                "+'<div class=form-group><label>Number of cows</label>'"
                "+'<input type=number id=m-cows value=100 min=1 max=9999></div>'"
                "+'<div class=form-group><label>Meals per day</label>'"
                "+'<select id=m-meals><option value=1>1</option><option value=2 selected>2</option>"
                  "<option value=3>3</option><option value=4>4</option></select></div>'"
                "+'<div class=form-group><label>Ration (kgDM/cow/day per ingredient)</label>'"
                "+'<div id=m-ration>';"
              "body+=ings.map(function(ing,i){"
                "return '<div class=ration-row>'"
                  "+'<span class=ration-name>'+ing.name+'</span>'"
                  "+'<input type=number class=ration-input id=m-ing-'+i+' value=0 min=0 step=0.05>'"
                  "+'<span class=ration-wet id=m-wet-'+i+'>0 kg wet</span>'"
                  "+'</div>';"
              "}).join('');"
              "body+='</div>'"
                "+'<button class=\"btn btn-block\" onclick=\"saveNewHerd()\">&#x2714; Create Herd</button>';"
              "openModal('New Herd',body);"
              // Live wet weight preview
              "ings.forEach(function(ing,i){"
                "var inp=document.getElementById('m-ing-'+i);"
                "var wet=document.getElementById('m-wet-'+i);"
                "if(inp&&wet)inp.addEventListener('input',function(){"
                  "var dm=parseFloat(inp.value)||0;"
                  "wet.textContent=(dm/(ing.dm_pct/100)).toFixed(1)+' kg wet';"
                "});"
              "});"
            "}"

            // ---- Save new herd ----
            "async function saveNewHerd(){"
              "var name=document.getElementById('m-name').value.trim();"
              "if(!name){alert('Enter a herd name');return;}"
              "var cows=parseInt(document.getElementById('m-cows').value)||100;"
              "var meals=parseInt(document.getElementById('m-meals').value)||2;"
              "var entries=[];"
              "ings.forEach(function(ing,i){"
                "var v=parseFloat(document.getElementById('m-ing-'+i).value)||0;"
                "if(v>0)entries.push({ingredient_idx:i,kg_dm_per_cow:v});"
              "});"
              "var r=await api('/api/herds','POST',{"
                "name:name,num_cows:cows,meals_per_day:meals,entries:entries});"
              "if(r&&r.ok){var h=await api('/api/herds');if(h)herds=h.herds||[];}"
              "renderHerds();closeModal();showToast('Herd created: '+name);"
            "}"

            // ---- Edit herd modal ----
            "function editHerd(i){"
              "var h=herds[i];"
              "var body='<div class=form-group><label>Herd name</label>'"
                "+'<input type=text id=e-name value=\"'+h.name+'\"></div>'"
                "+'<div class=form-group><label>Number of cows</label>'"
                "+'<input type=number id=e-cows value='+h.num_cows+' min=1 max=9999></div>'"
                "+'<div class=form-group><label>Meals per day</label>'"
                "+'<select id=e-meals><option value=1'+(h.meals_per_day==1?' selected':'')+'>1</option>'"
                  "+'<option value=2'+(h.meals_per_day==2?' selected':'')+'>2</option>'"
                  "+'<option value=3'+(h.meals_per_day==3?' selected':'')+'>3</option>'"
                  "+'<option value=4'+(h.meals_per_day==4?' selected':'')+'>4</option></select></div>'"
                "+'<div class=form-group><label>Ration (kgDM/cow/day)</label><div id=e-ration>';"
              "body+=ings.map(function(ing,j){"
                "var entry=(h.entries||[]).find(function(e){return e.ingredient_idx===j;});"
                "var val=entry?entry.kg_dm_per_cow:0;"
                "var wet=(val/(ing.dm_pct/100)).toFixed(1);"
                "return '<div class=ration-row>'"
                  "+'<span class=ration-name>'+ing.name+'</span>'"
                  "+'<input type=number class=ration-input id=e-ing-'+j+' value='+val+' min=0 step=0.05>'"
                  "+'<span class=ration-wet id=e-wet-'+j+'>'+wet+' kg wet</span>'"
                  "+'</div>';"
              "}).join('');"
              "body+='</div>'"
                "+'<button class=\"btn btn-block\" onclick=\"saveEditHerd('+i+')\">&#x2714; Save Changes</button>';"
              "openModal('Edit: '+h.name,body);"
              "ings.forEach(function(ing,j){"
                "var inp=document.getElementById('e-ing-'+j);"
                "var wet=document.getElementById('e-wet-'+j);"
                "if(inp&&wet)inp.addEventListener('input',function(){"
                  "var dm=parseFloat(inp.value)||0;"
                  "wet.textContent=(dm/(ing.dm_pct/100)).toFixed(1)+' kg wet';"
                "});"
              "});"
            "}"

            "async function saveEditHerd(i){"
              "var name=document.getElementById('e-name').value.trim();"
              "if(!name){alert('Enter a herd name');return;}"
              "var cows=parseInt(document.getElementById('e-cows').value)||100;"
              "var meals=parseInt(document.getElementById('e-meals').value)||2;"
              "var entries=[];"
              "ings.forEach(function(ing,j){"
                "var v=parseFloat(document.getElementById('e-ing-'+j).value)||0;"
                "if(v>0)entries.push({ingredient_idx:j,kg_dm_per_cow:v});"
              "});"
              "await api('/api/herd?id='+i,'POST',{"
                "name:name,num_cows:cows,meals_per_day:meals,entries:entries});"
              "herds[i].name=name;herds[i].num_cows=cows;"
              "herds[i].meals_per_day=meals;herds[i].entries=entries;"
              "renderHerds();renderDashHerd();closeModal();"
              "showToast('Herd saved: '+name);"
            "}"

            // ---- Ingredients ----
            "function renderIngredients(){"
              "var c=document.getElementById('ing-list');if(!c)return;"
              "c.innerHTML=ings.map(function(ing,i){"
                "return '<div class=ing-row>'"
                  "+'<span class=ing-name>'+ing.name+'</span>'"
                  "+'<input type=number class=ing-dm-input id=ing-dm-'+i"
                     "+' value='+ing.dm_pct.toFixed(1)"
                     "+' min=1 max=100 step=0.5 oninput=\"updateIngWet('+i+')\">'"
                  "+'<span class=ing-wet id=ing-wet-'+i+'>1kgDM='"
                    "+(100/ing.dm_pct).toFixed(2)+'kg wet</span>'"
                  "+'</div>';"
              "}).join('');"
            "}"

            "function updateIngWet(i){"
              "var v=parseFloat(document.getElementById('ing-dm-'+i).value)||1;"
              "var wet=document.getElementById('ing-wet-'+i);"
              "if(wet)wet.textContent='1kgDM='+(100/v).toFixed(2)+'kg wet';"
            "}"

            "async function saveIngredients(){"
              "var arr=ings.map(function(ing,i){"
                "var v=parseFloat(document.getElementById('ing-dm-'+i).value)||ing.dm_pct;"
                "return {name:ing.name,dm_pct:v,active:ing.active};"
              "});"
              "var r=await api('/api/ingredients','POST',{ingredients:arr});"
              "if(r&&r.ok){ings=arr;renderIngredients();renderDashHerd();"
                "showToast('Ingredient DM% saved');}"
            "}"

            // ---- Loading tab ----
            "function renderLoading(){renderFeedButtons();renderCombineButtons();maybeAutoBuild();pollStatus();}"
            // ensure the active mob's ration is loaded so its feed bars show
            "var autoBuildBusy=false;"
            "async function maybeAutoBuild(){"
              "if(loadActive||autoBuildBusy)return;"
              "autoBuildBusy=true;"
              "await api('/api/load/start','POST');"  // build + tare, no feed filling yet
              "feedBtnSig='';"
              "setTimeout(function(){autoBuildBusy=false;},1500);"
            "}"

            // live session info (refreshed each poll) used to state the buttons
            "var loadSteps=[],loadCurStep=0,loadActive=false,feedBtnSig='';"
            "var allowCombine=true;"

            // one button per feed in the active mob's ration. Each is a FILL
            // TARGET selector: tap to fill that product now; once it's in the
            // load, tapping again just re-activates it (one instance per load).
            "function renderFeedButtons(){"
              "var c=document.getElementById('add-ing-buttons');if(!c)return;"
              "var h=herds[activeHerd];"
              "if(!h||!h.entries||!h.entries.length){"
                "c.innerHTML='<p class=muted>No feeds allocated to this mob. Set a ration in Settings.</p>';"
                "feedBtnSig='';return;"
              "}"
              // signature includes ration + which steps exist/active so buttons restate
              "var stepSig=loadSteps.map(function(s){"
                "return s.ingredient_idx+(s.complete?'d':'')+(s.overshot?'o':'');}).join(',')"
                "+'#'+loadCurStep+'#'+(loadActive?1:0);"
              "var sig=activeHerd+':'+h.num_cows+':'+h.meals_per_day+':'+h.entries.map(function(e){"
                "return e.ingredient_idx+'='+e.kg_dm_per_cow;}).join(',')+'|'+stepSig;"
              "if(sig===feedBtnSig)return;"
              "feedBtnSig=sig;"
              "c.innerHTML=h.entries.map(function(e){"
                "var ii=e.ingredient_idx;"
                "var ing=ings[ii]||{name:'?',dm_pct:100};"
                // target is the dashboard ration amount; load target never exceeds it
                "var wet=(e.kg_dm_per_cow*h.num_cows/h.meals_per_day)/(ing.dm_pct/100);"
                // is this feed already in the load?
                "var stepIdx=-1;"
                "for(var k=0;k<loadSteps.length;k++){if(loadSteps[k].ingredient_idx===ii){stepIdx=k;break;}}"
                "var st=stepIdx>=0?loadSteps[stepIdx]:null;"
                "var cls='feed-btn',tag='';"
                "if(st){"
                  "if(loadActive&&stepIdx===loadCurStep){cls+=' filling';tag='<span class=feed-btn-state>&#x25CF; Filling now</span>';}"
                  "else if(st.complete){cls+=' done';tag='<span class=feed-btn-state>&#x2714; Loaded &mdash; tap to re-fill</span>';}"
                  "else{cls+=' inload';tag='<span class=feed-btn-state>In load &mdash; tap to fill</span>';}"
                "}else{tag='<span class=feed-btn-state>Tap to fill</span>';}"
                "return '<button class=\"'+cls+'\" onclick=\"fillFeed('+ii+','+wet.toFixed(1)+')\">'"
                  "+'<span class=feed-btn-name>'+ing.name+'</span>'"
                  "+'<span class=feed-btn-kg>'+wet.toFixed(0)+' kg wet target</span>'"
                  "+tag"
                  "+'</button>';"
              "}).join('');"
            "}"

            // tap handler: activate if already in load, else add (which also
            // makes it the active fill). One instance per feed per load.
            "async function fillFeed(ing,kg){"
              "var stepIdx=-1;"
              "for(var k=0;k<loadSteps.length;k++){if(loadSteps[k].ingredient_idx===ing){stepIdx=k;break;}}"
              "if(stepIdx>=0){"
                "await api('/api/load/activate?step='+stepIdx,'POST');"
                "showToast('Filling '+(ings[ing]?ings[ing].name:'feed'));"
              "}else{"
                "await api('/api/load/addstep?ing='+ing+'&kg='+kg,'POST');"
                "showToast('Filling '+(ings[ing]?ings[ing].name:'feed')+' ('+Math.round(kg)+' kg)');"
              "}"
            "}"

            // buttons to combine other mobs' whole mixes into this load (toggle)
            "var combineSig='';"
            "function renderCombineButtons(){"
              "var card=document.getElementById('combine-mob-buttons');if(!card)return;"
              "var wrap=card.parentElement;"
              // hide the whole combine card when combining is disabled
              "if(!allowCombine){if(wrap)wrap.style.display='none';combineSig='';return;}"
              "if(wrap)wrap.style.display='block';"
              "var inLoad={};loadSteps.forEach(function(s){inLoad[s.herd_idx]=true;});"
              "var sig=activeHerd+'|'+Object.keys(inLoad).join(',')+'|'+herds.length+'|'+allowCombine;"
              "if(sig===combineSig)return;combineSig=sig;"
              "var hint=document.getElementById('combine-hint');"
              "var html=herds.map(function(h,i){"
                "if(i===activeHerd)return '';"  // primary mob is the base load
                "if(!h.entries||!h.entries.length)return '';"
                "var added=inLoad[i];"
                "var wet=0;(h.entries||[]).forEach(function(e){"
                  "var ing=ings[e.ingredient_idx]||{dm_pct:100};"
                  "wet+=(e.kg_dm_per_cow*h.num_cows/h.meals_per_day)/(ing.dm_pct/100);});"
                "var cls='feed-btn'+(added?' filling':'');"
                "return '<button class=\"'+cls+'\" onclick=\"toggleCombineMob('+i+','+(added?1:0)+')\">'"
                  "+'<span class=feed-btn-name>'+h.name+'</span>'"
                  "+'<span class=feed-btn-kg>'+wet.toFixed(0)+' kg wet</span>'"
                  "+'<span class=feed-btn-state>'+(added?'&#x2714; in load - tap to remove':'Tap to combine')+'</span>'"
                  "+'</button>';"
              "}).filter(function(x){return x;}).join('');"
              "card.innerHTML=html||'<p class=muted>No other mobs with a ration set.</p>';"
              "if(hint){var n=Object.keys(inLoad).length;"
                "hint.textContent=n>1?('Combined load: '+n+' mobs. Feedout shows a bar per mob.'):"
                  "'Add another mob to feed two mixes from one batch.';}"
            "}"
            "async function toggleCombineMob(i,added){"
              "if(added){"
                "await api('/api/load/removeherd?id='+i,'POST');"
                "showToast('Removed '+(herds[i]?herds[i].name:'mob')+' from load');"
              "}else{"
                "await api('/api/load/addherd?id='+i,'POST');"
                "showToast('Added '+(herds[i]?herds[i].name:'mob')+' mix to load');"
              "}"
              "combineSig='';"
            "}"

            "function renderLoadingLive(d){"
              "var tot=document.getElementById('load-total-kg');"
              "var tgt=document.getElementById('load-total-target');"
              "var bar=document.getElementById('load-total-bar');"
              "var pctEl=document.getElementById('load-total-pct');"
              "var ctrl=document.getElementById('load-controls');"
              "var c=document.getElementById('load-status');"
              "if(!c||!ctrl)return;"
              "loadSteps=d.steps||[];"
              "loadCurStep=d.current_step;"
              "loadActive=!!d.session_active;"
              "allowCombine=(d.allow_combine!==false);"
              "renderFeedButtons();"
              "renderCombineButtons();"
              // total load card (live)
              "if(tot)tot.textContent=(d.total_loaded_kg||0).toFixed(0);"
              "if(tgt)tgt.textContent=(d.total_target_kg||0).toFixed(0);"
              "var tpct=Math.min(100,d.total_pct||0);"
              "if(bar){bar.style.width=tpct.toFixed(0)+'%';}"
              "if(pctEl)pctEl.textContent=(d.total_pct||0).toFixed(0)+'%';"
              // not loading: show start button
              "if(!d.session_active){"
                "ctrl.innerHTML='<button class=\"btn btn-block\" onclick=\"startLoad()\">&#x25B6; Start Load (active herd)</button>';"
                "c.innerHTML=d.session_complete?"
                  "'<p class=muted>Last load complete. Switch mob or start a new load.</p>':"
                  "'<p class=muted>Building load for this mob...</p>';"
                "return;"
              "}"
              // loading: show finish control
              "ctrl.innerHTML='<div class=btn-row>'"
                "+'<button class=\"btn btn-ghost\" onclick=\"confirmStep()\">&#x2714; Confirm Current</button>'"
                "+'<button class=\"btn btn-danger\" onclick=\"finishLoad()\">&#x25A0; Finish Load</button>'"
                "+'</div>';"
              // progress bar per feed (no buttons). For combined loads, group by mob.
              "var steps=d.steps||[];"
              "var lastHerd=-1,html='';"
              "for(var i=0;i<steps.length;i++){"
                "var s=steps[i];"
                "var ing=ings[s.ingredient_idx]||{name:'?'};"
                "var pct=s.target_wet_kg>0?Math.min(100,s.loaded_wet_kg/s.target_wet_kg*100):0;"
                "var isCur=(i===d.current_step);"
                "var cls=s.complete?'done':(s.overshot?'over':(isCur?'active':''));"
                "var barCls=s.overshot?'danger':(pct>90?'warn':'');"
                // mob sub-heading when the load spans more than one mob
                "if(s.herd_idx!==lastHerd){lastHerd=s.herd_idx;"
                  "var multi=false;for(var k=0;k<steps.length;k++){if(steps[k].herd_idx!==steps[0].herd_idx){multi=true;break;}}"
                  "if(multi)html+='<div class=mob-subhead>'+(herds[s.herd_idx]?herds[s.herd_idx].name:('Mob '+s.herd_idx))+'</div>';}"
                "html+='<div class=\"step-card '+cls+'\">'"
                  "+'<h3>'+ing.name+(isCur?' &#x25CF; loading now':'')+(s.complete?' &#x2714;':'')"
                    "+(s.overshot?' &#x26A0; OVER':'')+'</h3>'"
                  "+'<div class=prog-wrap>'"
                    "+'<div class=prog-bg><div class=\"prog-fill '+barCls+'\" style=\"width:'+pct.toFixed(0)+'%\"></div></div>'"
                  "+'</div>'"
                  "+'<div style=\"display:flex;justify-content:space-between;font-size:13px\">'"
                    "+'<span>Loaded: <b>'+s.loaded_wet_kg.toFixed(0)+' kg</b></span>'"
                    "+'<span>Target: <b>'+s.target_wet_kg.toFixed(0)+' kg</b> ('+pct.toFixed(0)+'%)</span>'"
                  "+'</div>'"
                  "+'</div>';"
              "}"
              "c.innerHTML=html;"
            "}"

            // ---- Load control actions ----
            "async function startLoad(){"
              "await api('/api/load/start','POST');"
              "showToast('Load started - tare done');"
            "}"
            "async function activateStep(i){"
              "await api('/api/load/activate?step='+i,'POST');"
              "showToast('Activated ingredient '+(i+1));"
            "}"
            "async function confirmStep(){"
              "await api('/api/load/confirm','POST');"
              "showToast('Step confirmed');"
            "}"
            "async function finishLoad(){"
              "if(!confirm('Finish load and start feeding out?'))return;"
              // capture the load composition BEFORE finishing clears it
              "foFeeds=captureLoadFeeds();foMobs=buildFoMobs();"
              "await api('/api/load/finish','POST');"
              "foActive=true;foStartKg=scaleKg;buildLaneTargets();"
              // record per-feed amounts (loaded vs target) to the log now
              "await logFeedout(foFeeds,foStartKg);foLogged=true;"
              "showToast('Load finished & logged - feeding out');"
              "showTab('feedout');"
            "}"

            // ---- Feedout module with feed lanes ----
            "function renderFeedout(){"
              "renderLaneControls();"
              // combined (multi-mob) loads use per-mob bars; hide lane controls
              "var combined=(foMobs&&foMobs.length>1);"
              "var lsc=document.getElementById('lane-setup-card');"
              "if(lsc)lsc.style.display=combined?'none':'block';"
              "var lbTitle=document.querySelector('#lane-bars-card .card-title');"
              "if(lbTitle)lbTitle.textContent=combined?'Mob progress (feed out each mob)':'Lane progress';"
              "var ctrl=document.getElementById('feedout-controls');if(!ctrl)return;"
              "if(!foActive){"
                "ctrl.innerHTML='<button class=\"btn btn-block\" onclick=\"startFeedout()\">&#x25B6; Start Feedout (use current weight)</button>';"
                "document.getElementById('lane-bars-card').style.display='none';"
              "}else{"
                "ctrl.innerHTML='<button class=\"btn btn-danger btn-block\" onclick=\"stopFeedout()\">&#x25A0; End Feedout</button>';"
                "document.getElementById('lane-bars-card').style.display='block';"
              "}"
            "}"

            // lane count + mode + variable % inputs
            "function renderLaneControls(){"
              "var lc=document.getElementById('lane-count');"
              "if(lc){var h='';for(var n=1;n<=4;n++){"
                "h+='<button class=\"lane-num-btn'+(n===laneCount?' active':'')+'\" onclick=\"setLaneCount('+n+')\">'+n+'</button>';}"
                "lc.innerHTML=h;}"
              "var me=document.getElementById('mode-even'),mv=document.getElementById('mode-var');"
              "if(me)me.classList.toggle('active',laneMode==='even');"
              "if(mv)mv.classList.toggle('active',laneMode==='variable');"
              "var pi=document.getElementById('lane-pct-inputs');"
              "if(pi){"
                "if(laneMode==='variable'&&laneCount>1){"
                  "var rows='';for(var i=0;i<laneCount;i++){"
                    "rows+='<div class=lane-pct-row><label>Lane '+(i+1)+' %</label>'"
                      "+'<input type=number id=lane-pct-'+i+' value='+(lanePct[i]||0)+' min=0 max=100 onchange=\"onLanePctChange()\"></div>';}"
                  "pi.innerHTML=rows+'<div class=sub id=lane-pct-sum></div>';updateLanePctSum();"
                "}else{pi.innerHTML='';}"
              "}"
            "}"

            "function setLaneCount(n){"
              "laneCount=n;"
              // reset to even split by default
              "lanePct=[];for(var i=0;i<n;i++)lanePct.push(Math.round(100/n));"
              "lanePct[n-1]=100-lanePct.slice(0,n-1).reduce(function(a,b){return a+b;},0);"
              "if(foActive)buildLaneTargets();"
              "renderLaneControls();"
            "}"
            "function setLaneMode(m){"
              "laneMode=m;"
              "if(m==='even'){lanePct=[];for(var i=0;i<laneCount;i++)lanePct.push(100/laneCount);}"
              "if(foActive)buildLaneTargets();"
              "renderLaneControls();"
            "}"
            "function onLanePctChange(){"
              "for(var i=0;i<laneCount;i++){var e=document.getElementById('lane-pct-'+i);"
                "if(e)lanePct[i]=parseFloat(e.value)||0;}"
              "updateLanePctSum();if(foActive)buildLaneTargets();"
            "}"
            "function updateLanePctSum(){"
              "var s=0;for(var i=0;i<laneCount;i++)s+=(lanePct[i]||0);"
              "var el=document.getElementById('lane-pct-sum');"
              "if(el){el.textContent='Total: '+s.toFixed(0)+'% '+(Math.abs(s-100)<0.5?'\\u2714':'(should be 100%)');"
                "el.style.color=Math.abs(s-100)<0.5?'#2d8a4e':'#c0392b';}"
            "}"

            // compute kg target per lane from the load start weight
            "function buildLaneTargets(){"
              "laneTargets=[];"
              "var pcts=[];"
              "if(laneMode==='even'){for(var i=0;i<laneCount;i++)pcts.push(100/laneCount);}"
              "else{var s=0;for(var i=0;i<laneCount;i++)s+=(lanePct[i]||0);"
                "for(var i=0;i<laneCount;i++)pcts.push(s>0?(lanePct[i]||0)/s*100:100/laneCount);}"
              "for(var i=0;i<laneCount;i++)laneTargets.push(foStartKg*pcts[i]/100);"
            "}"

            "function startFeedout(){"
              "foActive=true;foStartKg=scaleKg;buildLaneTargets();"
              "foFeeds=captureLoadFeeds();foMobs=buildFoMobs();foLogged=false;"
              "renderFeedout();showToast('Feedout started');"
            "}"
            "async function stopFeedout(){"
              "var dispensed=Math.max(0,foStartKg-scaleKg);"
              "foActive=false;renderFeedout();"
              "if(!foLogged){await logFeedout(foFeeds,dispensed);foLogged=true;"
                "showToast('Feedout ended - logged');}"
              "else{showToast('Feedout ended');}"
              "foLogged=false;foFeeds=[];"
            "}"

            // called every poll: total + per-lane progress with auto-advance
            "function updateFeedout(d){"
              "if(!foActive){var lb0=document.getElementById('lane-bars');if(lb0&&!laneTargets.length)lb0.innerHTML='';return;}"
              "var dispensed=Math.max(0,foStartKg-scaleKg);"
              "var remaining=Math.max(0,scaleKg);"
              "var pct=foStartKg>0?Math.min(100,dispensed/foStartKg*100):0;"
              "var set=function(id,val){var e=document.getElementById(id);if(e)e.textContent=val;};"
              "set('fo-remaining',remaining.toFixed(0));"
              "set('fo-dispensed',dispensed.toFixed(0));"
              "var bar=document.getElementById('fo-bar');"
              "if(bar)bar.style.width=pct.toFixed(0)+'%';"
              "var pe=document.getElementById('fo-pct');"
              "if(pe)pe.textContent=pct.toFixed(0)+'% fed out';"
              // Progress bars: per MOB when a combined (multi-mob) load,
              // otherwise per LANE. Both drain as fed out and auto-advance.
              "var lb=document.getElementById('lane-bars');"
              "if(!lb)return;"
              "var segs=[],perMob=(foMobs&&foMobs.length>1);"
              "if(perMob){"
                "for(var i=0;i<foMobs.length;i++)segs.push({label:foMobs[i].name,kg:foMobs[i].kg});"
              "}else{"
                "for(var i=0;i<laneTargets.length;i++)segs.push({label:'Lane '+(i+1),kg:laneTargets[i]});"
              "}"
              "if(!segs.length){lb.innerHTML='';return;}"
              "var cum=0,html='';"
              "for(var i=0;i<segs.length;i++){"
                "var t=segs[i].kg;"
                "var segFed=Math.min(t,Math.max(0,dispensed-cum));"
                "var segRemain=Math.max(0,t-segFed);"
                "var remPct=t>0?segRemain/t*100:0;"  // bar shows REMAINING (full->empty)
                "var done=segFed>=t-0.5;"
                "var isActive=(!done&&dispensed>=cum&&foActive);"
                "var cls='lane-bar'+(done?' done':(isActive?' active':''));"
                "var tag=done?'&#x2714; done':(isActive?'feeding now':'waiting');"
                "html+='<div class=\"'+cls+'\">'"
                  "+'<div class=lane-bar-head><span>'+segs[i].label+' ('+tag+')</span>'"
                    "+'<span>'+segRemain.toFixed(0)+' / '+t.toFixed(0)+' kg left</span></div>'"
                  "+'<div class=prog-bg><div class=prog-fill style=\"width:'+remPct.toFixed(0)+'%\"></div></div>'"
                  "+'</div>';"
                "cum+=t;"
              "}"
              "lb.innerHTML=html;"
            "}"

            // ---- Tare ----
            "async function tare(){"
              "showToast('Taring scale...');"
              "await api('/api/tare','POST');"
              "showToast('Scale tared');"
            "}"

            // ---- Modal ----
            "function openModal(title,body){"
              "document.getElementById('modal-title').textContent=title;"
              "document.getElementById('modal-body').innerHTML=body;"
              "document.getElementById('modal').classList.remove('hidden');"
              "document.getElementById('modal-overlay').classList.remove('hidden');"
            "}"
            "function closeModal(){"
              "document.getElementById('modal').classList.add('hidden');"
              "document.getElementById('modal-overlay').classList.add('hidden');"
            "}"

            // ---- Toast ----
            "function showToast(msg){"
              "var t=document.querySelector('.toast');"
              "if(!t){t=document.createElement('div');t.className='toast';document.body.appendChild(t);}"
              "t.textContent=msg;t.classList.add('show');"
              "setTimeout(function(){t.classList.remove('show');},2200);"
            "}"

            // ---- Init ----
            "loadAll();"
            "initSwipe();"
            "initHeaderSwipe();"
            "renderFeedout();"
            "pollInt=setInterval(pollStatus,1000);"
        );
    }

    // ============================================================
    //  API handlers
    // ============================================================

    void handleStatus() {
        StaticJsonDocument<1024> doc;
        doc["scale_kg"]      = g_scaleKg;
        doc["scale_ok"]      = g_scaleOK;
        doc["active_herd"]   = g_activeHerd;
        doc["session_active"]= g_session.active;
        doc["session_complete"] = g_session.complete;
        doc["current_step"]  = g_session.current_step;
        doc["allow_combine"] = g_allowCombine;

        // Totals for the scale summary (live)
        float totalTarget = g_session.totalTargetWet();
        float totalLoaded = g_scaleKg;   // tared at session start = total wet on wagon
        doc["total_target_kg"] = totalTarget;
        doc["total_loaded_kg"] = totalLoaded;
        doc["total_pct"]       = (totalTarget > 0.1f)
                                 ? (totalLoaded / totalTarget * 100.0f) : 0.0f;

        JsonArray steps = doc.createNestedArray("steps");
        for (uint8_t i = 0; i < g_session.num_steps; i++) {
            JsonObject s = steps.createNestedObject();
            s["ingredient_idx"]  = g_session.steps[i].ingredient_idx;
            s["herd_idx"]        = g_session.steps[i].herd_idx;
            s["target_wet_kg"]   = g_session.steps[i].target_wet_kg;
            s["loaded_wet_kg"]   = g_session.steps[i].loaded_wet_kg;
            s["complete"]        = g_session.steps[i].complete;
            s["overshot"]        = g_session.steps[i].overshot;
        }

        String out;
        serializeJson(doc, out);
        sendJSON(200, out);
    }

    void handleGetHerds() {
        StaticJsonDocument<4096> doc;
        JsonArray arr = doc.createNestedArray("herds");
        for (uint8_t i = 0; i < g_herdCount; i++) {
            JsonObject h = arr.createNestedObject();
            h["name"]          = g_herds[i].name;
            h["num_cows"]      = g_herds[i].num_cows;
            h["meals_per_day"] = g_herds[i].meals_per_day;
            JsonArray entries  = h.createNestedArray("entries");
            for (uint8_t j = 0; j < g_herds[i].num_entries; j++) {
                JsonObject e = entries.createNestedObject();
                e["ingredient_idx"] = g_herds[i].entries[j].ingredient_idx;
                e["kg_dm_per_cow"]  = g_herds[i].entries[j].kg_dm_per_cow;
            }
        }
        String out;
        serializeJson(doc, out);
        sendJSON(200, out);
    }

    void handleCreateHerd() {
        if (g_herdCount >= MAX_HERDS) { sendError("Max herds reached"); return; }
        StaticJsonDocument<2048> doc;
        if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
            sendError("Bad JSON"); return;
        }
        Herd& h = g_herds[g_herdCount];
        memset(&h, 0, sizeof(h));
        strncpy(h.name, doc["name"] | "Herd", MAX_NAME_LEN - 1);
        h.num_cows      = doc["num_cows"] | 100;
        h.meals_per_day = doc["meals_per_day"] | 2;
        h.active        = true;

        JsonArray entries = doc["entries"].as<JsonArray>();
        for (JsonObject e : entries) {
            if (h.num_entries >= MAX_INGREDIENTS) break;
            h.entries[h.num_entries].ingredient_idx = e["ingredient_idx"];
            h.entries[h.num_entries].kg_dm_per_cow  = e["kg_dm_per_cow"];
            h.num_entries++;
        }
        g_saveHerdIdx     = g_herdCount;
        g_saveHerdPending = true;
        g_herdCount++;
        sendOK();
    }

    void handleHerdById() {
        int id = server.arg("id").toInt();
        if (id < 0 || id >= g_herdCount) { sendError("Bad id"); return; }
        StaticJsonDocument<2048> doc;
        Herd& h = g_herds[id];
        doc["name"]          = h.name;
        doc["num_cows"]      = h.num_cows;
        doc["meals_per_day"] = h.meals_per_day;
        JsonArray entries    = doc.createNestedArray("entries");
        for (uint8_t j = 0; j < h.num_entries; j++) {
            JsonObject e = entries.createNestedObject();
            e["ingredient_idx"] = h.entries[j].ingredient_idx;
            e["kg_dm_per_cow"]  = h.entries[j].kg_dm_per_cow;
        }
        String out;
        serializeJson(doc, out);
        sendJSON(200, out);
    }

    void handleUpdateHerd() {
        int id = server.arg("id").toInt();
        if (id < 0 || id >= g_herdCount) { sendError("Bad id"); return; }

        // set_active shortcut
        if (server.arg("plain").indexOf("set_active") >= 0) {
            g_activeHerd = id;
            sendOK(); return;
        }

        StaticJsonDocument<2048> doc;
        if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
            sendError("Bad JSON"); return;
        }
        Herd& h = g_herds[id];
        strncpy(h.name, doc["name"] | h.name, MAX_NAME_LEN - 1);
        h.num_cows      = doc["num_cows"] | h.num_cows;
        h.meals_per_day = doc["meals_per_day"] | h.meals_per_day;
        h.num_entries   = 0;
        JsonArray entries = doc["entries"].as<JsonArray>();
        for (JsonObject e : entries) {
            if (h.num_entries >= MAX_INGREDIENTS) break;
            h.entries[h.num_entries].ingredient_idx = e["ingredient_idx"];
            h.entries[h.num_entries].kg_dm_per_cow  = e["kg_dm_per_cow"];
            h.num_entries++;
        }
        g_saveHerdIdx     = id;
        g_saveHerdPending = true;
        sendOK();
    }

    void handleDeleteHerd() {
        int id = server.arg("id").toInt();
        if (id < 0 || id >= g_herdCount) { sendError("Bad id"); return; }
        for (uint8_t i = id; i < g_herdCount - 1; i++) g_herds[i] = g_herds[i + 1];
        g_herdCount--;
        if (g_activeHerd >= g_herdCount && g_herdCount > 0) g_activeHerd = g_herdCount - 1;
        g_saveHerdPending = true;
        g_saveHerdIdx     = 0xFF;  // signal full resave
        sendOK();
    }

    void handleGetIngredients() {
        StaticJsonDocument<2048> doc;
        JsonArray arr = doc.createNestedArray("ingredients");
        for (uint8_t i = 0; i < g_ingCount; i++) {
            JsonObject o = arr.createNestedObject();
            o["name"]   = g_ingredients[i].name;
            o["dm_pct"] = g_ingredients[i].dm_pct;
            o["active"] = g_ingredients[i].active;
        }
        String out;
        serializeJson(doc, out);
        sendJSON(200, out);
    }

    void handleUpdateIngredients() {
        StaticJsonDocument<2048> doc;
        if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
            sendError("Bad JSON"); return;
        }
        JsonArray arr = doc["ingredients"].as<JsonArray>();
        uint8_t i = 0;
        for (JsonObject o : arr) {
            if (i >= g_ingCount) break;
            g_ingredients[i].dm_pct = o["dm_pct"] | g_ingredients[i].dm_pct;
            i++;
        }
        // Signal main loop to persist
        g_saveHerdPending = true;
        g_saveHerdIdx     = 0xFE;  // signal ingredient save
        sendOK();
    }

    void handleTare() {
        g_tarePending = true;
        sendOK();
    }

    // ---- Load control (set flags, AppController services them) ----

    void handleLoadStart() {
        g_startLoadPending = true;
        sendOK();
    }

    void handleLoadActivate() {
        int step = server.arg("step").toInt();
        if (step < 0 || step >= g_session.num_steps) { sendError("Bad step"); return; }
        g_activateStepIdx     = (uint8_t)step;
        g_activateStepPending = true;
        sendOK();
    }

    void handleLoadAddStep() {
        int ing = server.hasArg("ing") ? server.arg("ing").toInt() : -1;
        float kg = server.hasArg("kg") ? server.arg("kg").toFloat() : 0.0f;
        if (ing < 0 || ing >= g_ingCount) { sendError("Bad ingredient"); return; }
        if (kg <= 0.0f || kg > 99999.0f)  { sendError("Bad target"); return; }
        g_addStepIng      = (uint8_t)ing;
        g_addStepTargetKg = kg;
        g_addStepPending  = true;
        sendOK();
    }

    void handleLoadAddHerd() {
        int id = server.hasArg("id") ? server.arg("id").toInt() : -1;
        if (id < 0 || id >= g_herdCount) { sendError("Bad herd"); return; }
        if (!g_allowCombine)            { sendError("Combining disabled"); return; }
        g_addHerdIdx     = (uint8_t)id;
        g_addHerdPending = true;
        sendOK();
    }

    void handleLoadRemoveHerd() {
        int id = server.hasArg("id") ? server.arg("id").toInt() : -1;
        if (id < 0 || id >= g_herdCount) { sendError("Bad herd"); return; }
        g_removeHerdIdx     = (uint8_t)id;
        g_removeHerdPending = true;
        sendOK();
    }

    void handleSetSettings() {
        if (server.hasArg("allow_combine")) {
            g_allowCombine = (server.arg("allow_combine").toInt() != 0);
            g_saveSettingsPending = true;
        }
        sendOK();
    }

    void handleLoadConfirm() {
        g_confirmStepPending = true;
        sendOK();
    }

    void handleLoadFinish() {
        g_finishLoadPending = true;
        sendOK();
    }

    // ---- Quick headcount change from dashboard ----

    void handleSetCows() {
        int id   = server.hasArg("id")   ? server.arg("id").toInt()   : g_activeHerd;
        int cows = server.hasArg("cows") ? server.arg("cows").toInt() : -1;
        if (id < 0 || id >= g_herdCount) { sendError("Bad id"); return; }
        if (cows < 1 || cows > 9999)     { sendError("Bad cows"); return; }
        g_herds[id].num_cows = (uint16_t)cows;
        g_saveHerdIdx     = (uint8_t)id;
        g_saveHerdPending = true;
        sendOK();
    }

    // ============================================================
    //  Feed log (LittleFS CSV: date,mob,kg per line)
    //  Date is YYYYMMDD supplied by the phone (browser local clock).
    // ============================================================

    void handleLogAdd() {
        StaticJsonDocument<2048> doc;
        if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
            sendError("Bad JSON"); return;
        }
        long date  = doc["date"] | 0L;
        String mob = doc["mob"]  | "Unknown";
        if (date < 19000101L) { sendError("Bad date"); return; }
        mob.replace(",", " "); mob.replace("\n", " ");
        if (mob.length() > 24) mob = mob.substring(0, 24);

        File f = LittleFS.open("/feedlog.csv", FILE_APPEND);
        if (!f) { sendError("FS write failed"); return; }

        // One line per feed:  date,mob,feed,fedKg,targetKg
        JsonArray feeds = doc["feeds"].as<JsonArray>();
        if (feeds.isNull() || feeds.size() == 0) {
            // fall back to a single total entry if no per-feed data given
            float kg = doc["kg"] | 0.0f;
            if (kg > 0.0f)
                f.printf("%ld,%s,Mixed load,%.1f,%.1f\n", date, mob.c_str(), kg, kg);
        } else {
            for (JsonObject fe : feeds) {
                String name = fe["name"] | "Feed";
                name.replace(",", " "); name.replace("\n", " ");
                if (name.length() > 24) name = name.substring(0, 24);
                float fed    = fe["fed"]    | 0.0f;
                float target = fe["target"] | 0.0f;
                if (fed <= 0.0f) continue;
                f.printf("%ld,%s,%s,%.1f,%.1f\n",
                         date, mob.c_str(), name.c_str(), fed, target);
            }
        }
        f.close();
        sendOK();
    }

    void handleLogQuery() {
        long from = server.hasArg("from") ? server.arg("from").toInt() : 0;
        long to   = server.hasArg("to")   ? server.arg("to").toInt()   : 99999999L;

        const uint8_t MAXM = 16;
        String mobNames[MAXM]; float mobKg[MAXM]; uint8_t mobN = 0;
        String fdNames[MAXM];  float fdFed[MAXM]; float fdTgt[MAXM]; uint8_t fdN = 0;
        float total = 0.0f;
        uint32_t lines = 0;

        File f = LittleFS.open("/feedlog.csv", FILE_READ);
        if (f) {
            while (f.available()) {
                String line = f.readStringUntil('\n');
                line.trim();
                if (line.length() < 5) continue;
                // split into up to 5 fields
                int c1 = line.indexOf(',');
                int c2 = line.indexOf(',', c1 + 1);
                int c3 = line.indexOf(',', c2 + 1);
                int c4 = line.indexOf(',', c3 + 1);
                if (c1 < 0 || c2 < 0) continue;
                long d = line.substring(0, c1).toInt();
                if (d < from || d > to) continue;
                String mob = line.substring(c1 + 1, c2);
                String feed; float fed = 0, tgt = 0;
                if (c3 < 0) {
                    // old 3-field format: date,mob,kg
                    feed = "Mixed load";
                    fed  = line.substring(c2 + 1).toFloat();
                    tgt  = fed;
                } else {
                    feed = line.substring(c2 + 1, c3);
                    if (c4 < 0) { fed = line.substring(c3 + 1).toFloat(); tgt = fed; }
                    else { fed = line.substring(c3 + 1, c4).toFloat();
                           tgt = line.substring(c4 + 1).toFloat(); }
                }
                total += fed; lines++;
                // per-mob
                int8_t mi = -1;
                for (uint8_t k = 0; k < mobN; k++) if (mobNames[k] == mob) { mi = k; break; }
                if (mi < 0 && mobN < MAXM) { mi = mobN; mobNames[mobN] = mob; mobKg[mobN] = 0; mobN++; }
                if (mi >= 0) mobKg[mi] += fed;
                // per-feed
                int8_t fi = -1;
                for (uint8_t k = 0; k < fdN; k++) if (fdNames[k] == feed) { fi = k; break; }
                if (fi < 0 && fdN < MAXM) { fi = fdN; fdNames[fdN] = feed; fdFed[fdN] = 0; fdTgt[fdN] = 0; fdN++; }
                if (fi >= 0) { fdFed[fi] += fed; fdTgt[fi] += tgt; }
            }
            f.close();
        }

        StaticJsonDocument<4096> doc;
        doc["total_kg"] = total;
        doc["entries"]  = lines;
        JsonArray ma = doc.createNestedArray("mobs");
        for (uint8_t k = 0; k < mobN; k++) {
            JsonObject o = ma.createNestedObject();
            o["name"] = mobNames[k]; o["kg"] = mobKg[k];
        }
        JsonArray fa = doc.createNestedArray("feeds");
        for (uint8_t k = 0; k < fdN; k++) {
            JsonObject o = fa.createNestedObject();
            o["name"]   = fdNames[k];
            o["fed"]    = fdFed[k];
            o["target"] = fdTgt[k];
            o["over"]   = (fdFed[k] - fdTgt[k]);
        }
        String out;
        serializeJson(doc, out);
        sendJSON(200, out);
    }

    void handleLogClear() {
        LittleFS.remove("/feedlog.csv");
        sendOK();
    }

    void handleNotFound() {
        server.send(404, "text/plain", "Not found");
    }

private:
    WebServer server;
};
