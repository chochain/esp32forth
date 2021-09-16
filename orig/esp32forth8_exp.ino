/******************************************************************************/
/* esp32Forth, Version 8 : for NodeMCU ESP32S                                 */
/******************************************************************************/
#include <stdint.h>     // uintxx_t
#include <stdlib.h>     // strtol
#include <string.h>     // strcmp
#include <exception>    // try...catch, throw (disable for less capable MCU)
///
/// logical units (instead of physical) for type check and portability
///
typedef uint16_t IU;    // instruction pointer unit
typedef int32_t  DU;    // data unit
typedef uint16_t U16;   // unsigned 16-bit integer
typedef uint8_t  U8;    // byte, unsigned character
///
/// alignment macros
///
#define ALIGN(sz)       ((sz) + (-(sz) & 0x1))
#define ALIGN32(sz)     ((sz) + (-(sz) & 0x1f))
///
/// array class template (so we don't have dependency on C++ STL)
/// Note:
///   * using decorator pattern
///   * this is similar to vector class but much simplified
///   * v array is dynamically allocated due to ESP32 has a 96K hard limit
///
template<class T, int N>
struct List {
    T   *v;             /// fixed-size array storage
    int idx = 0;        /// current index of array
    
    List()  { v = new T[N]; }      /// dynamically allocate array memory
    ~List() { delete[] v;   }      /// free the memory
    T& operator[](int i)   { return i < 0 ? v[idx + i] : v[i]; }
    T pop() {
        if (idx>0) return v[--idx];
        throw "ERR: List empty";
    }
    T push(T t) {
        if (idx<N) return v[idx++] = t;
        throw "ERR: List full";
    }
    void push(T *a, int n)  { for (int i=0; i<n; i++) push(*(a+i)); }
    void merge(List& a)     { for (int i=0; i<a.idx; i++) push(a[i]);}
    void clear(int i=0)     { idx=i; }
};
///
/// functor implementation - for lambda support (without STL)
///
struct fop { virtual void operator()(IU) = 0; };
template<typename F>
struct XT : fop {           // universal functor
    F fp;
    XT(F &f) : fp(f) {}
    void operator()(IU c) { fp(c); }
};
///
/// universal Code class
/// Note:
///   * 8-byte on 32-bit machine, 16-byte on 64-bit machine
///
struct Code {
    const char *name = 0;   /// name field
    union {                 /// either a primitive or colon word
        fop *xt = 0;        /// lambda pointer
        struct {            /// a colon word
            U16 def:  1;    /// colon defined word
            U16 immd: 1;    /// immediate flag
            U16 len:  14;   /// len of pf
            IU  pf;         /// offset to pmem space (16-bit for 64K range)
        };
    };
    template<typename F>    /// template function for lambda
    Code(const char *n, F f, bool im=false) : name(n) {
        xt = new XT<F>(f);
        immd = im ? 1 : 0;
    }
    Code() {}               /// create a blank struct (for initilization)
};
///
/// main storages in RAM
/// Note:
///   1.By separating pmem from dictionary, it makes dictionary uniform size
///   * (i.e. the RISC vs CISC debate) which eliminates the need for link field
///   * however, it requires size tuning manually
///   2.For ease of byte counting, we use U8 for pmem instead of U16.
///   * this makes IP increment by 2 instead of word size. If needed, it can be
///   * readjusted.
///
List<DU,   64>      ss;   /// data stack, can reside in registers for some processors
List<DU,   64>      rs;   /// return stack
List<Code, 1024>    dict; /// fixed sized dictionary (RISC vs CISC)
List<U8,   64*1024> pmem; /// parameter memory i.e. storage for all colon definitions
///
/// system variables
///
bool compile = false;
DU   top = -1, base = 10;
IU   WP = 0, IP = 0;
///
/// dictionary search functions - can be adapted for ROM+RAM
///
int find(const char *s) {
    for (int i = dict.idx - (compile ? 2 : 1); i >= 0; --i) {
        if (strcmp(s, dict[i].name)==0) return i;
    }
    return -1;
}
///
/// colon word compiler
/// Note:
///   * we separate dict and pmem space to make word uniform in size
///   * if they are combined then can behaves similar to classic Forth
///   * with an addition link field added.
///
#define XIP  (dict[-1].len)                 /* end of pf */
void colon(const char *name) {
    const char *nfd = (const char*)&pmem[pmem.idx];      // current pmem pointer
    int sz = ALIGN(strlen(name)+1);         // IU (16-bit) alignment
    pmem.push((U8*)name, sz);               // setup raw name field
    Code c(nfd, [](int){});                 // create a new word on dictionary
    c.def = 1;                              // specify a colon word
    c.len = 0;                              // advance counter (by number of U16)
    c.pf  = pmem.idx;                       // capture code field index
    dict.push(c);                           // copy Code struct into dictionary
};
void addcode(IU c) {
    pmem.push((U8*)&c, sizeof(IU));         // add an opcode to pf
    XIP += sizeof(IU);                      // advance by instruction size
}
void addvar() {                             // add a dovar (variable)
    DU n = 0;                               // default variable value
    addcode(find("dovar"));                 // dovar (+parameter field)
    pmem.push((U8*)&n, sizeof(DU));         // data storage (32-bit integer now)
    XIP += sizeof(DU);                      // skip to next field
}
void addlit(DU n) {                         // add a dolit (constant)
    addcode(find("dolit"));                 // dovar (+parameter field)
    pmem.push((U8*)&n, sizeof(DU));         // data storage (32-bit integer now)
    XIP += sizeof(DU);                      // skip to next field
}
void adddotstr(const char *s) {             // print a string
    int sz = ALIGN(strlen(s)+1);            // IU (16-bit) alignment
    addcode(find("dotstr"));                // dostr, (+parameter field)
    pmem.push((U8*)s, sz);                  // byte0, byte1, byte2, ..., byteN
    XIP += sz;                              // skip to next field
}
void addstr(const char *s) {                // add a string
    int sz = ALIGN(strlen(s)+1);            // IU (16-bit) alignment
    addcode(find("dostr"));                 // dostr, (+parameter field)
    pmem.push((U8*)s, sz);                  // byte0, byte1, byte2, ..., byteN
    XIP += sz;                              // skip to next field
}
///
/// Forth inner interpreter
///
int rs_max = 0;                             // rs watermark
void nest(IU c) {
    ///
    /// by not using any temp variable here can prevent auto stack allocation
    ///
    if (!dict[c].def) {                     // is a primitive?
        (*(fop*)(((uintptr_t)dict[c].xt)&~0x3))(c);  // mask out immd (and def), and execute
        return;
    }
    // is a colon words
    rs.push(WP); rs.push(IP); WP=c; IP=0;   // setup call frame
    if (rs.idx > rs_max) rs_max = rs.idx;   // keep rs sizing matrics
    try {
        while (IP < dict[c].len) {          // in instruction range
            nest(pmem[dict[c].pf + IP]);    // fetch/exec instruction from pmem space
            IP += sizeof(IU);               // advance to next instruction by 2
        }                                   // can be IP++ if pmem unit is 16-bit
    }
    catch(...) {}
    IP = rs.pop(); WP = rs.pop();           // restore call frame
}
///
/// utilize C++ standard template libraries for core IO functions only
/// Note:
///   * we use STL for its convinence, but
///   * if it takes too much memory for target MCU,
///   * these functions can be replaced with our own implementation
///
#include <sstream>      // iostream, stringstream
#include <iomanip>      // setbase
#include <string>       // string class
using namespace std;    // default to C++ standard template library
istringstream   fin;    // forth_in
ostringstream   fout;   // forth_out
string strbuf;          // input string buffer
///
/// Arduino specific macros
///
#define to_string(i)        string(String(i).c_str())
#define analogWrite(c,v,mx) ledcWrite((c),(8191/mx)*min((int)(v),mx))
#define ENDL                endl
///
/// debug functions
///
void dot_r(int n, int v) { fout << setw(n) << setfill(' ') << v; }
void to_s(IU c) {
    fout << dict[c].name << " " << c << (dict[c].immd ? "* " : " ");
}
void ss_dump() {
    fout << " <"; for (int i=0; i<ss.idx; i++) { fout << ss[i] << " "; }
    fout << top << "> ok" << ENDL;
}
void mem_dump(IU p0, U16 sz) {
    fout << setbase(16) << setfill('0');
    for (IU i=ALIGN32(p0); i<=ALIGN32(p0+sz); i+=0x20) {
        fout << setw(4) << i << ':';
        U8 *p = &pmem[i];
        for (int j=0; j<0x20; j++) {
            fout << setw(2) << (U16)*(p+j);
            if ((j%4)==3) fout << ' ';
        }
        fout << ' ';
        for (int j=0; j<0x20; j++) {   // print and advance to next byte
            char c = *(p+j) & 0x7f;
            fout << (char)((c==0x7f||c<0x20) ? '_' : c);
        }
        fout << ENDL;
    }
    fout << setbase(base);
}
void see(IU c, int dp=0) {
    if (c<0) return;
    to_s(c);
    if (!dict[c].def) return;
    for (int n=dict[c].len, i=0; i<n; i+=sizeof(IU)) {
        // TODO:
    }
}
void words() {
    for (int i=0; i<dict.idx - 1; i--) {
        if ((i%10)==0) fout << ENDL; to_s(i);
    }
}
///
/// memory statistics dump - for heap and stack debugging
///
static void mem_stat() {
    Serial.print("Core:");          Serial.print(xPortGetCoreID());
    Serial.print(" heap[maxblk=");  Serial.print(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.print(", avail=");        Serial.print(heap_caps_get_free_size(MALLOC_CAP_8BIT));
    Serial.print(", maxbss=");       Serial.print(maxbss);
    Serial.print(", pmem=");         Serial.print(pmem.idx);
    Serial.print("], lowest[heap="); Serial.print(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    Serial.print(", stack=");        Serial.print(uxTaskGetStackHighWaterMark(NULL));
    Serial.println("]");
    if (!heap_caps_check_integrity_all(true)) {
//        heap_trace_dump();     // dump memory, if we have to
        abort();                 // bail, on any memory error
    }
}
///
/// macros to reduce verbosity
///
inline void WORD()     { fin >> strbuf; colon(strbuf.c_str()); }  // create a colon word
inline char *SCAN(c)   { getline(fin, strbuf, c); return strbuf.c_str(); }
inline DU   PUSH(DU v) { ss.push(top); return top = v;         }
inline DU   POP()      { DU n=top; top=ss.pop(); return n;     }
#define     CODE(s, g) { s, [&](IU c){ g; }, 0 }
#define     IMMD(s, g) { s, [&](IU c){ g; }, 1 }
#define     BOOL(f)    ((f)?-1:0)
///
/// macros to abstract pmem access
///
#define     PFID       (dict[WP].pf+IP+sizeof(IU))  /* get parameter field index */
#define     CELL(a)    (*(DU*)&pmem[a])
#define     HALF(a)    (*(U16*)&pem[a])
#define     BYTE(a)    (*&pem[a])
///
/// global memory access
///
#define     PEEK(a)    (DU)(*(DU*)((uintptr_t)(a)))
#define     POKE(a, c) (*(DU*)((uintptr_t)(a))=(DU)(c))
///
/// primitives (ROMable)
/// Note:
///   * we merge prim into dictionary in main()
///   * However, since primitive is statically compiled
///   * it can be stored in ROM, and only
///   * find() needs to be modified to support ROM+RAM
///
auto _colon = [&](int c) {
    fin >> strbuf;
    colon(strbuf.c_str());
    compile=true;
};
static Code prim[] PROGMEM = {
    ///
    /// @defgroup Stack ops
    /// @{
    CODE("dup",  PUSH(top)),
    CODE("drop", top = ss.pop()),
    CODE("over", PUSH(ss[-1])),
    CODE("swap", DU n = ss.pop(); PUSH(n)),
    CODE("rot",  DU n = ss.pop(); DU m = ss.pop(); ss.push(n); PUSH(m)),
    CODE("pick", DU i = top; top = ss[-i]),
    CODE(">r",   rs.push(POP())),
    CODE("r>",   PUSH(rs.pop())),
    CODE("r@",   PUSH(rs[-1])),
    /// @}
    /// @defgroup Stack ops - double
    /// @{
    CODE("2dup", PUSH(ss[-1]); PUSH(ss[-1])),
    CODE("2drop",ss.pop(); top = ss.pop()),
    CODE("2over",PUSH(ss[-3]); PUSH(ss[-3])),
    CODE("2swap",
        DU n = ss.pop(); DU m = ss.pop(); DU l = ss.pop();
        ss.push(n); PUSH(l); PUSH(m)),
    /// @}
    /// @defgroup ALU ops
    /// @{
    CODE("+",    top += ss.pop()),
    CODE("*",    top *= ss.pop()),
    CODE("-",    top =  ss.pop() - top),
    CODE("/",    top =  ss.pop() / top),
    CODE("mod",  top =  ss.pop() % top),
    CODE("*/",   top =  ss.pop() * ss.pop() / top),
    CODE("/mod",
        DU n = ss.pop(); DU t = top;
        ss.push(n % t); top = (n / t)),
    CODE("*/mod",
        DU n = ss.pop() * ss.pop();
        DU t = top;
        ss.push(n % t); top = (n / t)),
    CODE("and",  top = ss.pop() & top),
    CODE("or",   top = ss.pop() | top),
    CODE("xor",  top = ss.pop() ^ top),
    CODE("abs",  top = abs(top)),
    CODE("negate", top = -top),
    CODE("max",  DU n=ss.pop(); top = (top>n)?top:n),
    CODE("min",  DU n=ss.pop(); top = (top<n)?top:n),
    CODE("2*",   top *= 2),
    CODE("2/",   top /= 2),
    CODE("1+",   top += 1),
    CODE("1-",   top -= 1),
    /// @}
    /// @defgroup Logic ops
    /// @{
    CODE("0= ",  top = BOOL(top == 0)),
    CODE("0<",   top = BOOL(top <  0)),
    CODE("0>",   top = BOOL(top >  0)),
    CODE("=",    top = BOOL(ss.pop() == top)),
    CODE(">",    top = BOOL(ss.pop() >  top)),
    CODE("<",    top = BOOL(ss.pop() <  top)),
    CODE("<>",   top = BOOL(ss.pop() != top)),
    CODE(">=",   top = BOOL(ss.pop() >= top)),
    CODE("<=",   top = BOOL(ss.pop() <= top)),
    /// @}
    /// @defgroup IO ops
    /// @{
    CODE("base@",   PUSH(base)),
    CODE("base!",   fout << setbase(base = POP())),
    CODE("hex",     fout << setbase(base = 16)),
    CODE("decimal", fout << setbase(base = 10)),
    CODE("cr",      fout << ENDL),
    CODE(".",       fout << POP() << " "),
    CODE(".r",      DU n = POP(); dot_r(n, POP())),
    CODE("u.r",     DU n = POP(); dot_r(n, abs(POP()))),
    CODE(".f",      DU n = POP(); fout << setprecision(n) << POP()),
    CODE("key",     PUSH(SCAN(' ')[0])),
    CODE("emit",    char b = (char)POP(); fout << b),
    CODE("space",   fout << " "),
    CODE("spaces",  for (DU n = POP(), i = 0; i < n; i++) fout << " "),
    /// @}
    /// @defgroup Literal ops
    /// @{
    CODE("dovar",   PUSH(PFID); IP += sizeof(DU)),
    CODE("dolit",   PUSH(CELL(PFID)); IP += sizeof(DU)),
    CODE("dostr",   PUSH(PFID); IP += ALIGN(strlen(s)+1)),
    CODE("dotstr",
        const char *s = (char*)&pmem[PFID]; // get string pointer
        fout << s;                          // send to output console
        IP += ALIGN(strlen(s)+1)),          // advance to next instruction
    CODE("[",       compile = false),
    CODE("]",       compile = true),
    IMMD("(",       SCAN(')')),
    IMMD(".(",      fout << SCAN(')')),
    CODE("\\",      SCAN('\n')),
    CODE("$\"",     addstr(SCAN('"')+1)),
    IMMD(".\"",     adddotstr(SCAN('"')+1)),
    /// @}
    /// @defgroup Branching ops
    /// @brief - if...then, if...else...then
    /// @{
    CODE("exit",    throw " "),
    CODE("branch" , IP = (IU)pmem[PFID] - sizeof(IU)),
    CODE("0branch", IP = POP() ? IP + sizeof(IU) : (IU)pmem[PFID] - sizeof(IU)),
    CODE("donext" , DU i = rs.pop() - 1;
         if (i<0) IP += sizeof(IU);
         else { IP = (IU)pmem[PFID] - sizeof(IU); rs.push(i); }),
    IMMD("if",      addcode(find("0branch")); PUSH(XIP); addcode(0)), // if    ( -- here ) 
    IMMD("else",                                                      // else ( here -- there )
        addcode(find("branch"));      
        IU h=XIP;   addcode(0); pmem[dict[-1]->pf + POP()]=XIP; PUSH(h)),
    IMMD("then",    pmem[dict[-1]->pf + POP()]=XIP),
    /// @}
    /// @defgroup Loops
    /// @brief  - begin...again, begin...f until, begin...f while...repeat
    /// @{
    IMMD("begin",   PUSH(XIP)),
    IMMD("again",   addcode(find("branch")); addcode(POP())),          // again    ( there -- ) 
    IMMD("until",   addcode(find("0branch")); addcode(POP())),         // until    ( there -- ) 
    IMMD("while",   addcode(find("0branch")); PUSH(XIP); addcode(0)),  // while    ( there -- there here ) 
    IMMD("repeat",  addcode(find("branch"));                           // repeat    ( there1 there2 -- ) 
        IU t=POP(); addcode(POP()); pmem[dict[-1]->pf + t]=XIP),
    /// @}
    /// @defgrouop For loops
    /// @brief  - for...next, for...aft...then...next
    /// @{
    IMMD("for" ,    addcode(find(">r")); PUSH(XIP)),                    // for ( -- here )
    IMMD("next",    addcode(find("donext")); addcode(POP())),           // next ( here -- )
    IMMD("aft",     POP(); addcode(find("branch"));                     // aft ( here -- here there )
        IU h=XIP; addcode(0); PUSH(XIP); PUSH(h)),
    /// @}
    /// @defgrouop Compiler ops
    /// @{
    CODE(":",        WORD(); compile=true),
    IMMD(";",        compile = false),
    CODE("create",   WORD();
         addcode(find("dovar"));                             // dovar (+parameter field) 
         XIP += sizeof(DU)),                                 // skip to next field
    CODE("variable", WORD(); addvar()),
    CODE("constant", WORD(); addlit(POP())),
    CODE("c@",    IU w = POP(); PUSH(BYTE(w));),             // w -- n
    CODE("c!",    IU w = POP(); BYTE(w) = POP()),
    CODE("w@",    IU w = POP(); PUSH(HALF(w))),              // w -- n
    CODE("w!",    IU w = POP(); HALF(w) = POP()),
    CODE("@",     IU w = POP(); PUSH(CELL(w))),              // w -- n
    CODE("!",     IU w = POP(); CELL(w) = POP();),           // n w --
    CODE("+!",    IU w = POP(); CELL(w) += POP()),           // n w --
    CODE("?",     IU w = POP(); fout << CELL(w) << " "),     // w --
    CODE("array@",IU a = POP()*sizeof(DU); IU w = POP(); PUSH(CELL(w+a))),   // w a -- n
    CODE("array!",IU a = POP()*sizeof(DU); IU w = POP(); CELL(w+a) = POP()), // n w a --
    CODE("allot", DU v = 0;                                  // n --
        for (IU n = POP(), i = 0; i < n; i++) pmem.push((U8*)&v, sizeof(DU))),
    CODE(",",     DU i = POP(); pmem.push((U8*)&i, sizeof(DU))),
    /// @}
    /// @defgroup metacompiler
    /// @{
    CODE("'",     IU w = find(SCAN(' ')); PUSH(w)),
#if 0
    CODE("does",
        ForthList<CodeP>& src = dict[WP]->pf;               // source word : xx create...does...;
        int n = src.size();
        while (Code::IP < n) dict[-1]->pf.push(src[Code::IP++])),       // copy words after "does" to new the word
    CODE("to",                                              // n -- , compile only
        CodeP tgt = find(next_idiom());
        if (tgt) tgt->pf[0]->qf[0] = POP()),                // update constant
    CODE("is",                                              // w -- , execute only
        CodeP tgt = find(next_idiom());
        if (tgt) {
            tgt->pf.clear();
            tgt->pf.merge(dict[POP()]->pf);
        }),
    CODE("[to]",
        ForthList<CodeP>& src = dict[WP]->pf;               // source word : xx create...does...;
        src[Code::IP++]->pf[0]->qf[0] = POP()),             // change the following constant
#endif
    /// @}
    /// @defgroup Debug ops
    /// @{
    CODE("here",  PUSH(pmem.idx)),
    CODE("words", words()),
    CODE(".s",    ss_dump()),
    CODE("see",   see(find(SCAN(' ')))),
    CODE("dump",  DU sz = POP(); IU a = POP(); mem_dump(a, sz)),
    CODE("forget",
        IU w = find(SCAN(' '));
        if (w<0) return;
        IU b = find("boot")+1;
        dict.clear(w > b ? w : b)),
    CODE("clock", PUSH(millis())),
    CODE("delay", delay(POP())),
    CODE("peek",  DU a = POP(); PUSH(PEEK(a))),
    CODE("poke",  DU a = POP(); POKE(a, POP())),
    /// @}
    /// @defgroup Arduino specific ops
    /// @{
    CODE("pin",   DU p = POP(); pinMode(p, POP())),
    CODE("in",    PUSH(digitalRead(POP()))),
    CODE("out",   DU p = POP(); digitalWrite(p, POP())),
    CODE("adc",   PUSH(analogRead(POP()))),
    CODE("duty",  DU p = POP(); analogWrite(p, POP(), 255)),
    CODE("attach",DU p  = POP(); ledcAttachPin(p, POP())),
    CODE("setup", DU ch = POP(); DU freq=POP(); ledcSetup(ch, freq, POP())),
    CODE("tone",  DU ch = POP(); ledcWriteTone(ch, POP())),
    /// @}
    CODE("bye",   exit(0)),
    CODE("boot",  dict.clear(find("boot") + 1))
};
const int PSZ = sizeof(prim)/sizeof(Code);
///
/// dictionary initialization
///
void forth_init() {
    for (int i=0; i<PSZ; i++) {             /// copy prim(ROM) into RAM dictionary,
        dict.push(prim[i]);                 /// find() can be modified to support
    }                                       /// searching both spaces
    words();
}
///
/// outer interpreter
///
void forth_outer() {
    while (fin >> strbuf) {
        const char *idiom = strbuf.c_str();
        printf("%s=>", idiom);
        int w = find(idiom);           /// * search through dictionary
        if (w>=0) {                                 /// * word found?
            printf("%s\n", dict[w].name);
            if (compile && !dict[w].immd) {         /// * in compile mode?
                addcode(w);                         /// * add to colon word
            }
            else nest(w);                           /// * execute forth word
            continue;
        }
        // try as a number
        char *p;
        int n = static_cast<int>(strtol(idiom, &p, base));
        printf("%d\n", n);
        if (*p != '\0') {                           /// * not number
            fout << idiom << "? " << ENDL;          ///> display error prompt
            compile = false;                        ///> reset to interpreter mode
            ss.clear(0); top = -1;
            break;                                  ///> skip the entire input buffer
        }
        // is a number
        if (compile) addlit(n);                     /// * add literal when in compile mode
        else PUSH(n);                               ///> or, add value onto data stack
    }
    if (!compile) ss_dump();
    return 1;
}
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
/// ledc 
///
// use first channel of 16 channels (started from zero)
#define LEDC_CHANNEL_0     0
// use 13 bit precission for LEDC timer
#define LEDC_TIMER_13_BIT  13
// use 5000 Hz as a LEDC base frequency
#define LEDC_BASE_FREQ     5000
// fade LED PIN (replace with LED_BUILTIN constant for built-in LED)
#define LED_PIN            5
#define BRIGHTNESS         255    // how bright the LED is

void setup() {
    Serial.begin(115200);
    delay(100);
    // Setup timer and attach timer to a led pin
    ledcSetup(0, 100, LEDC_TIMER_13_BIT);
    ledcAttachPin(5, 0);
//    analogWrite(0, 250, BRIGHTNESS);
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

    forth_init();
    Serial.println("\nesp32forth8 experimental 3");
    mem_stat();
}

void loop(void) {
    // for debugging: we can also take user input from Serial Monitor
    if (Serial.available()) {
        String cmd = Serial.readString();
        Serial.println(cmd);                  // sent cmd to console
        Serial.print(process_command(cmd));   // sent result to console
        mem_stat();
        delay(2);
    }
}
