/******************************************************************************/
/* esp32Forth, Version 8 : for NodeMCU ESP32S                                 */
/******************************************************************************/
#include <stdint.h>     // uintxx_t
#include <stdlib.h>     // strtol
#include <string.h>     // strcmp
#include <exception>    // try...catch, throw (disable for less capable MCU)
#include "SPIFFS.h"     // flash memory
///
/// control whether lambda capture parameter
/// Note:  0 reduces 100ms/1M cycles 
#define  LAMBDA_CAP    0
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
    int max = 0;        /// high watermark for debugging

    List()  { v = new T[N]; }      /// dynamically allocate array memory
    ~List() { delete[] v;   }      /// free the memory
    T& operator[](int i)   { return i < 0 ? v[idx + i] : v[i]; }
    T pop() {
        if (idx>0) return v[--idx];
        throw "ERR: List empty";
    }
    T push(T t) {
        if (idx<N) return v[max=idx++] = t;
        throw "ERR: List full";
    }
    void push(T *a, int n)  { for (int i=0; i<n; i++) push(*(a+i)); }
    void merge(List& a)     { for (int i=0; i<a.idx; i++) push(a[i]);}
    void clear(int i=0)     { idx=i; }
};
///
/// functor implementation - for lambda support (without STL)
///
#if LAMBDA_CAP
struct fop { virtual void operator()(IU) = 0; };
template<typename F>
struct XT : fop {           // universal functor
    F fp;
    XT(F &f) : fp(f) {}
    void operator()(IU c) { fp(c); }
};
#else
typedef void (*fop)();
#endif // LAMBDA_CAP
///
/// universal Code class
/// Note:
///   * 8-byte on 32-bit machine, 16-byte on 64-bit machine
///
#if LAMBDA_CAP
struct Code {
    const char *name = 0;   /// name field
    union {                 /// either a primitive or colon word
        fop xt = 0;         /// lambda pointer
        struct {            /// a colon word
            U16 def:  1;    /// colon defined word
            U16 immd: 1;    /// immediate flag
            U16 len:  14;   /// len of pf (16K max)
            IU  pfa;        /// offset to pmem space (16-bit for 64K range)
        };
    };
    template<typename F>    /// template function for lambda
    Code(const char *n, F f, bool im=false) : name(n) {
        xt = new XT<F>(f);
        immd = im ? 1 : 0;
    }
    Code() {}               /// create a blank struct (for initilization)
};
#else
struct Code {
    const char *name = 0;   /// name field
    union {                 /// either a primitive or colon word
        fop xt = 0;         /// lambda pointer
        struct {            /// a colon word
            U16 def:  1;    /// colon defined word
            U16 immd: 1;    /// immediate flag
            U16 len:  14;   /// len of pf (16K max)
            IU  pfa;        /// offset to pmem space (16-bit for 64K range)
        };
    };
    Code(const char *n, fop f, bool im=false) : name(n), xt(f) {
        immd = im ? 1 : 0;
    }
    Code() {}               /// create a blank struct (for initilization)
};
#endif // LAMBDA_CAP
///==============================================================================
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
List<Code, 2048>    dict; /// fixed sized dictionary (RISC vs CISC)
List<U8,   64*1024> pmem; /// parameter memory i.e. storage for all colon definitions
///
/// system variables
///
bool compile = false;
DU   top = -1, base = 10;
DU   ucase = 1;           /// case sensitivity control
IU   WP = 0;              /// current word pointer
U8   *IP = 0, *IP0 = 0;   /// current instruction pointer and base pointer
///
/// macros to abstract dict and pmem physical implementation
/// Note:
///   so we can change pmem implementation anytime without affecting opcodes defined below
///
#define STRLEN(s) (ALIGN(strlen(s)+1))      /** calculate string size with alignment     */
#define XIP       (dict[-1].len)            /** parameter field tail of latest word      */
#define PFA(w)    ((U8*)&pmem[dict[w].pfa]) /** parameter field of a word                */
#define CELL(a)   (*(DU*)&pmem[a])          /** fetch a cell from parameter memory       */
#define HALF(a)   (*(U16*)&pmem[a])         /** fetch half a cell from parameter memory  */
#define BYTE(a)   (*(U8*)&pmem[a])          /** fetch a byte from parameter memory       */
#define STR(a)    ((char*)&pmem[a])         /** fetch string pointer to parameter memory */
#define JMPIP     (IP0 + *(IU*)IP)          /** branching target address                 */
#define SETJMP(a) (*(IU*)(PFA(-1) + (a)))   /** address offset for branching opcodes     */
#define HERE      (pmem.idx)                /** current parameter memory index           */
#define IPOFF     ((IU)(IP - &pmem[0]))     /** IP offset relative parameter memory root */
#define CALL(w)   \
    if (dict[w].def) nest(w); \
    else ((fop)(((uintptr_t)dict[w].xt)&~0x3))()
///==============================================================================
///
/// dictionary search functions - can be adapted for ROM+RAM
///
inline int  STREQ(const char *s1, const char *s2) {
    return ucase ? strcasecmp(s1, s2)==0 : strcmp(s1, s2)==0;
}
int find(const char *s) {
    for (int i = dict.idx - (compile ? 2 : 1); i >= 0; --i) {
        if (STREQ(s, dict[i].name)) return i;
    }
    return -1;
}
///
/// inline functions to abstract and reduce verbosity
///
inline void ADD_IU(IU i)   { pmem.push((U8*)&i, sizeof(IU));  XIP+=sizeof(IU);  }  /** add an instruction into pmem */
inline void ADD_DU(DU v)   { pmem.push((U8*)&v, sizeof(DU)),  XIP+=sizeof(DU);  }  /** add a cell into pmem         */
inline void ADD_BYTE(U8 b) { pmem.push((U8*)&b, sizeof(U8));  XIP+=sizeof(U8);  }
inline void ADD_HALF(U16 w){ pmem.push((U8*)&w, sizeof(U16)); XIP+=sizeof(U16); }
inline void ADD_STR(const char *s) {                                               /** add a string to pmem         */
    int sz = STRLEN(s); pmem.push((U8*)s,  sz); XIP += sz;
}
inline void ADD_WORD(const char *s) { ADD_IU(find(s)); }                           /** find a word and add to pmem  */
///==============================================================================
///                   
/// colon word compiler
/// Note:
///   * we separate dict and pmem space to make word uniform in size
///   * if they are combined then can behaves similar to classic Forth
///   * with an addition link field added.
///
void colon(const char *name) {
    char *nfa = STR(HERE);                  // current pmem pointer
    int sz = STRLEN(name);                  // string length, aligned
    pmem.push((U8*)name,  sz);              // setup raw name field
#if LAMBDA_CAP
    Code c(nfa, [](int){});                 // create a new word on dictionary
#else
    Code c(nfa, NULL);
#endif // LAMBDA_CAP
    c.def = 1;                              // specify a colon word
    c.len = 0;                              // advance counter (by number of U16)
    c.pfa = HERE;                           // capture code field index
    dict.push(c);                           // deep copy Code struct into dictionary
};
///
/// Forth inner interpreter
///
void nest(IU c) {
    /// handles a colon word
    rs.push((DU)(IP - IP0)); rs.push(WP);   /// * setup call frame
    IP0 = IP = PFA(WP=c);                   // CC: this takes 30ms/1K, need work
    // i.e. IP = ((U8*)&pmem[dict[c].pfa])
    IU n = dict[c].len;                     // CC: this saved 300ms/1M
    try {                                   // CC: is dict[c] kept in cache?
        while ((IU)(IP - IP0) < n) {        /// * recursively call all children
            IU c1 = *IP; IP += sizeof(IU);  // CC: cost of (n, c1) on stack?
            CALL(c1);                       ///> execute child word
        }                                   ///> can do IP++ if pmem unit is 16-bit
    }
    catch(...) {}                           ///> protect if any exeception
    yield();                                ///> give other tasks some time
    IP0 = PFA(WP=rs.pop());                 /// * restore call frame
    IP  = IP0 + rs.pop();
}
///==============================================================================
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
void (*fout_cb)(int, const char*);  // forth output callback function
///
/// Arduino specific macros
///
#define analogWrite(c,v,mx) ledcWrite((c),(8191/mx)*min((int)(v),mx))
#define ENDL                endl; fout_cb(fout.str().length(), fout.str().c_str()); fout.str("")
///================================================================================
/// debug functions
///                   
void dot_r(int n, int v) { fout << setw(n) << setfill(' ') << v; }
void to_s(IU c) {
    fout << dict[c].name << " " << c << (dict[c].immd ? "* " : " ");
}
///
/// recursively disassemble colon word
///
void see(IU *cp, IU *ip, int dp=0) {
    fout << ENDL; for (int i=dp; i>0; i--) fout << "  ";            // indentation
    if (dp) fout << "[" << setw(2) << *ip << ": ";                  // ip offset
    else    fout << "[ ";
    IU c = *cp;
    to_s(c);                                                        // name field
    if (dict[c].def) {                                              // a colon word
        for (IU n=dict[c].len, ip1=0; ip1<n; ip1+=sizeof(IU)) {     // walk through children
            IU *cp1 = (IU*)(PFA(c) + ip1);                          // wp of next children node
            see(cp1, &ip1, dp+1);                                   // dive recursively
        }
    }
    static const char *nlist[7] PROGMEM = {   // even string compare is expensive
        "dovar", "dolit", "dostr", "dotstr",  // but since see is a user timeframe
        "branch", "0branch", "donext"         // function, so we can trade time
    };                                        // with space keeping everything local
    int i=0;
    while (i<7 && strcmp(nlist[i], dict[c].name)) i++;
    switch (i) {
    case 0: case 1:
        fout << "= " << *(DU*)(cp+1); *ip += sizeof(DU); break;
    case 2: case 3:
        fout << "= \"" << (char*)(cp+1) << '"';
        *ip += STRLEN((char*)(cp+1)); break;
    case 4: case 5: case 6:
        fout << "j" << *(cp+1); *ip += sizeof(IU); break;
    }
    fout << "] ";
}
void words() {
    for (int i=0; i<dict.idx; i++) {
        if ((i%10)==0) { fout << ENDL; yield(); }
        to_s(i);
    }
}
void ss_dump() {
    fout << " <"; for (int i=0; i<ss.idx; i++) { fout << ss[i] << " "; }
    fout << top << "> ok" << ENDL;
}
///
/// dump pmem at p0 offset for sz bytes
///
void mem_dump(IU p0, DU sz) {
    fout << setbase(16) << setfill('0') << ENDL;
    for (IU i=ALIGN32(p0); i<=ALIGN32(p0+sz); i+=16) {
        fout << setw(4) << i << ": ";
        for (int j=0; j<16; j++) {
            char c = pmem[i+j];
            fout << setw(2) << (int)c << (j%4==3 ? "  " : " ");
        }
        for (int j=0; j<16; j++) {   // print and advance to next byte
            char c = pmem[i+j] & 0x7f;
            fout << (char)((c==0x7f||c<0x20) ? '_' : c);
        }
        fout << ENDL;
        yield();
    }
    fout << setbase(base);
}
///================================================================================
///
/// macros to reduce verbosity
///
inline char *NEXT_WORD()  { fin >> strbuf; return (char*)strbuf.c_str(); } // get next idiom
inline char *SCAN(char c) { getline(fin, strbuf, c); return (char*)strbuf.c_str(); }
inline DU   PUSH(DU v)    { ss.push(top); return top = v;         }
inline DU   POP()         { DU n=top; top=ss.pop(); return n;     }
#if LAMBDA_CAP
#define     CODE(s, g)    { s, [](int c){ g; }, 0 }
#define     IMMD(s, g)    { s, [](int c){ g; }, 1 }
#else
#define     CODE(s, g)    { s, []{ g; }, 0 }
#define     IMMD(s, g)    { s, []{ g; }, 1 }
#endif // LAMBDA_CAP
#define     BOOL(f)       ((f)?-1:0)
///
/// global memory access macros
///
#define     PEEK(a)    (DU)(*(DU*)((uintptr_t)(a)))
#define     POKE(a, c) (*(DU*)((uintptr_t)(a))=(DU)(c))
///================================================================================
///
/// primitives (ROMable)
/// Note:
///   * we merge prim into dictionary in main()
///   * However, since primitive is statically compiled
///   * it can be stored in ROM, and only
///   * find() needs to be modified to support ROM+RAM
///
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
    CODE("key",     PUSH(NEXT_WORD()[0])),
    CODE("emit",    char b = (char)POP(); fout << b),
    CODE("space",   fout << " "),
    CODE("spaces",  for (DU n = POP(), i = 0; i < n; i++) fout << " "),
    /// @}
    /// @defgroup Literal ops
    /// @{
    CODE("dovar",   PUSH(IPOFF); IP += sizeof(DU)),
    CODE("dolit",   PUSH(*(DU*)IP); IP += sizeof(DU)),
    CODE("dostr",
        const char *s = (const char*)IP;            // get string pointer
        PUSH(IPOFF); IP += STRLEN(s)),
    CODE("dotstr",
        const char *s = (const char*)IP;            // get string pointer
        fout << s;  IP += STRLEN(s)),               // send to output console
    CODE("[",       compile = false),
    CODE("]",       compile = true),
    IMMD("(",       SCAN(')')),
    IMMD(".(",      fout << SCAN(')')),
    CODE("\\",      SCAN('\n')),
    CODE("$\"",
        const char *s = SCAN('"')+1;        // string skip first blank
        ADD_WORD("dostr");                  // dostr, (+parameter field)
        ADD_STR(s)),                        // byte0, byte1, byte2, ..., byteN
    IMMD(".\"",
        const char *s = SCAN('"')+1;        // string skip first blank
        ADD_WORD("dotstr");                 // dostr, (+parameter field)
        ADD_STR(s)),                        // byte0, byte1, byte2, ..., byteN
    /// @}
    /// @defgroup Branching ops
    /// @brief - if...then, if...else...then
    /// @{
    CODE("branch" , IP = JMPIP),                                 // unconditional branch
    CODE("0branch", IP = POP() ? IP + sizeof(IU) : JMPIP),       // conditional branch
    IMMD("if",      ADD_WORD("0branch"); PUSH(XIP); ADD_IU(0)),  // if    ( -- here ) 
    IMMD("else",                                                 // else ( here -- there )
        ADD_WORD("branch");
        IU h=XIP;   ADD_IU(0); SETJMP(POP()) = XIP; PUSH(h)),
    IMMD("then",    SETJMP(POP()) = XIP),
    /// @}
    /// @defgroup Loops
    /// @brief  - begin...again, begin...f until, begin...f while...repeat
    /// @{
    IMMD("begin",   PUSH(XIP)),
    IMMD("again",   ADD_WORD("branch");  ADD_IU(POP())),         // again    ( there -- ) 
    IMMD("until",   ADD_WORD("0branch"); ADD_IU(POP())),         // until    ( there -- ) 
    IMMD("while",   ADD_WORD("0branch"); PUSH(XIP); ADD_IU(0)),  // while    ( there -- there here ) 
    IMMD("repeat",  ADD_WORD("branch");                          // repeat    ( there1 there2 -- ) 
        IU t=POP(); ADD_IU(POP()); SETJMP(t) = XIP),             // set forward and loop back address
    /// @}
    /// @defgrouop For loops
    /// @brief  - for...next, for...aft...then...next
    /// @{
    CODE("donext",
        if ((rs[-1] -= 1) >= 0) IP = JMPIP;                      // rs[-1]-=1 saved 2000ms/1M cycles
        else { IP += sizeof(IU); rs.pop(); }),
    IMMD("for" ,    ADD_WORD(">r"); PUSH(XIP)),                  // for ( -- here )
    IMMD("next",    ADD_WORD("donext"); ADD_IU(POP())),          // next ( here -- )
    IMMD("aft",                                                  // aft ( here -- here there )
        POP(); ADD_WORD("branch");
        IU h=XIP; ADD_IU(0); PUSH(XIP); PUSH(h)),
    /// @}
    /// @defgrouop Compiler ops
    /// @{
    CODE(":", colon(NEXT_WORD()); compile=true),
    IMMD(";", compile = false),
    CODE("create",
        colon(NEXT_WORD());                                      // create a new word on dictionary
        ADD_WORD("dovar");                                       // dovar (+parameter field) 
        XIP -= sizeof(DU)),                                      // backup one field
    CODE("variable",                                             // create a variable
        colon(NEXT_WORD());                                      // create a new word on dictionary
        DU n = 0;                                                // default value
        ADD_WORD("dovar");                                       // dovar (+parameter field)
        ADD_DU(n)),                                              // data storage (32-bit integer now)
    CODE("constant",                                             // create a constant
        colon(NEXT_WORD());                                      // create a new word on dictionary
        ADD_WORD("dolit");                                       // dovar (+parameter field)
        ADD_DU(POP())),                                          // data storage (32-bit integer now)
    //
    // be careful with memory access, especially BYTE because
    // it could make access misaligned which slows the access speed by 2x
    //
    CODE("c@",    IU w = POP(); PUSH(BYTE(w));),                 // w -- n
    CODE("c!",    IU w = POP(); BYTE(w) = POP()),
    CODE("c,",    DU n = POP(); ADD_BYTE(n)),
    CODE("w@",    IU w = POP(); PUSH(HALF(w))),                  // w -- n
    CODE("w!",    IU w = POP(); HALF(w) = POP()),
    CODE("w,",    DU n = POP(); ADD_HALF(n)),
    CODE("@",     IU w = POP(); PUSH(CELL(w))),                  // w -- n
    CODE("!",     IU w = POP(); CELL(w) = POP();),               // n w --
    CODE(",",     DU n = POP(); ADD_DU(n)),
    CODE("allot", DU v = 0; for (IU n = POP(), i = 0; i < n; i++) ADD_DU(v)), // n --
    CODE("+!",    IU w = POP(); CELL(w) += POP()),               // n w --
    CODE("?",     IU w = POP(); fout << CELL(w) << " "),         // w --
    /// @}
    /// @defgroup metacompiler
    /// @{
    CODE("exit",  throw " "),
    CODE("exec",  CALL(POP())),
    CODE("does",  /* TODO */),
    CODE("to",    /* TODO */),
    CODE("is",    /* TODO */),
    CODE("[to]",  /* TODO */),
    /// @}
    /// @defgroup Debug ops
    /// @{
    CODE("here",  PUSH(HERE)),
    CODE("ucase", ucase = POP()),
    CODE("words", words()),
    CODE("'",     IU w = find(NEXT_WORD()); PUSH(w)),
    CODE(".s",    ss_dump()),
    CODE("see",   IU w = find(NEXT_WORD()); IU ip=0; see(&w, &ip)),
    CODE("dump",  DU n = POP(); IU a = POP(); mem_dump(a, n)),
    CODE("peek",  DU a = POP(); PUSH(PEEK(a))),
    CODE("poke",  DU a = POP(); POKE(a, POP())),
    CODE("forget",
        IU w = find(NEXT_WORD());
        if (w<0) return;
        IU b = find("boot")+1;
        dict.clear(w > b ? w : b)),
    CODE("clock", PUSH(millis())),
    CODE("delay", delay(POP())),
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
    CODE("bye",   exit(0)),                   /// soft reboot ESP32
    CODE("boot",  dict.clear(find("boot") + 1); pmem.clear())
};
const int PSZ = sizeof(prim)/sizeof(Code);
///
/// dictionary initialization
///
void forth_init() {
    for (int i=0; i<PSZ; i++) {              /// copy prim(ROM) into RAM dictionary,
        dict.push(prim[i]);                  /// find() can be modified to support
    }                                        /// searching both spaces
}
///
/// outer interpreter
///
void forth_outer(const char *cmd, void(*callback)(int, const char*)) {
    fin.clear();                             /// clear input stream error bit if any
    fin.str(cmd);                            /// feed user command into input stream
    fout_cb = callback;                      /// setup callback function
    fout.str("");                            /// clean output buffer, ready for next run
    while (fin >> strbuf) {
        const char *idiom = strbuf.c_str();
        printf("%s=>", idiom);
        int w = find(idiom);                 /// * search through dictionary
        if (w>=0) {                          /// * word found?
            printf("%s %d\n", dict[w].name, w);
            if (compile && !dict[w].immd) {  /// * in compile mode?
                ADD_IU(w);                   /// * add found word to new colon word
            }
            else CALL(w);                    /// * execute forth word
            continue;
        }
        // try as a number
        char *p;
        int n = static_cast<int>(strtol(idiom, &p, base));
        printf("%d\n", n);
        if (*p != '\0') {                    /// * not number
            fout << idiom << "? " << ENDL;   ///> display error prompt
            compile = false;                 ///> reset to interpreter mode
            ss.clear(0); top = -1;
            break;                           ///> skip the entire input buffer
        }
        // is a number
        if (compile) {                       /// * add literal when in compile mode
            ADD_WORD("dolit");               ///> dovar (+parameter field)
            ADD_DU(n);                       ///> data storage (32-bit integer now)
        }
        else PUSH(n);                        ///> or, add value onto data stack
    }
    if (!compile) ss_dump();
}
///==========================================================================
/// ForthVM front-end handlers
///==========================================================================
///
/// translate ESP32 String to/from Forth input/output streams (in C++ string)
///
#define LOGF(s)  Serial.print(F(s))
#define LOG(v)   Serial.print(v)
///
/// memory statistics dump - for heap and stack debugging
///
static void mem_stat() {
    LOGF("Core:");           LOG(xPortGetCoreID());
    LOGF(" heap[maxblk=");   LOG(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    LOGF(", avail=");        LOG(heap_caps_get_free_size(MALLOC_CAP_8BIT));
    LOGF(", ss_max=");       LOG(ss.max);
    LOGF(", rs_max=");       LOG(rs.max);
    LOGF(", pmem=");         LOG(HERE);
    LOGF("], lowest[heap="); LOG(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    LOGF(", stack=");        LOG(uxTaskGetStackHighWaterMark(NULL));
    LOGF("]\n");
    if (!heap_caps_check_integrity_all(true)) {
//        heap_trace_dump();     // dump memory, if we have to
        abort();                 // bail, on any memory error
    }
}
///
/// Forth bootstrap loader (from Flash)
///
static int forth_load(const char *fname) {
    auto dummy = [](int, const char *) { /* do nothing */ };
    if (!SPIFFS.begin()) {
        LOGF("Error mounting SPIFFS"); return 1; }
    File file = SPIFFS.open(fname, "r");
    if (!file) {
        LOGF("Error opening file:"); LOG(fname); return 1; }
    LOGF("Loading file: "); LOG(fname); LOGF("...");
    while (file.available()) {
        char cmd[256], *p = cmd, c;
        while ((c = file.read())!='\n') *p++ = c;   // one line a time
        *p = '\0';
        LOGF("\n<< "); LOG(cmd);                    // show bootstrap command
        forth_outer(cmd, dummy); }
    LOGF("Done loading.\n");
    file.close();
    SPIFFS.end();
    return 0;
}

void forth_setup() {
    ///
    /// ForthVM initalization
    ///
    forth_init();
//    forth_load("/load.txt");    // compile /data/load.txt

    mem_stat();
}
///==========================================================================
/// ESP32 Web Serer connection and index page
///==========================================================================
#include <WiFi.h>

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
         background-color:#f8f0f0;'>ESP32Forth v8</div>
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
                forth_outer(http_req.c_str(), send_chunk);
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
    forth_setup();
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
        forth_outer(console_cmd.c_str(), send_to_con);
        mem_stat();
        delay(2);
    }
}
