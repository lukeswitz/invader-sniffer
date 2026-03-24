#ifndef CONFIG_SERVER_H
#define CONFIG_SERVER_H

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "config.h"

class ConfigServer {
private:
    AsyncWebServer server;
    OUIConfig& ouiConfig;
    bool configMode = false;
    uint32_t bootTime = 0;
    static constexpr const char* AP_SSID = "SNIFF-CONFIG";
    static constexpr const char* AP_PASS = "sniffconfig";
    static constexpr uint32_t CONFIG_TIMEOUT_MS = 30000;

    static const char* getHtmlPage() {
        return R"rawHtml(
    <!DOCTYPE html>
    <html>
        <head>
            <meta charset="UTF-8">
            <meta name="viewport" content="width=device-width,initial-scale=1">
            <title>CONFIG</title>
            <style>
                *{margin:0;padding:0;box-sizing:border-box}
                body{font-family:system-ui,-apple-system,sans-serif;background:#fff;
                    color:#000;padding:40px 20px;transition:background 200ms,color 200ms}
                body.dark{background:#000;color:#fff}
                .container{max-width:600px;margin:0 auto}
                .header{display:flex;justify-content:space-between;align-items:center;
                    margin-bottom:30px}
                h1{font-size:18px;font-weight:600;letter-spacing:0.5px}
                .theme-btn{padding:1px 1px;border:1px solid currentColor;background:transparent;
                    color:inherit;font-size:12px;font-weight:600;text-transform:uppercase;
                    letter-spacing:0.5px;cursor:pointer}
                .theme-btn:hover{color:currentColor;color:var(--bg)}
                .form-group{margin-bottom:20px}
                label{display:block;font-size:12px;font-weight:600;margin-bottom:6px;
                    text-transform:uppercase;letter-spacing:0.5px}
                input{width:100%;padding:8px 10px;border:1px solid currentColor;font-size:13px;
                    font-family:monospace;background:inherit;color:inherit;transition:border 200ms}
                input:focus{outline:none;border:2px solid currentColor}
                .oui-inputs{display:grid;grid-template-columns:repeat(5,1fr);gap:8px;
                    margin-bottom:15px}
                .oui-inputs input{font-size:11px}
                .help-text{font-size:11px;opacity:0.6;margin-top:4px;line-height:1.5}
                .buttons{display:flex;gap:10px;margin-top:25px}
                button{padding:10px 16px;border:1px solid currentColor;background:inherit;
                    color:inherit;font-size:12px;font-weight:600;text-transform:uppercase;
                    letter-spacing:0.5px;cursor:pointer;flex:1;transition:all 200ms}
                button:hover{background:currentColor;color:var(--bg-inv)}
                button.danger{border-color:#cc0000;color:#cc0000}
                body.dark button.danger{color:#ff4444}
                button.danger:hover{background:#cc0000;color:#fff}
                body.dark button.danger:hover{background:#ff4444}
                .targets{margin-top:40px;padding-top:30px;border-top:1px solid currentColor}
                .targets h2{font-size:14px;font-weight:600;margin-bottom:15px;
                    letter-spacing:0.5px}
                .target-row{display:flex;justify-content:space-between;align-items:center;
                    padding:10px 0;border-bottom:1px solid;border-bottom-color:currentColor;
                    opacity:0.8;font-size:12px;transition:opacity 200ms}
                body.dark .target-row{border-bottom-color:rgba(255,255,255,0.2)}
                .target-row{border-bottom-color:rgba(0,0,0,0.1)}
                .target-row:last-child{border-bottom:none}
                .target-info{flex:1}
                .target-oui{font-family:monospace;font-weight:600;font-size:11px}
                .target-name{font-size:11px;opacity:0.6;margin-top:3px}
                .target-actions{display:flex;gap:8px}
                button.small{padding:4px 8px;font-size:10px;flex:0}
                .status{margin-top:20px;padding:10px;border:1px solid;
                    border-color:currentColor;font-size:11px;text-align:center;display:none;
                    transition:border-color 200ms}
                .status.show{display:block}
                .status.ok{border-color:#00cc00;color:#00cc00}
                .status.error{border-color:#cc0000;color:#cc0000}
                .empty{opacity:0.5;text-align:center;padding:20px;font-size:12px}
            </style>
        </head>
        <body>
            <div class="container">
                <div class="header">
                    <h1>TARGET CONFIGURATION</h1>
                    <br>
                    <div>    
                        <button class="theme-btn" id="themeBtn" onclick="toggleTheme()" title="Toggle theme">
                            ☀
                        </button></div>    
                </div>
                
                
                
                <div>
                    <label>Device Name</label>
                    <input type="text" id="name" placeholder="e.g., Apple Phone">
                    <div class="help-text">Supports * wildcard in name</div>
                </div>
                
                <div style="margin-top:20px">
                    <label>OUI (MAC prefix) - up to 5</label>
                    <div class="oui-inputs">
                        <input type="text" id="oui0" placeholder="AA:BB:CC" maxlength="8">
                        <input type="text" id="oui1" placeholder="AA:BB:CC" maxlength="8">
                        <input type="text" id="oui2" placeholder="AA:BB:CC" maxlength="8">
                        <input type="text" id="oui3" placeholder="AA:BB:CC" maxlength="8">
                        <input type="text" id="oui4" placeholder="AA:BB:CC" maxlength="8">
                    </div>
                    <div class="help-text">Enter at least one OUI</div>
                </div>
                
                <div class="buttons">
                    <button onclick="saveConfig()">Save Configuration</button>
                    <button onclick="saveAndLaunch()" style="background-color:#001F;color:#0F0">
                        Save & Start Capture
                    </button>
                </div>
                
                <div id="status" class="status"></div>
                
                <div class="targets">
                    <h2>ACTIVE TARGETS</h2>
                    <div id="targetsList">
                        <div class="empty">Loading...</div>
                    </div>
                    <button onclick="saveConfig()" style="width:100%;margin-top:20px">
                        Save Configuration
                    </button>
                </div>
                
            </div>
            
            <script>
                let targets=[];
                
                function toggleTheme(){
                    const isDark=document.body.classList.toggle('dark');
                    const btn=document.getElementById('themeBtn');
                    btn.textContent=isDark?'☀':'☾';
                    localStorage.setItem('theme',isDark?'dark':'light')
                }
                
                function loadTheme(){
                    const saved=localStorage.getItem('theme');
                    const isDark=saved==='dark'||(!saved&&window.matchMedia('(prefers-color-scheme:dark)').matches);
                    if(isDark){
                        document.body.classList.add('dark')
                    }
                    document.getElementById('themeBtn').textContent=isDark?'☀':'☾'
                }
                
                function loadTargets(){
                    fetch("/api/targets").then(r=>r.json()).then(d=>{
                        targets=d.targets;renderTargets()
                    }).catch(e=>showStatus("Load failed","error"))
                }
                
                function renderTargets(){
                    let html='';
                    if(targets.length===0){
                        html='<div class="empty">No targets configured</div>'
                    }else{
                        targets.forEach((t,i)=>{
                            html+='<div class="target-row">'
                            +'<div class="target-info">'
                            +'<div class="target-name">'+t.name+'</div>';
                            
                            if(t.ouis && t.ouis.length>0){
                                t.ouis.forEach(oui=>{
                                    html+='<div class="target-oui">'+oui+'</div>'
                                });
                            }else{
                                html+='<div class="target-oui">xx:xx:xx</div>'
                            }
                            
                            html+='</div>'
                            +'<div class="target-actions">'
                            +'<button class="small danger" onclick="deleteTarget('+i+')">DELETE</button>'
                            +'</div>'
                            +'</div>'
                        })
                    }
                    document.getElementById('targetsList').innerHTML=html
                }
                
                function addTarget(){
                    const name=document.getElementById('name').value.trim();
                    
                    const ouis=[];
                    for(let i=0;i<5;i++){
                        const val=document.getElementById('oui'+i).value.trim();
                        if(val){
                            if(!/^[0-9A-F]{2}:[0-9A-F]{2}:[0-9A-F]{2}$/i.test(val)){
                                showStatus("Invalid OUI format","error");
                                return
                            }
                            ouis.push(val)
                        }
                    }
                    
                    if(!name && ouis.length===0){
                        showStatus("Name or OUI required","error");
                        return
                    }
                    
                    fetch('/api/add',{
                        method:'POST',
                        headers:{'Content-Type':'application/json'},
                        body:JSON.stringify({name,ouis})
                    }).then(r=>r.json()).then(d=>{
                        if(d.ok){
                            document.getElementById('name').value='';
                            for(let i=0;i<5;i++)document.getElementById('oui'+i).value='';
                            loadTargets();
                            showStatus("Target added","ok")
                        }else showStatus(d.error||"Failed","error")
                    }).catch(e=>showStatus("Network error","error"))
                }
                
                function deleteTarget(i){
                    fetch('/api/delete',{
                        method:'POST',
                        headers:{'Content-Type':'application/json'},
                        body:JSON.stringify({idx:i})
                    })
                    .then(r=>r.json()).then(d=>{
                        if(d.ok){loadTargets();showStatus("Deleted","ok")}
                        else showStatus("Delete failed","error")
                    }).catch(e=>showStatus("Network error","error"))
                }
                
                function clearAll(){
                    if(!confirm("Delete all targets?"))return;
                    fetch('/api/clear',{method:'POST'})
                    .then(r=>r.json()).then(d=>{
                        if(d.ok){targets=[];renderTargets();showStatus("All cleared","ok")}
                        else showStatus("Clear failed","error")
                    }).catch(e=>showStatus("Network error","error"))
                }
                
                function saveConfig(){
                    fetch('/api/save',{method:'POST'})
                    .then(r=>r.json()).then(d=>{
                        showStatus(d.ok?"Configuration saved":"Save failed",d.ok?"ok":"error")
                    }).catch(e=>showStatus("Network error","error"))
                }

                function saveAndLaunch(){
                    fetch('/api/start',{method:'POST'})
                    .then(r=>r.json()).then(d=>{
                        if(d.ok){
                            showStatus("Saved - device starting capture...","ok")
                        }else showStatus("Save failed","error")
                    }).catch(e=>showStatus("Network error","error"))
                }
                
                function showStatus(msg,type){
                    const s=document.getElementById('status');
                    s.textContent=msg;s.className='status '+type;s.classList.add('show');
                    setTimeout(()=>{s.classList.remove('show')},2500)
                }
                
                loadTheme();
                loadTargets();
            </script>
        </body>
    </html>
    )rawHtml";
    }

    
void handleRoot(AsyncWebServerRequest* request) {
    request->send(200, "text/html", getHtmlPage());
}

public:
ConfigServer(OUIConfig& cfg)
    : server(80), ouiConfig(cfg) {}

bool begin() {
    Serial.println("[cfg] Starting config AP...");
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    
    if (!WiFi.softAP(AP_SSID, AP_PASS)) {
        Serial.println("[cfg] softAP failed");
        return false;
    }
    
    delay(200);
    
    Serial.printf("[cfg] AP SSID: %s\n", AP_SSID);
    Serial.printf("[cfg] IP: %s\n",
        WiFi.softAPIP().toString().c_str());

    server.on("/", HTTP_GET, [this](AsyncWebServerRequest* r) {
        handleRoot(r);
    });

    server.on("/api/targets", HTTP_GET,
        [this](AsyncWebServerRequest* r) {
        String json = "{\"targets\":[";
        for (int i = 0; i < ouiConfig.getCount(); i++) {
            const TargetOUI* t = &ouiConfig.getTargets()[i];
            if (i > 0) json += ",";
            json += "{\"name\":\"";
            json += t->name;
            json += "\",\"ouis\":[";
            for (int j = 0; j < t->ouiCount; j++) {
                if (t->ouis[j][0] == 0 && 
                    t->ouis[j][1] == 0 && 
                    t->ouis[j][2] == 0) continue;
                if (j > 0) json += ",";
                char buf[12];
                snprintf(buf, sizeof(buf), "\"%02X:%02X:%02X\"",
                    t->ouis[j][0], t->ouis[j][1], t->ouis[j][2]);
                json += buf;
            }
            json += "]}";
        }
        json += "]}";
        r->send(200, "application/json", json);
    });

    server.on("/api/add", HTTP_POST,
        [this](AsyncWebServerRequest* r) {},
        nullptr,
        [this](AsyncWebServerRequest* r, uint8_t* data,
            size_t len, size_t index, size_t total) {
        if (index == 0) {
            JsonDocument doc;
            deserializeJson(doc, data, len);
            
            const char* name = doc["name"];
            JsonArray ouisArr = doc["ouis"].as<JsonArray>();
            
            bool hasName = name && strlen(name) > 0;
            bool hasOuis = ouisArr.size() > 0;
            
            if (!hasName && !hasOuis) {
                r->send(200, "application/json",
                    "{\"ok\":false,\"error\":\"Name or OUI required\"}");
                return;
            }
            
            uint8_t ouiList[5][3];
            int ouiCount = 0;
            
            for (const char* ouiStr : ouisArr) {
                if (ouiCount >= 5) break;
                if (!ouiStr || strlen(ouiStr) == 0) continue;
                
                uint8_t oui[3];
                if (sscanf(ouiStr, "%02hhX:%02hhX:%02hhX",
                    &oui[0], &oui[1], &oui[2]) == 3) {
                    memcpy(ouiList[ouiCount], oui, 3);
                    ouiCount++;
                }
            }
            
            if (ouiCount > 0) {
                ouiConfig.addTarget(ouiList[0], name ? name : "");
                int targetIdx = ouiConfig.getCount() - 1;
                for (int i = 1; i < ouiCount; i++) {
                    ouiConfig.addOuiToTarget(targetIdx, ouiList[i]);
                }
            } else {
                uint8_t dummy[3] = {0, 0, 0};
                TargetOUI* target = ouiConfig.addTargetNameOnly(name);
            }
            
            Serial.printf("[config] added target: %s (%d OUIs)\n",
                hasName ? name : "(no name)", ouiCount);
            r->send(200, "application/json", "{\"ok\":true}");
        }
    });

    server.on("/api/delete", HTTP_POST,
        [this](AsyncWebServerRequest* r) {},
        nullptr,
        [this](AsyncWebServerRequest* r, uint8_t* data,
            size_t len, size_t index, size_t total) {
        if (index == 0) {
            JsonDocument doc;
            deserializeJson(doc, data, len);
            int idx = doc["idx"].as<int>();
            
            if (idx >= 0 && idx < ouiConfig.getCount()) {
                ouiConfig.deleteTarget(idx);
                r->send(200, "application/json", "{\"ok\":true}");
            } else {
                r->send(200, "application/json",
                    "{\"ok\":false,\"error\":\"Invalid index\"}");
            }
        }
    });

    server.on("/api/clear", HTTP_POST,
        [this](AsyncWebServerRequest* r) {
        ouiConfig.clearTargets();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/clients", HTTP_GET,
        [this](AsyncWebServerRequest* r) {
        int clients = WiFi.softAPgetStationNum();
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"clients\":%d}", clients);
        r->send(200, "application/json", buf);
    });

    server.on("/api/start", HTTP_POST,
        [this](AsyncWebServerRequest* r) {
        bool ok = ouiConfig.save();
        r->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        configMode = false;
    });

    server.on("/api/save", HTTP_POST,
        [this](AsyncWebServerRequest* r) {
        bool ok = ouiConfig.save();
        String resp = ok ? "{\"ok\":true}" :
            "{\"ok\":false,\"error\":\"Save failed\"}";
        r->send(200, "application/json", resp);
    });

    server.begin();
    delay(100);
    bootTime = millis();
    configMode = true;
    return true;
}

bool isConfigMode() const { return configMode; }

bool update() {
    if (!configMode) return false;
    
    int connectedClients = WiFi.softAPgetStationNum();
    
    if (connectedClients > 0) {
        bootTime = millis();
        return true;
    }
    
    if (millis() - bootTime > CONFIG_TIMEOUT_MS) {
        Serial.println("[cfg] timeout - entering capture");
        configMode = false;
        return false;
    }
    return true;
}
};

#endif