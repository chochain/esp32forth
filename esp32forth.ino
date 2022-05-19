///
/// esp32Forth, Version 8 : for NodeMCU ESP32S
/// benchmark: 1M test case
///    1440ms Dr. Ting's orig/esp32forth_82
///    1240ms ~/Download/forth/esp32/esp32forth8_exp9 
///    1045ms orig/esp32forth8_1
///     700ms + INLINE List methods
///==========================================================================
/// ESP32 Web Serer connection and index page
///==========================================================================
#include <WiFi.h>
#include "eforth.h"

//const char *WIFI_SSID = "Sonic-6af4";
//const char *WIFI_PASS = "7b369c932f";
const char *WIFI_SSID = "Frontier7008";
const char *WIFI_PASS = "8551666595";

static const char *HTML_INDEX PROGMEM = R"XX(
HTTP/1.1 200 OK
Content-type:text/html

<html><head><meta charset='UTF-8'><title>esp32forth</title>
<style>body{font-family:'Courier New',monospace;font-size:12px;}</style>
</head>
<body>
    <div id='log' style='float:left;overflow:auto;height:600px;width:600px;
         background-color:#f8f0f0;'>esp32forth v8</div>
    <textarea id='tib' style='height:600px;width:400px;'
        onkeydown='if (13===event.keyCode) forth()'>words</textarea>
</body>
<script>
let log = document.getElementById('log')
let tib = document.getElementById('tib')
let idx = 0
function send_post(url, ary) {
    let id  = '_'+(idx++).toString()
    let cmd = '\n---CMD'+id+'\n'
    let req = ary.slice(0,30).join('\n')
    log.innerHTML += '<div id='+id+'><font color=blue>'+req.replace(/\n/g,'<br/>')+'</font></div>'
    fetch(url, {
        method: 'POST', headers: { 'Context-Type': 'text/plain' },
        body: cmd+req+cmd
     }).then(rsp=>rsp.text()).then(txt=>{
        document.getElementById(id).innerHTML +=
            txt.replace(/\n/g,'<br/>').replace(/\s/g,'&nbsp;')
        log.scrollTop=log.scrollHeight
        ary.splice(0,30)
        if (ary.length > 0) send_post(url, ary)
    })
}
function forth() {
    let ary = tib.value.split('\n')
    send_post('/input', ary)
    tib.value = ''
}
window.onload = ()=>{ tib.focus() }
</script></html>

)XX";

static const char *HTML_CHUNKED PROGMEM = R"XX(
HTTP/1.1 200 OK
Content-type:text/plain
Transfer-Encoding: chunked

)XX";

ForthVM *vm = new ForthVM();

namespace ForthServer {
    WiFiServer server;
    WiFiClient client;
    String     http_req;
    int readline() {
        http_req.clear();
        while (client.connected()) {
            if (client.available()) {
                char c = client.read();
                if (c == '\n') return 1;
                if (c != '\r') http_req += c;
            }
        }
        return 0;
    }
    void handle_index() {
        client.println(HTML_INDEX);                 ///
        delay(30);                   // give browser sometime to receive
    }
    void send_chunk(int len, const char *msg) {
        Serial.print(msg);
        client.println(len, HEX);
        client.println(msg);
        yield();
    }
    void handle_input() {
        while (readline() && http_req.length()>0);  /// skip HTTP header
        for (int i=0; i<4 && readline(); i++) {     /// find Forth command token
            if (http_req.startsWith("---CMD")) break;
        }
        // process Forth command, return in chunks
        client.println(HTML_CHUNKED);               /// send HTTP chunked header
        for (int i=0; readline(); i++) {
            if (http_req.startsWith("---CMD")) break;
            if (http_req.length() > 0) {
                Serial.println(http_req);           /// echo on console
                vm->outer(http_req.c_str(), send_chunk);
            }
        }
        send_chunk(0, "\r\n");                      /// close HTTP chunk stream
    }
    void handle_client() {                          /// uri router
        if (!(client = server.available())) return;
        while (readline()) {
            if (http_req.startsWith("GET /")) {
                handle_index();
                break;
            }
            else if (http_req.startsWith("POST /input")) {
                handle_input();
                break;
            }
        }
        client.stop();
        yield();
    }
    void setup(const char *ssid, const char *pass) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pass);
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
        }
        Serial.print("WiFi Connected. Server IP="); Serial.print(WiFi.localIP());
        server.begin(80);
        Serial.println(" port 80");
        // reserve string space
        http_req.reserve(256);
    }
};

///==========================================================================
/// ESP32 routines
///==========================================================================
void robot_setup() {
    // Setup timer and attach timer to a led pin
    ledcSetup(0, 100, 13);
    ledcAttachPin(5, 0);
    analogWrite(0, 250, 255);
    pinMode(2,OUTPUT);
    digitalWrite(2, HIGH);   // turn the LED2 on
    pinMode(16,OUTPUT);
    digitalWrite(16, LOW);   // motor1 forward
    pinMode(17,OUTPUT);
    digitalWrite(17, LOW);   // motor1 backward
    pinMode(18,OUTPUT);
    digitalWrite(18, LOW);   // motor2 forward
    pinMode(19,OUTPUT);
    digitalWrite(19, LOW);   // motor2 bacward
}

String console_cmd;
void setup() {
    Serial.begin(115200);
    delay(100);

    robot_setup();
    ForthServer::setup(WIFI_SSID, WIFI_PASS);
    vm->init();
    console_cmd.reserve(256);
    LOGF("\nesp32forth8\n");
}

void loop(void) {
    ForthServer::handle_client();
    delay(2);              // yield to background tasks (interrupt, timer,...)
    ///
    /// while Web requests come in from handleInput asynchronously,
    /// we also take user input from console (for debugging mostly)
    ///
    // for debugging: we can also take user input from Serial Monitor
    static auto send_to_con = [](int len, const char *rst) { LOG(rst); };
    if (Serial.available()) {
        console_cmd = Serial.readString();
        LOG(console_cmd);
        vm->outer(console_cmd.c_str(), send_to_con);
        vm->mem_stat();
        delay(2);
    }
}
