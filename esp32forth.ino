/******************************************************************************/
/* esp32Forth, Version 6.4 : for NodeMCU ESP32S                               */
/******************************************************************************/
#include "SPIFFS.h"
#include <WiFi.h>
#include <WebServer.h>
#include "SPIFFS.h"
#include "src/ceforth.h"

const char* ssid = "Frontier7008";
const char* pass = "8551666595";

WebServer server(80);

/******************************************************************************/
/* ledc                                                                       */
/******************************************************************************/
/* LEDC Software Fade */
// use first channel of 16 channels (started from zero)
#define LEDC_CHANNEL_0     0
// use 13 bit precission for LEDC timer
#define LEDC_TIMER_13_BIT  13
// use 5000 Hz as a LEDC base frequency
#define LEDC_BASE_FREQ     5000
// fade LED PIN (replace with LED_BUILTIN constant for built-in LED)
#define LED_PIN            5
#define BRIGHTNESS         255    // how bright the LED is

static const char *index_html PROGMEM = R"XX(
<html><head><meta charset='UTF-8'><title>esp32forth</title>
<style>body{font-family:'Courier New',monospace;font-size:12px;}</style>
</head>
<body>
    <div id='log' style='float:left;overflow:auto;height:600px;width:600px;
         background-color:#f8f0f0;'>ESP32Forth 8.02</div>
    <textarea id='tib' style='height:600px;width:400px;'
        onkeydown='if (13===event.keyCode) forth()'>words</textarea>
</body>
<script>
let log = document.getElementById('log')
let tib = document.getElementById('tib')
function httpPost(url, items, callback) {
    let fd = new FormData()
    for (k in items) { fd.append(k, items[k]) }
    let r = new XMLHttpRequest()
    r.onreadystatechange = function() {
        if (this.readyState != XMLHttpRequest.DONE) return
        callback(this.status===200 ? this.responseText : null) }
    r.open('POST', url)
    r.send(fd) }
function chunk(ary, d) {                        // recursive call to sequence POSTs
    req = ary.slice(0,40).join('\n')            // 40*(average 50 byte/line) ~= 2K
    if (req=='' || d>30) return                 // bail looping, just in case
    log.innerHTML+='<font color=blue>'+req.replace(/\n/g, '<br/>')+'</font>'
    httpPost('/input', { cmd: req }, rsp=>{
        if (rsp !== null) {
            log.innerHTML += rsp.replace(/\n/g, '<br/>').replace(/\s/g,'&nbsp;')
            log.scrollTop=log.scrollHeight      // scroll down
            chunk(ary.splice(40), d+1) }})}     // next 300 tokens
function forth() { chunk(tib.value.split('\n'), 1); tib.value = '' }
window.onload = ()=>{ tib.focus() }
</script></html>
)XX";

istringstream forth_in;
ostringstream forth_out;
ForthVM *forth_vm =  new ForthVM(forth_in, forth_out);

static String forth_exec(String cmd) {
    forth_out.str("");               // ready output buffer for next run
    forth_in.clear();                // clear any input stream error bit 
    forth_in.str(cmd.c_str());       // feed user command into input stream
    forth_vm->outer();               // invoke FVM outer interpreter
    return String(forth_out.str().c_str());
}

static int forth_load(const char *fname) {
    if (!SPIFFS.begin()) {
        Serial.println("Error mounting SPIFFS"); return 1;
    }
    File file = SPIFFS.open(fname, "r");
    if (!file) {
        Serial.print("Error opening file:"); Serial.println(fname); return 1;
    }
    Serial.print("Loading file: "); Serial.print(fname); Serial.print("...");
    while (file.available()) {
        String cmd = file.readStringUntil('\n');
        Serial.println("<< "+cmd);
        forth_exec(cmd);
    }
    Serial.println("Done loading."); 
    file.close();
    SPIFFS.end();
    
    return 0;
}

static void returnFail(String msg) {
    server.send(500, "text/plain", msg + "\r\n");
}

static void handleInput() {
    if (!server.hasArg("cmd")) return returnFail("Missing Input");
    //
    String cmd = server.arg("cmd");
    Serial.println(">> "+cmd);
    String rst = forth_exec(cmd);
    Serial.print(rst);              // fetch FVM outout stream
    server.setContentLength(rst.length());
    server.send(200, "text/plain; charset=utf-8", rst);
}

void setup() {
    Serial.begin(115200);
    delay(100);
  
    //  WiFi.config(ip, gateway, subnet);
    WiFi.mode(WIFI_STA);
    // attempt to connect to Wifi network:
    WiFi.begin(ssid, pass);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // if you get a connection, report back via serial:
    server.begin();
    Serial.println("Booting esp32Forth v6.3 ...");

    // Setup timer and attach timer to a led pin
    ledcSetup(0, 100, LEDC_TIMER_13_BIT);
    ledcAttachPin(5, 0);
    analogWrite(0, 250, BRIGHTNESS);
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

    // Setup web server handlers
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", index_html);
    });
    server.on("/input", HTTP_POST, handleInput);
    server.begin();
    Serial.println("HTTP server started");
    ///
    /// ForthVM initalization
    ///
    forth_vm->init();
    forth_load("/load.txt");    // compile \data\load.txt  
}

void loop(void) {
    server.handleClient();
    delay(2);                  // allow the cpu to switch to other tasks
    
    // for debugging: we can also take user input from Serial Monitor
    if (Serial.available()) {
        String cmd = Serial.readString();
        Serial.print(forth_exec(cmd));   // check! might not be thread-safe
    }
}
