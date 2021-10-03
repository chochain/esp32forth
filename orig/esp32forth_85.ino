/******************************************************************************/
/* esp32Forth, Version 8 : for NodeMCU ESP32S                                 */
/******************************************************************************/
#include <stdlib.h>     // strtol
#include <exception>    // try...catch, throw (disable for less capable MCU)
#include "SPIFFS.h"     // flash memory
#include <WiFi.h>
#include <sstream>      // iostream, stringstream
#include <iomanip>      // setbase
#include <string>       // string class
using namespace std;
///
/// translate ESP32 String to/from Forth input/output streams (in C++ string)
///
#define LOGF(s)  Serial.print(F(s))
#define LOG(v)   Serial.print(v)
#define LOGH(v)  Serial.print(v, HEX)

istringstream   fin;    // forth_in
ostringstream   fout;   // forth_out
void (*fout_cb)(const char*);  // forth output callback function

#define ENDL       endl; fout_cb(fout.str().c_str()); fout.str("")
#define FALSE      0
#define TRUE       -1
#define BOOL(f)    ((f) ? TRUE : FALSE)
#define ALIGN(sz)  ((sz) + (-(sz) & 0x3))
#define IMMD_FLAG  0x80

int rack[256] = { 0 };
int stack[256] = { 0 };
unsigned char R = 0, S = 0, bytecode, c;	// CC: bytecode should be U32, c is not used
int* Pointer;								// CC: Pointer is unused
int  P, IP, WP, top, len;					// CC: len can be local
int  lfa, nfa, cfa, pfa;					// CC: pfa is unused; lfa, cfa can be local
int  DP, lnk, context;						// CC: DP, context and lnk usage are interchangable
int  ucase = 1, compile = 0, base = 16;
string idiom, s;                            // CC: s is unused; idiom can be local

uint32_t data[1024] = {};
uint8_t  *cData = (uint8_t*)data;

inline int  popR()       { return rack[(unsigned char)R--]; }
inline void pushR(int v) { rack[(unsigned char)++R] = v; }
inline int  pop()        { int n = top; top = stack[(unsigned char)S--]; return n; }
inline void push(int v)  { stack[(unsigned char)++S] = top; top = v; }
inline void next()       { P = data[IP >> 2]; WP = P; IP += 4; }
inline void nest()       { pushR(IP); IP = WP + 4; next(); }
inline void unnest()     { IP = popR(); next(); }
inline void comma(int n) { data[DP >> 2] = n; DP += 4; }
void comma_s(int lex, string s) {
	comma(lex); len = s.length(); cData[DP++] = len;
	for (int i = 0; i < len; i++) { cData[DP++] = s[i]; }
	while (DP & 3) { cData[DP++] = 0; }
}

string next_idiom(char delim = 0) {
	string s; delim ? getline(fin, s, delim) : fin >> s; return s;
}
void dot_r(int n, int v) {
	fout << setw(n) << setfill(' ') << v;
}
int find(string s) {						// CC: nfa, lfa, cfa, len modified
	int len_s = s.length();
	nfa = context;
	while (nfa) {
        lfa = nfa - 4;           				// CC: 4 = sizeof(IU)
        len = (int)cData[nfa] & 0x1f;			// CC: 0x1f = ~IMMD_FLAG
        if (len_s == len) {
            int success = 1;                    // CC: memcmp
            for (int i = 0; i < len; i++) {
                if (s[i] != cData[nfa + 1 + i])
                {
                    success = 0; break;
                }
            }
            if (success) { return cfa = ALIGN(nfa + len + 1); }
        }
        nfa = data[lfa >> 2];
	}
	return 0;
}
void words() {
	int n = 0;
	nfa = context; // CONTEXT
	while (nfa) {
        lfa = nfa - 4;
        len = (int)cData[nfa] & 0x1f;
        for (int i = 0; i < len; i++)
            fout << cData[nfa + 1 + i];
        if ((++n%10)==0) { fout << ENDL; } 
        else             { fout << ' ';  }
        nfa = data[lfa >> 2];
	}
	fout << ENDL;
}
void CheckSum() {                            // CC: P updated, but used as a local variable
    char sum = 0;
    pushR(P);
	fout << setw(4) << setbase(16) << P << ": ";
	for (int i = 0; i < 16; i++) {
        sum += cData[P];
        fout << setw(2) << (int)cData[P++] << ' ';
	}
	fout << setw(4) << (sum & 0xff) << "  ";
	P = popR();
	for (int i = 0; i < 16; i++) {
        sum = cData[P++] & 0x7f;
        fout << (char)((sum < 0x20) ? '_' : sum);
	}
	fout << ENDL;
}
void dump() {  // a n --                       // CC: P updated, but used as a local variable
    int sz = pop() / 16;
    P = pop();
	fout << ENDL;
	for (int i = 0; i < sz; i++) { CheckSum(); }
}
void ss_dump() {
	fout << "< "; for (int i = S - 4; i < S + 1; i++) { fout << stack[i] << " "; }
	fout << top << " >ok" << ENDL;
}

// Macro Assembler
///
/// Code - data structure to keep primitive definitions
///
struct Code {
    string name;
    void   (*xt)(void);
    int    immd;
};
#define CODE(s, g)  { s, []{ g; }, 0 }
#define IMMD(s, g)  { s, []{ g; }, IMMD_FLAG }
static struct Code primitives[] PROGMEM = {
    /// Execution flow ops
	CODE("ret",   next()),
	CODE("nop",   {}),
	CODE("nest",  nest()),
	CODE("unnest",unnest()),
	/// Stack ops
	CODE("dup",   stack[++S] = top),
	CODE("drop",  pop()),
	CODE("over",  push(stack[(S - 1)])),
	CODE("swap",  int n = top; top = stack[S]; stack[S] = n),
	CODE("rot",
         int n = stack[(S - 1)];
         stack[(S - 1)] = stack[S];
         stack[S] = top; top = n),
	CODE("pick",  top = stack[(S - top)]),
	CODE(">r",    rack[++R] = pop()),
	CODE("r>",    push(rack[R--])),
	CODE("r@",    push(rack[R])),
	/// Stack ops - double
	CODE("2dup",  push(stack[(S - 1)]); push(stack[(S - 1)])),
	CODE("2drop", pop; pop),
	CODE("2over", push(stack[(S - 3)]); push(stack[(S - 3)])),
	CODE("2swap", 
         int n = pop(); int m = pop(); int l = pop(); int i = pop();
         push(m); push(n); push(i); push(l)),
	/// ALU ops
	CODE("+",     int n = pop(); top += n),
	CODE("-",     int n = pop(); top -= n),
	CODE("*",     int n = pop(); top *= n),
	CODE("/",     int n = pop(); top /= n),
	CODE("mod",   int n = pop(); top %= n),
	CODE("*/",
         int n = pop(); int m = pop(); int l = pop();
         push(m*l/n)),
	CODE("/mod",
         int n = pop(); int m = pop();
         push(m % n); push(m / n)),
	CODE("*/mod",
         int n = pop(); int m = pop();
         int l = pop(); push((m * l) % n); push((m * l) / n)),
	CODE("and",   top &= stack[S--]),
	CODE("or",    top |= stack[S--]),
	CODE("xor",   top ^= stack[S--]),
	CODE("abs",   top = abs(top)),
	CODE("negate",top = -top),
	CODE("max",   int n = pop(); top = max(top, n)),
	CODE("min",   int n = pop(); top = min(top, n)),
	CODE("2*",    top *= 2),
	CODE("2/",    top /= 2),
	CODE("1+",    top += 1),
	CODE("1-",    top -= 1),
	/// Logic ops
	CODE("0=",    top = BOOL(top == 0)),
    CODE("0<",    top = BOOL(top < 0)),
	CODE("0>",    top = BOOL(top > 0)),
	CODE("=",     int n = pop(); top = BOOL(top == n)),
	CODE(">",     int n = pop(); top = BOOL(top > n)),
	CODE("<",     int n = pop(); top = BOOL(top < n)),
	CODE("<>",    int n = pop(); top = BOOL(top != n)),
	CODE(">=",    int n = pop(); top = BOOL(top >= n)),
	CODE("<=",    int n = pop(); top = BOOL(top <= n)),
	/// IO ops
	CODE("base@", push(base)),
	CODE("base!", base = pop(); fout << setbase(base)),
	CODE("hex",   base = 16; fout << setbase(base)),
	CODE("decimal", base = 10; fout << setbase(base)),
	CODE("cr",    fout << ENDL),
	CODE(".",     fout << top << " "; pop),
	CODE(".r",    int n = pop(); dot_r(n, top); pop),
	CODE("u.r",   int n = pop(); dot_r(n, abs(top)); pop),
	CODE(".s",    ss_dump()),
	CODE("key",   push(next_idiom()[0])),
	CODE("emit",  char b = (char)pop(); fout << b),
	CODE("space", fout << " "),
	CODE("spaces",int n = pop(); for (int i = 0; i < n; i++) fout << " "),
	/// Literal ops
	CODE("dostr",
         int p = IP; push(p); len = cData[p];
         p += (len + 1); p += (-p & 3); IP = p),
	CODE("dotstr",
         int p = IP; len = cData[p++];
         for (int i = 0; i < len; i++) fout << cData[p++];
         p += (-p & 3); IP = p),
	CODE("dolit", push(data[IP >> 2]); IP += 4),
	CODE("dovar", push(WP + 4)),
	IMMD("[",     compile = 0),
	CODE("]",     compile = 1),
    IMMD("(",     next_idiom(')')),
    IMMD(".(",    fout << next_idiom(')')),
    IMMD("\\",    next_idiom('\n')),
    IMMD("$*",    comma_s(find("dostr"), next_idiom('"'))),
    IMMD(".\"",   comma_s(find("dotstr"), next_idiom('"'))),
	/// Branching ops
	CODE("branch", IP = data[IP >> 2]; next()),
	CODE("0branch",
         if (top == 0) IP = data[IP >> 2];
         else IP += 4;
         pop(); next()),
	CODE("donext",
         if (rack[R]) {
             rack[R] -= 1; IP = data[IP >> 2];
         }
         else { IP += 4;  R--; }
         next()),
	IMMD("if", comma(find("0branch")); comma(0); push(DP)),
    IMMD("else",
         comma(find("branch")); data[top >> 2] = DP + 4;
         top = DP; comma(0)),
    IMMD("then", data[top >> 2] = DP; pop()),
	/// Loops
	IMMD("begin",  push(DP)),
    IMMD("while",  comma(find("0branch")); comma(0); push(DP)),
    IMMD("repeat",
         comma(find("branch")); int n = pop();
         comma(pop()); data[n >> 2] = DP),
	IMMD("again",  comma(find("branch")); comma(pop())),
    IMMD("until",  comma(find("0branch")); comma(pop())),
	///  For loops
	IMMD("for", comma((find(">r"))); push(DP)),
	IMMD("aft",
         pop();
         comma((find("branch"))); comma(0); push(DP); push(DP - 4)),
    IMMD("next",   comma(find("donext")); comma(pop())),
	///  Compiler ops
	CODE("exit",  IP = popR(); next()),
	CODE("docon", push(data[(WP + 4) >> 2])),
    CODE(":",
         string s = next_idiom();
         lnk = DP + 4; comma_s(context, s);
         comma(cData[find("nest")]); compile = 1),
	IMMD(";",
         context = lnk; compile = 0;
         comma(find("unnest"))),
	CODE("variable", 
         string s = next_idiom();
         lnk = DP + 4; comma_s(context, s);
         context = lnk;
         comma(cData[find("dovar")]); comma(0)),
	CODE("constant",
         string s = next_idiom();
         lnk = DP + 4; comma_s(context, s);
         context = lnk;
         comma(cData[find("docon")]); comma(pop())),
	CODE("@",  top = data[top >> 2]),
	CODE("!",  int a = pop(); data[a >> 2] = pop()),
	CODE("?",  fout << data[pop() >> 2] << " "),
	CODE("+!", int a = pop(); data[a >> 2] += pop()),
	CODE("allot",
         int n = pop();
         for (int i = 0; i < n; i++) cData[DP++] = 0),
	CODE(",",  comma(pop())),
	/// metacompiler
	CODE("create",
         string s = next_idiom();
         lnk = DP + 4; comma_s(context, s);
         context = lnk;
         comma(find("nest")); comma(find("dovar"))),
	CODE("does", comma(find("nest"))), // copy words after "does" to new the word
	CODE("to",                         // n -- , compile only
         int n = find(next_idiom());
         data[(cfa + 4) >> 2] = pop()),
	CODE("is",                         // w -- , execute only
         int n = find(next_idiom());
         data[cfa >> 2] = pop()),
	CODE("[to]",
         int n = data[IP >> 2]; data[(n + 4) >> 2] = pop()),
	/// Debug ops
	CODE("bye",   exit(0)),
	CODE("here",  push(DP)),
	CODE("words", words()),
	CODE("dump",  dump()),
	CODE("'" ,    push(find(next_idiom()))),
	CODE("see",
         int n = find(next_idiom());
         for (int i = 0; i < 20; i++) fout << data[(n >> 2) + i];
         fout << ENDL),
	CODE("ucase", ucase = pop()),
    CODE("clock", push(millis())),
    CODE("boot",  DP = find("boot") + 4; lnk = nfa)
};

void encode(struct Code *prim) {
    const char *seq = prim->name.c_str();
	int sz = prim->name.length();
	comma(lnk);                     // CC: link field (U32 now)
	lnk = DP;
	cData[DP++] = sz | prim->immd;	// CC: attribute byte = length(0x1f) + immediate(0x80)
	for (int i = 0; i < sz; i++) { cData[DP++] = seq[i]; }
	while (DP & 3) { cData[DP++] = 0; }
	comma(P++); 					/// CC: cfa = sequential bytecode (U32 now)
	LOGH(P-1); LOGF(":"); LOGH(DP-4); LOGF(" "); LOG(seq); LOG("\n");
}

void run(int n) {					/// inner interpreter, CC: P, WP, IP, R, bytecode modified
	P = n; WP = n; IP = 0; R = 0;
	do {
        bytecode = cData[P++];		/// CC: bytecode is U8, storage is U32, using P++ is incorrect
        primitives[bytecode].xt();	/// execute colon
	} while (R != 0);
}

void forth_outer(const char *cmd, void(*callback)(const char*)) {
    fin.clear();                    /// clear input stream error bit if any
    fin.str(cmd);                   /// feed user command into input stream
    fout_cb = callback;             /// setup callback function
    fout.str("");                   /// clean output buffer, ready for next run
	while (fin >> idiom) {
        if (find(idiom)) {
            if (compile && ((cData[nfa] & IMMD_FLAG)==0))
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
                else { push(n); }
            }
        }
	}
    if (!compile) ss_dump();        ///  * dump stack and display ok prompt
}

void forth_init() {
	cData = (unsigned char*)data;
	IP = 0; lnk = 0; P = 0;
	S = 0; R = 0;

	for (int i=0; i<sizeof(primitives)/sizeof(Code); i++) encode(&primitives[i]);

	context = DP - 12;

	// Boot Up
	P = 0; WP = 0; IP = 0; S = 0; R = 0;
	top = -1;
}

///==========================================================================
/// ForthVM front-end handlers
///==========================================================================
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
/// ESP32 routines
///==========================================================================
String console_cmd;
void setup() {
    Serial.begin(115200);
    delay(100);
    ///
    /// ForthVM initalization
    ///
    LOGF("\nBooting...");
    forth_init();
    mem_stat();
    console_cmd.reserve(256);
    LOGF("\nesp32forth8.4\n");
}

void loop(void) {
    ///
    /// while Web requests come in from handleInput asynchronously,
    /// we also take user input from console (for debugging mostly)
    ///
    // for debugging: we can also take user input from Serial Monitor
    static auto send_to_con = [](const char *rst) { LOG(rst); };
    if (Serial.available()) {
        console_cmd = Serial.readString();
        LOG(console_cmd);
        forth_outer(console_cmd.c_str(), send_to_con);
        mem_stat();
        delay(2);
    }
}
