/******************************************************************************/
/* ESP32 ceForth, Version 8.5                                                 */
/******************************************************************************/
#include <sstream>      // iostream, stringstream
#include <string>       // string class
#include <iomanip>      // setw, setbase, ...
using namespace std;
typedef uint8_t    U8;
typedef uint32_t   U32;

#define FALSE      0
#define TRUE       -1
#define LOGICAL    ? -1 : 0
#define pop        top = stack[(U8)S--]
#define push       stack[(U8)++S] = top; top =
#define popR       rack[(U8)R--]
#define pushR      rack[(U8)++R]
#define ALIGN(sz)  ((sz) + (-(sz) & 0x3))
#define IMMD_FLAG  0x80
#define analogWrite(c,v,mx) ledcWrite((c),(8191/mx)*min((int)(v),mx))
///
/// translate ESP32 String to/from Forth input/output streams (in C++ string)
///
#define LOGF(s)    Serial.print(F(s))
#define LOG(v)     Serial.print(v)
#define ENDL       endl; LOG(fout.str().c_str()); fout.str("")

istringstream fin;      /// ForthVM input stream
ostringstream fout;     /// ForthVM output stream
///
/// ForthVM global variables
///

int rack[256] = { 0 };
int stack[256] = { 0 };
int top;
U8  R = 0, S = 0, bytecode;
U32 P, IP, WP, nfa;
U32 DP, thread, context;
int ucase = 1, compile = 0, base = 16;
string idiom;

U32 iData[16000] = {};
U8  *cData = (U8*)iData;

void next()       { P = iData[IP >> 2]; WP = P; IP += 4; }
void nest()       { pushR = IP; IP = WP + 4; next(); }
void unnest()     { IP = popR; next(); }
void comma(int n) { iData[DP >> 2] = n; DP += 4; }
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
        int lfa = nfa - 4;
        if (len_s == len) {
            bool ok = true;
            const char *c = (const char*)&cData[nfa+1], *p = s.c_str();
            for (int i=0; ok && i<len; i++, c++, p++) {
                ok = (ucase && *c > 0x40)
                    ? ((*c & 0x5f) == (*p & 0x5f))
                    : (*c == *p);
            }
            if (ok) return ALIGN(nfa + len + 1);
        }
        nfa = iData[lfa >> 2];
    }
    return 0;
}
void words() {
    int n = 0;
    nfa = context; // CONTEXT
    while (nfa) {
        int len = (int)cData[nfa] & 0x1f;
        int lfa = nfa - 4;
        for (int i = 0; i < len; i++)
            fout << cData[nfa + 1 + i];
        fout << ((++n % 10 == 0) ? '\n' : ' ');
        nfa = iData[lfa >> 2];
    }
    fout << ENDL;
}
void CheckSum() {                            // CC: P updated, but used as a local variable
    pushR = P; char sum = 0;
    fout << setw(4) << setbase(16) << P << ": ";
    for (int i = 0; i < 16; i++) {
        sum += cData[P];
        fout << setw(2) << (int)cData[P++] << ' ';
    }
    fout << setw(4) << (sum & 0XFF) << "  ";
    P = popR;
    for (int i = 0; i < 16; i++) {
        sum = cData[P++] & 0x7f;
        fout << (char)((sum < 0x20) ? '_' : sum);
    }
    fout << ENDL;
}
void dump() {// a n --                       // CC: P updated, but used as a local variable
    fout << ENDL;
    int len = top / 16; pop; P = top; pop;
    for (int i = 0; i < len; i++) { CheckSum(); }
}
void ss_dump() {
    fout << "< "; for (int i = S - 4; i < S + 1; i++) { fout << stack[i] << " "; }
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
    CODE("dostr",
         int p = IP; push p; int len = cData[p];
         p += (len + 1); p += (-p & 3); IP = p),
    CODE("dotstr",
         int p = IP; int len = cData[p++];
         for (int i = 0; i < len; i++) fout << cData[p++];
         p += (-p & 3); IP = p),
    CODE("dolit", push iData[IP >> 2]; IP += 4),
    CODE("dovar", push WP + 4),
    IMMD("[",     compile = 0),
    CODE("]",     compile = 1),
    IMMD("(",     next_idiom(')')),
    IMMD(".(",    fout << next_idiom(')')),
    IMMD("\\",    next_idiom('\n')),
    IMMD("$*",    comma_s(find("dostr"), next_idiom('"'))),
    IMMD(".\"",   comma_s(find("dotstr"), next_idiom('"'))),
    /// Branching ops
    CODE("branch", IP = iData[IP >> 2]; next()),
    CODE("0branch",
         if (top == 0) IP = iData[IP >> 2];
         else IP += 4;  pop; next()),
    CODE("donext",
         if (rack[R]) {
             rack[R] -= 1; IP = iData[IP >> 2];
         }
         else { IP += 4;  R--; }
         next()),
    IMMD("if",    comma(find("0branch")); push DP; comma(0)),
    IMMD("else",
         comma(find("branch")); iData[top >> 2] = DP + 4;
         top = DP; comma(0)),
    IMMD("then", iData[top >> 2] = DP; pop),
    /// Loops
    IMMD("begin",  push DP),
    IMMD("while", comma(find("0branch")); push DP; comma(0)),
    IMMD("repeat",
         comma(find("branch")); int n = top; pop;
         comma(top); pop; iData[n >> 2] = DP),
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
    CODE("docon", push iData[(WP + 4) >> 2]),
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
    CODE("@",  top = iData[top >> 2]),
    CODE("!",  int a = top; pop; iData[a >> 2] = top; pop),
    CODE("?",  fout << iData[top >> 2] << " "; pop),
    CODE("+!", int a = top; pop; iData[a >> 2] += top; pop),
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
         iData[(n + 4) >> 2] = top; pop),
    CODE("is",                         // w -- , execute only
         int n = find(next_idiom());
         iData[n >> 2] = top; pop),
    CODE("[to]",
         int n = iData[IP >> 2]; iData[(n + 4) >> 2] = top; pop),
    /// Debug ops
    CODE("bye",   exit(0)),
    CODE("here",  push DP),
    CODE("words", words()),
    CODE("dump",  dump()),
    CODE("'" ,    push find(next_idiom())),
    CODE("see",
         int n = find(next_idiom());
         for (int i = 0; i < 20; i++) fout << iData[(n >> 2) + i];
         fout << ENDL),
    CODE("ucase", ucase = top; pop),
    CODE("clock", push millis()),
    CODE("boot",  DP = find("boot") + 4; thread = nfa)
};
// Macro Assembler
void encode(struct Code* prim) {
    string seq = prim->name;
    int immd = prim->immd;
    int len  = seq.length();
    comma(thread);                   /// lfa: link field (U32)
    thread = DP;
    cData[DP++] = len | immd;        /// nfa: word length + immediate bit
    for (int i = 0; i < len; i++) { cData[DP++] = seq[i]; }
    while (DP & 3) { cData[DP++] = 0; }
    comma(P++);                      /// cfa: sequential bytecode (U32 now)
    fout << P - 1 << ':' << DP - 4 << ' ' << seq << ' ';
}
void run(int n) { /// inner interpreter, P, WP, IP, R, bytecode modified
    P = n; WP = n; IP = 0; R = 0;
    do {
        bytecode = cData[P++];
        primitives[bytecode].xt();  /// execute colon
    } while (R != 0);
}
void forth_outer(const char *cmd) {
    fin.clear();                    /// clear input stream error bit if any
    fin.str(cmd);                   /// feed user command into input stream
    fout.str("");                   /// clean output buffer, ready for next run
    fout << setbase(base);
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
/// ForthVM front-end handlers
///==========================================================================
void forth_init() {
    cData = (unsigned char*)iData;
    IP = 0; thread = 0; P = 0;
    S = 0; R = 0;
    fout << "Build dictionary" << setbase(16) << ENDL;
    for (int i = 0; i < sizeof(primitives) / sizeof(Code); i++) encode(&primitives[i]);
    context = DP - 12;
    fout << "\nPointers DP=" << DP << " Link=" << context << " Words=" << P << ENDL;
    // dump dictionary
    fout << "\nDump dictionary\n" << setbase(16);
    P = 0;
    for (int len = 0; len < 100; len++) { CheckSum(); }
    // Boot Up
    P = 0; WP = 0; IP = 0; S = 0; R = 0;
    top = -1;
    fout << "\nesp32forth_85\n" << setbase(16);
    words();
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

    forth_init();
    mem_stat();
    console_cmd.reserve(16000);
    idiom.reserve(256);
}
void loop(void) {
    if (Serial.available()) {
        console_cmd = Serial.readString();
        LOG(console_cmd);
        forth_outer(console_cmd.c_str());
        mem_stat();
        delay(2);
    }
}
