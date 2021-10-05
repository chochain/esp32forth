/******************************************************************************/
/* ESP32 ceForth, Version 8.5                                                 */
/******************************************************************************/
#include <sstream>      // iostream, stringstream
#include <string>       // string class
#include <iomanip>      // setw, setbase, ...
#include "SPIFFS.h"     // flash memory
#include <WebServer.h>
using namespace std;
typedef uint8_t    U8;
typedef uint32_t   U32;

#define IMMD_FLAG  0x80
#define FALSE      0
#define TRUE       -1
#define LOGICAL    ? TRUE : FALSE
#define pop        top = stack[(U8)S--]
#define push       stack[(U8)++S] = top; top =
#define popR       rack[(U8)R--]
#define pushR      rack[(U8)++R]
#define ALIGN(sz)  ((sz) + (-(sz) & 0x3))
#define analogWrite(c,v,mx) ledcWrite((c),(8191/mx)*min((int)(v),mx))
#define PEEK(a)    (U32)(*(U32*)((uintptr_t)(a)))
#define POKE(a, c) (*(U32*)((uintptr_t)(a))=(U32)(c))
///
/// translate ESP32 String to/from Forth input/output streams (in C++ string)
///
#define LOGF(s)    Serial.print(F(s))
#define LOG(v)     Serial.print(v)
#define LOGH(v)    Serial.print(v, HEX)
#define ENDL       endl; yield()

istringstream fin;      /// ForthVM input stream
ostringstream fout;     /// ForthVM output stream
///
/// ForthVM global variables
///   Note: the iData macro increase 1M throughput by 8%
///
int rack[256] = { 0 };
int stack[256] = { 0 };
U8  cData[1024*64] = {};           // 64K
#define iData(a)  *(U32*)&cData[a]

int top;
U8  R = 0, S = 0, bytecode;
U32 P, IP, WP, nfa;
U32 DP, thread, context;
int ucase = 1, compile = 0, base = 16;
string idiom;                      // ForthVM input buffer

void next()       { P = iData(IP); WP = P; IP += 4; }
void nest()       { pushR = IP; IP = WP + 4; next(); }
void unnest()     { IP = popR; next(); }
void comma(int n) { iData(DP) = n; DP += 4; }
void comma_s(int lex, string s) {
    comma(lex);
    int len = cData[DP++] = s.length();
    for (int i = 0; i < len; i++) { cData[DP++] = s[i]; }
    while (DP & 3) { cData[DP++] = 0; }
}
string next_idiom(char delim = 0) {
    delim ? getline(fin, idiom, delim) : fin >> idiom; return idiom;
}
void dot_r(int n, int v) {
    fout << setw(n) << setfill(' ') << v;
}
int find(string s) {            // scan dictionary, return cfa found or 0
    int len_s = s.length();
    nfa = context;
    while (nfa) {
        int len = (int)cData[nfa] & 0x1f;
        if (len_s == len) {
            bool ok = true;
            const char *c = (const char*)&cData[nfa+1], *p = s.c_str();
            for (int i=0; ok && i<len; i++, c++, p++) {
                ok = (ucase && *c > 0x40)
                    ? ((*c & 0x5f) == (*p & 0x5f))
                    : (*c == *p);
            }
            if (ok) {
                yield();
                return ALIGN(nfa + len + 1);
            }
        }
        nfa = iData(nfa - 4);           // link field to previous word
    }
    yield();
    return 0;
}
void words() {
    int n = 0;
    nfa = context;                     // CONTEXT
    fout << ENDL;
    while (nfa) {
        int len = (int)cData[nfa] & 0x1f;
        for (int i = 0; i < len; i++)
            fout << cData[nfa + 1 + i];
        if ((++n % 10) == 0) { fout << ENDL; }
        else                 { fout << ' ';  }
        nfa = iData(nfa - 4);         // link field to previous word
    }
    fout << ENDL;
}
void dump(int a, int n) {
    fout << setbase(16) << ENDL;
    for (int r = 0, sz = ((n+15)/16); r < sz; r++) {
        int p0 = a + r * 16;
        char sum = 0;
        fout <<setw(4) << p << ": ";
        for (int p = p0, i = 0; i < 16; i++, p++) {
            sum += cData[p];
            fout <<setw(2) << (int)cData[p] << ' ';
        }
        fout << setw(4) << (sum & 0xff) << "  ";
        for (int p = p0, i = 0; i < 16; i++, p++) {
            sum = cData[p] & 0x7f;
            fout <<(char)((sum < 0x20) ? '_' : sum);
        }
        fout << ENDL;
    }
    fout << setbase(base) << ENDL;
}
void ss_dump() {
    fout << "< "; for (int i = 0; i < 5; i++) fout << stack[(U8)(S - 4 + i)] << " ";
    fout << top << " >ok" << ENDL;
}
///
/// Code - data structure to keep primitive definitions
///
struct Code{
    string name;
    void   (*xt)(void);
    int    immd;
} ;
#define CODE(s, g)  { s, []{ g; }, 0 }
#define IMMD(s, g)  { s, []{ g; }, IMMD_FLAG }
static struct Code primitives[] = {
    /// Execution control ops
    CODE("ret",   next()),
    CODE("nop",   {}),
    CODE("nest",  nest()),
    CODE("unnest",unnest()),
    /// Stack ops
    CODE("dup",   stack[++S] = top),
    CODE("drop",  pop),
    CODE("over",  push stack[(S - 1)]),
    CODE("swap",  int n = top; top = stack[S]; stack[S] = n),
    CODE("rot",
         int n = stack[(S - 1)];
         stack[(S - 1)] = stack[S];
         stack[S] = top; top = n),
    CODE("pick",  top = stack[(S - top)]),
    CODE(">r",    rack[++R] = top; pop),
    CODE("r>",    push rack[R--]),
    CODE("r@",    push rack[R]),
    /// Stack ops - double
    CODE("2dup",  push stack[(S - 1)]; push stack[(S - 1)]),
    CODE("2drop", pop; pop),
    CODE("2over", push stack[(S - 3)]; push stack[(S - 3)]),
    CODE("2swap",
         int n = top; pop; int m = top; pop; int l = top; pop; int i = top; pop;
         push m; push n; push i; push l),
    /// ALU ops
    CODE("+",     int n = top; pop; top += n),
    CODE("-",     int n = top; pop; top -= n),
    CODE("*",     int n = top; pop; top *= n),
    CODE("/",     int n = top; pop; top /= n),
    CODE("mod",   int n = top; pop; top %= n),
    CODE("*/",
         int n = top; pop; int m = top; pop;
         int l = top; pop; push(m * l) / n),
    CODE("/mod",
         int n = top; pop; int m = top; pop;
         push(m % n); push(m / n)),
    CODE("*/mod",
         int n = top; pop; int m = top; pop;
         int l = top; pop; push((m * l) % n); push((m * l) / n)),
    CODE("and",   top &= stack[S--]),
    CODE("or",    top |= stack[S--]),
    CODE("xor",   top ^= stack[S--]),
    CODE("abs",   top = abs(top)),
    CODE("negate",top = -top),
    CODE("max",   int n = top; pop; top = max(top, n)),
    CODE("min",   int n = top; pop; top = min(top, n)),
    CODE("2*",    top *= 2),
    CODE("2/",    top /= 2),
    CODE("1+",    top += 1),
    CODE("1-",    top += -1),
    /// Logic ops
    CODE("0=",    top = (top == 0) LOGICAL),
    CODE("0<",    top = (top < 0) LOGICAL),
    CODE("0>",    top = (top > 0) LOGICAL),
    CODE("=",     int n = top; pop; top = (top == n) LOGICAL),
    CODE(">",     int n = top; pop; top = (top > n) LOGICAL),
    CODE("<",     int n = top; pop; top = (top < n) LOGICAL),
    CODE("<>",    int n = top; pop; top = (top != n) LOGICAL),
    CODE(">=",    int n = top; pop; top = (top >= n) LOGICAL),
    CODE("<=",    int n = top; pop; top = (top <= n) LOGICAL),
    /// IO ops
    CODE("base@", push base),
    CODE("base!", base = top; pop; fout << setbase(base)),
    CODE("hex",   base = 16; fout << setbase(base)),
    CODE("decimal", base = 10; fout << setbase(base)),
    CODE("cr",    fout << ENDL),
    CODE(".",     fout << top << " "; pop),
    CODE(".r",    int n = top; pop; dot_r(n, top); pop),
    CODE("u.r",   int n = top; pop; dot_r(n, abs(top)); pop),
    CODE(".s",    ss_dump()),
    CODE("key",   push(next_idiom()[0])),
    CODE("emit",  char b = (char)top; pop; fout << b),
    CODE("space", fout << " "),
    CODE("spaces",int n = top; pop; for (int i = 0; i < n; i++) fout << " "),
    /// Literal ops
    CODE("dostr", push IP; IP = ALIGN(IP + cData[IP] + 1)),
    CODE("dotstr", 
         int len = cData[IP++];
         for (int i = 0; i < len; i++) fout << cData[IP++];
         IP = ALIGN(IP)),
    CODE("dolit", push iData(IP); IP += 4),
    CODE("dovar", push WP + 4),
    IMMD("[",     compile = 0),
    CODE("]",     compile = 1),
    IMMD("(",     next_idiom(')')),
    IMMD(".(",    fout << next_idiom(')')),
    IMMD("\\",    next_idiom('\n')),
    IMMD("$*",    comma_s(find("dostr"), next_idiom('"'))),
    IMMD(".\"",   comma_s(find("dotstr"), next_idiom('"'))),
    /// Branching ops
    CODE("branch", IP = iData(IP); next()),
    CODE("0branch",
         if (top == 0) IP = iData(IP);
         else IP += 4;  pop; next()),
    CODE("donext",
         if (rack[R]) {
             rack[R] -= 1; IP = iData(IP);
         }
         else { IP += 4;  R--; }
         next()),
    IMMD("if",    comma(find("0branch")); push DP; comma(0)),
    IMMD("else",
         comma(find("branch")); iData(top) = DP + 4;
         top = DP; comma(0)),
    IMMD("then", iData(top) = DP; pop),
    /// Loops
    IMMD("begin",  push DP),
    IMMD("while", comma(find("0branch")); push DP; comma(0)),
    IMMD("repeat",
         comma(find("branch")); int n = top; pop;
         comma(top); pop; iData(n) = DP),
    IMMD("again", comma(find("branch")); comma(top); pop),
    IMMD("until", comma(find("0branch")); comma(top); pop),
    ///  For loops
    IMMD("for", comma((find(">r"))); push DP),
    IMMD("aft",
         pop;
         comma((find("branch"))); comma(0); push DP; push DP - 4),
    IMMD("next",  comma(find("donext")); comma(top); pop),
    ///  Compiler ops
    CODE("exit",  IP = popR; next()),
    CODE("docon", push iData(WP + 4)),
    CODE(":",
         thread = DP + 4; comma_s(context, next_idiom());
         comma(cData[find("nest")]); compile = 1),
    IMMD(";",
         context = thread; compile = 0;
         comma(find("unnest"))),
    CODE("variable",
         thread = DP + 4; comma_s(context, next_idiom());
         context = thread;
         comma(cData[find("dovar")]); comma(0)),
    CODE("constant",
         thread = DP + 4; comma_s(context, next_idiom());
         context = thread;
         comma(cData[find("docon")]); comma(top); pop),
    CODE("@",  top = iData(top)),
    CODE("!",  int a = top; pop; iData(a) = top; pop),
    CODE("?",  fout << iData(top) << " "; pop),
    CODE("+!", int a = top; pop; iData(a) += top; pop),
    CODE("allot",
         int n = top; pop;
         for (int i = 0; i < n; i++) cData[DP++] = 0),
    CODE(",",  comma(top); pop),
    /// metacompiler
    CODE("create",
         thread = DP + 4; comma_s(context, next_idiom());
         context = thread;
         comma(find("nest")); comma(find("dovar"))),
    CODE("does", comma(find("nest"))), // copy words after "does" to new the word
    CODE("to",                         // n -- , compile only
         int n = find(next_idiom());
         iData(n + 4) = top; pop),
    CODE("is",                         // w -- , execute only
         int n = find(next_idiom());
         iData(n) = top; pop),
    CODE("[to]",
         int n = iData(IP); iData(n + 4) = top; pop),
    /// Debug ops
    CODE("bye",   exit(0)),
    CODE("here",  push DP),
    CODE("words", words()),
    CODE("dump",  int n = top; pop; int a = top; pop; dump(a, n)),
    CODE("'" ,    push find(next_idiom())),
    CODE("see",
         int n = find(next_idiom());
         for (int i = 0; i < 80; i+=4) fout << iData(n + i);
         fout << ENDL),
    CODE("ucase", ucase = top; pop),
    CODE("clock", push millis()),
    CODE("peek",  int a = top; pop; push PEEK(a)),
    CODE("poke",  int a = top; pop; POKE(a, top); pop),
    CODE("delay", delay(top); pop),
    /// Arduino specific ops
    CODE("pin",   int p = top; pop; pinMode(p, top); pop),
    CODE("in",    int p = top; pop; push digitalRead(p)),
    CODE("out",   int p = top; pop; digitalWrite(p, top); pop),
    CODE("adc",   int p = top; pop; push analogRead(p)),
    CODE("duty",  int p = top; pop; analogWrite(p, top, 255); pop),
    CODE("attach",int p = top; pop; ledcAttachPin(p, top); pop),
    CODE("setup", int ch = top; pop; int freq=top; pop; ledcSetup(ch, freq, top); pop),
    CODE("tone",  int ch = top; pop; ledcWriteTone(ch, top); pop),
    CODE("boot",  DP = find("boot") + 4; context = nfa)
};
// Macro Assembler
void encode(struct Code* prim) {     /// DP, thread, and P updated
    string seq = prim->name;
    int immd = prim->immd;
    int len  = seq.length();
    comma(thread);                   /// lfa: link field (U32)
    thread = DP;
    cData[DP++] = len | immd;        /// nfa: word length + immediate bit
    for (int i = 0; i < len; i++) { cData[DP++] = seq[i]; }
    while (DP & 3) { cData[DP++] = 0; }
    comma(P++);                      /// cfa: sequential bytecode (U32 now)
}
void run(int n) { /// inner interpreter, P, WP, IP, R, bytecode modified
    P = n; WP = n; IP = 0; R = 0;
    do {
        bytecode = cData[P++];       /// fetch bytecode
        primitives[bytecode].xt();   /// execute colon byte-by-byte
    } while (R != 0);
}
void forth_init() {
    DP = thread = P = 0; S = R = 0;
    for (int i = 0; i < sizeof(primitives) / sizeof(Code); i++) {
        encode(&primitives[i]);
    }
    context = DP - 12;               /// lfa: 12 = ALIGN("boot"+1)+sizeof(U32)
    LOG("\nDP=");     LOGH(DP);
    LOG(" link=");    LOGH(context);
    LOG(" Words=");   LOGH(P);
    LOG("\n");
    // Boot Up
    P = WP = IP = 0;
    top = -1;
    fout << setbase(base);
}
void forth_outer() {
    while (fin >> idiom) {
        int cfa = find(idiom);
        if (cfa) {
            if (compile && ((cData[nfa] & IMMD_FLAG) == 0))
                comma(cfa);
            else run(cfa);
        }
        else {
            char* p;
            int n = (int)strtol(idiom.c_str(), &p, base);
            if (*p != '\0') {                   ///  not number
                fout << idiom << "? " << ENDL;  ///  display error prompt
                compile = 0;                    ///  reset to interpreter mode
                getline(fin, idiom, '\n');      ///  skip the entire line
            }
            else {
                if (compile) { comma(find("dolit")); comma(n); }
                else { push n; }
            }
        }
    }
    if (!compile) ss_dump();      /// dump stack and display ok prompt
}
///==========================================================================
/// ESP32 Web Serer connection and index page
///==========================================================================
//const char *ssid = "Sonic-6af4";
//const char *pass = "7b369c932f";
const char *ssid = "Frontier7008";
const char *pass = "8551666595";

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
         background-color:#f8f0f0;'>esp32Forth v8</div>
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
    req = ary.slice(0,30).join('\n')            // 30*(average 50 byte/line) ~= 1.5K
    if (req=='' || d>20) return                 // bail looping, just in case
    log.innerHTML+='<font color=blue>'+req.replace(/\n/g, '<br/>')+'</font>'
    httpPost('/input', { cmd: req }, rsp=>{
        if (rsp !== null) {
            log.innerHTML += rsp.replace(/\n/g, '<br/>').replace(/\s/g,'&nbsp;')
            log.scrollTop=log.scrollHeight      // scroll down
            chunk(ary.splice(30), d+1) }})}     // next 30 lines
function forth() {
    let str = tib.value.replace(/\\.*\n/g,'').split(/(\(\s[^\)]+\))/)
    let cmd = str.map(v=>v[0]=='(' ? v.replaceAll('\n',' ') : v).join('')
    chunk(cmd.split('\n'), 1); tib.value = '' }
window.onload = ()=>{ tib.focus() }
</script></html>
)XX";
///==========================================================================
/// ForthVM front-end handlers
///==========================================================================
///
/// translate ESP32 String to/from Forth input/output streams (in C++ string)
///
static String process_command(String cmd) {
    fout.str("");                       // clean output buffer, ready for next run
    fin.clear();                        // clear input stream error bit if any
    fin.str(cmd.c_str());               // feed user command into input stream
    forth_outer();                      // invoke outer interpreter
    return String(fout.str().c_str());  // return response as a String object
}
///
/// Forth bootstrap loader (from Flash)
///
static int forth_load(const char *fname) {
    if (!SPIFFS.begin()) {
        LOG("Error mounting SPIFFS\n"); return 1; }
    File file = SPIFFS.open(fname, "r");
    if (!file) {
        LOG("Error opening file:"); LOG(fname); return 1; }
    LOG("Loading file: /data"); LOG(fname); LOG("...\n");
    while (file.available()) {
        // retrieve command from Flash memory
        String cmd = file.readStringUntil('\n');
        LOG("<< "+cmd+"\n");  // display bootstrap command on console
        // send it to Forth command processor
        process_command(cmd);
    }
    LOG("Done loading.\n");
    file.close();
    SPIFFS.end();
    return 0;
}
///
/// memory statistics dump - for heap and stack debugging
///
static void mem_stat() {
    LOGF("Core:");           LOG(xPortGetCoreID());
    LOGF(" heap[maxblk=");   LOG(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    LOGF(", avail=");        LOG(heap_caps_get_free_size(MALLOC_CAP_8BIT));
    LOGF(", pmem=");         LOG(context);
    LOGF("], lowest[heap="); LOG(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    LOGF(", stack=");        LOG(uxTaskGetStackHighWaterMark(NULL));
    LOGF("]\n");
    if (!heap_caps_check_integrity_all(true)) {
//        heap_trace_dump();     // dump memory, if we have to
        abort();                 // bail, on any memory error
    }
}
///==========================================================================
/// Web Server handlers
///==========================================================================
static void handleInput() {
    // receive POST from web browser
    if (!server.hasArg("cmd")) { // make sure parameter contains "cmd" property
        server.send(500, "text/plain", "Missing Input\r\n");
        return;
    }
    // retrieve command from web server
    String cmd = server.arg("cmd");
    LOG("\n>> "+cmd+"\n");       // display requrest on console
    // send requrest command to Forth command processor, and receive response
    String rsp = process_command(cmd);
    LOG(rsp);                    // display response on console
    mem_stat();
    // send response back to web browser
    server.setContentLength(rsp.length());
    server.send(200, "text/plain; charset=utf-8", rsp);
}
///==========================================================================
/// ESP32 routines
///==========================================================================
String console_cmd;          /// ESP32 input buffer
void setup() {
    Serial.begin(115200);
    delay(100);
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
    analogWrite(0, 0, 255);
    //  WiFi.config(ip, gateway, subnet);
    WiFi.mode(WIFI_STA);
    // attempt to connect to Wifi network:
    WiFi.begin(ssid, pass);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        LOG("."); }
    LOG("WiFi connected, IP Address: ");
    LOG(WiFi.localIP());
    server.begin();
    // Setup web server handlers
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", index_html); });
    server.on("/input", HTTP_POST, handleInput);
    LOG("\nHTTP server started\n");
    ///
    /// ForthVM initalization
    ///
    console_cmd.reserve(16000);
    idiom.reserve(256);
    forth_init();
    forth_load("/load.txt");
    mem_stat();
    LOG("\nesp32Forth v8.5\n");
}
void loop(void) {
    server.handleClient(); // ESP32 handle web requests
    delay(2);              // yield to background tasks (interrupt, timer,...)
    if (Serial.available()) {
        console_cmd = Serial.readString();
        LOG(console_cmd);
        LOG(process_command(console_cmd));
        mem_stat();
        delay(2);
    }
}
