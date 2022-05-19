///
/// esp32Forth, Version 8 : for NodeMCU ESP32S
///
/// benchmark: 1M test case
///    1440ms Dr. Ting's orig/esp32forth_82
///    1240ms ~/Download/forth/esp32/esp32forth8_exp9 
///    1045ms orig/esp32forth8_1
///     735ms + INLINE List methods
///
/******************************************************************************/
#include <stdlib.h>     // strtol
#include <string.h>     // strcmp
#include <exception>    // try...catch, throw (disable for less capable MCU)
#include "SPIFFS.h"     // flash memory
#include "eforth.h"
///
/// version info
///
#define APP_NAME         "esp32forth"
#define MAJOR_VERSION    "8"
#define MINOR_VERSION    "1"
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
List<DU,   E4_SS_SZ>   ss;       /// data stack, can reside in registers for some processors
List<DU,   E4_RS_SZ>   rs;       /// return stack
List<Code, E4_DICT_SZ> dict;     /// fixed sized dictionary (RISC vs CISC)
List<U8,   E4_PMEM_SZ> pmem;     /// parameter memory i.e. storage for all colon definitions
U8  *MEM0 = &pmem[0];            /// based of parameter memory block
UFP DICT0;                       /// base of dictionary
///
/// system variables
///
bool compile = false;
DU   top = -1, base = 10;
DU   ucase = 1;                 /// case sensitivity control
IU   WP = 0;                    /// current word pointer
U8   *IP = MEM0, *IP0 = MEM0;   /// current instruction pointer and cached base pointer
///
/// macros to abstract dict and pmem physical implementation
/// Note:
///   so we can change pmem implementation anytime without affecting opcodes defined below
///
#define PFA(w)    ((U8*)&pmem[dict[w].pfa]) /** parameter field pointer of a word        */
#define HERE      (pmem.idx)                /** current parameter memory index           */
#define OFF(ip)   ((IU)((U8*)(ip) - MEM0))  /** IP offset (index) in parameter memory    */
#define MEM(ip)   (MEM0 + *(IU*)(ip))       /** pointer to IP address fetched from pmem  */
#define CELL(a)   (*(DU*)&pmem[a])          /** fetch a cell from parameter memory       */
#define SETJMP(a) (*(IU*)&pmem[a])          /** address offset for branching opcodes     */
#define BOOL(f)   ((f)?-1:0)
///==============================================================================
///
/// dictionary search functions - can be adapted for ROM+RAM
///
int pfa2word(U8 *ip) {
    IU   ipx = *(IU*)ip;
    U8   *xt = (U8*)(DICT0 + ipx);
    for (int i = dict.idx - 1; i >= 0; --i) {
        if (ipx & 1) {
            if (dict[i].pfa == (ipx & ~1)) return i;
        }
        else if ((U8*)dict[i].xt == xt) return i;
    }
    return -1;
}
inline int streq(const char *s1, const char *s2) {
    return ucase ? strcasecmp(s1, s2)==0 : strcmp(s1, s2)==0;
}
int find(const char *s) {
    for (int i = dict.idx - (compile ? 2 : 1); i >= 0; --i) {
        if (streq(s, dict[i].name)) return i;
    }
    return -1;
}
///==============================================================================
///
/// colon word compiler
/// Note:
///   * we separate dict and pmem space to make word uniform in size
///   * if they are combined then can behaves similar to classic Forth
///   * with an addition link field added.
///
enum {
    EXIT = 0, DOVAR, DOLIT, DOSTR, DOTSTR, BRAN, ZBRAN, DONEXT, DOES, TOR
} forth_opcode;

void colon(const char *name) {
    char *nfa = (char*)&pmem[HERE];         // current pmem pointer
    int sz = STRLEN(name);                  // string length, aligned
    pmem.push((U8*)name,  sz);              // setup raw name field
#if LAMBDA_OK
    Code c(nfa, [](int){});                 // create a new word on dictionary
#else  // LAMBDA_OK
    Code c(nfa, NULL);
#endif // LAMBDA_OK
    c.def = 1;                              // specify a colon word
    c.len = 0;                              // advance counter (by number of U16)
    c.pfa = HERE;                           // capture code field index
    dict.push(c);                           // deep copy Code struct into dictionary
};
void add_iu(IU i) { pmem.push((U8*)&i, sizeof(IU)); dict[-1].len+=sizeof(IU);  }  /** add an instruction into pmem */
void add_du(DU v) { pmem.push((U8*)&v, sizeof(DU)), dict[-1].len+=sizeof(DU);  }  /** add a cell into pmem         */
void add_str(const char *s) {                                            /** add a string to pmem         */
    int sz = STRLEN(s); pmem.push((U8*)s,  sz); dict[-1].len += sz;
}
void  add_w(IU w) {
    Code &c  = dict[w];
    IU   ipx = c.def ? (c.pfa | 1) : (IU)((UFP)c.xt - DICT0);
    add_iu(ipx);
}
inline DU   POP()       { DU n = top; top=ss.pop(); return n; }
inline void PUSH(DU v)  { ss.push(top); top = v; }
///============================================================================
///
/// Forth inner interpreter (handles a colon word)
/// Note:
///   use local stack, 1070 => 1058, but used 64 bytes per call
///   use NOP opcode terminator, 1070 => 1099, but no need for ipx on stack
#define CALL(w) \
    if (dict[w].def) { WP = w; IP = MEM0 + dict[w].pfa; nest(); } \
    else (*(fop)(((UFP)dict[w].xt) & ~0x3))()
/*
void nest(IU c) {
    rs.push(IP - MEM0); rs.push(WP);        /// * setup call frame
    IP0 = IP = PFA(WP=c);                   // CC: this takes 30ms/1K, need work
    try {                                   // CC: is dict[c] kept in cache?
        U8 *ipx = IP + PFLEN(c);            // CC: this saved 350ms/1M
        while (IP < ipx) {                  /// * recursively call all children
            IU c1 = *IP; IP += sizeof(IU);  // CC: cost of (ipx, c1) on statck?
            CALL(c1);                       ///> execute child word
        }                                   ///> can do IP++ if pmem unit is 16-bit
    }
    catch(...) {}                           ///> protect if any exeception
    yield();                                ///> give other tasks some time
    IP0 = PFA(WP = rs.pop());               /// * restore call frame
    IP  = MEM0 + rs.pop();
}
*/
void nest() {
    int dp = 0;                                      /// iterator depth control
    while (dp >= 0) {
        /// function core
        auto ipx = *(IU*)IP;                         /// hopefully use register than cached line
        while (ipx) {
            if (ipx & 1) {
                rs.push(WP);                         /// * setup callframe (ENTER)
                rs.push(OFF(IP) + sizeof(IU));
                IP = MEM0 + (ipx & ~0x1);            /// word pfa (def masked)
                dp++;
            }
            else {
                UFP xt = DICT0 + (ipx & ~0x3);       /// * function pointer
                IP += sizeof(IU);                    /// advance to next pfa
                (*(fop*)xt)();
            }
            ipx = *(IU*)IP;
        }
        if (dp-- > 0) {
            IP = MEM0 + rs.pop();                    /// * restore call frame (EXIT)
            WP = rs.pop();
        }
    }
    yield();                                ///> give other tasks some time
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

///================================================================================
///
/// IO & debug functions
///
inline char *next_idiom() { fin >> strbuf; return (char*)strbuf.c_str(); } // get next idiom
inline char *scan(char c) { getline(fin, strbuf, c); return (char*)strbuf.c_str(); }
inline void dot_r(int n, int v) { fout << setw(n) << setfill(' ') << v; }
inline void to_s(IU c) {
    fout << dict[c].name << " " << c << (dict[c].immd ? "* " : " ");
}
///
/// recursively disassemble colon word
///
void see(U8 *ip, int dp=1) {
    while (*(IU*)ip) {
        fout << ENDL; for (int i=dp; i>0; i--) fout << "  ";        // indentation
        fout << setw(4) << OFF(ip) << "[ " << setw(-1);
        IU c = pfa2word(ip);
        to_s(c);                                                    // name field
        if (dict[c].def && dp <= 2) {                               // is a colon word
            see(PFA(c), dp+1);                                      // recursive into child PFA
        }
        ip += sizeof(IU);
        switch (c) {
        case DOVAR: case DOLIT:
            fout << "= " << *(DU*)ip; ip += sizeof(DU); break;
        case DOSTR: case DOTSTR:
            fout << "= \"" << (char*)ip << '"';
            ip += STRLEN((char*)ip); break;
        case BRAN: case ZBRAN: case DONEXT:
            fout << "j" << *(IU*)ip; ip += sizeof(IU); break;
        }
        fout << "] ";
    }
}
void words() {
    fout << setbase(16);
    for (int i=0; i<dict.idx; i++) {
        if ((i%10)==0) { fout << ENDL; yield(); }
        to_s(i);
    }
    fout << setbase(base);
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
    for (IU i=ALIGN16(p0); i<=ALIGN16(p0+sz); i+=16) {
        fout << setw(4) << i << ": ";
        for (int j=0; j<16; j++) {
            U8 c = pmem[i+j];
            fout << setw(2) << (int)c << (j%4==3 ? "  " : " ");
        }
        for (int j=0; j<16; j++) {   // print and advance to next byte
            U8 c = pmem[i+j] & 0x7f;
            fout << (char)((c==0x7f||c<0x20) ? '_' : c);
        }
        fout << ENDL;
        yield();
    }
    fout << setbase(base);
}
///
/// global memory access macros
///
#define     PEEK(a)    (DU)(*(DU*)((UFP)(a)))
#define     POKE(a, c) (*(DU*)((UFP)(a))=(DU)(c))
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
    /// @defgroup Executino control ops
    /// @brief - do not change order, see forth_opcode enum sequence
    /// @{
    CODE("exit",    {}),
    CODE("dovar",   PUSH(OFF(IP)); IP += sizeof(DU)),
    CODE("dolit",   PUSH(*(DU*)IP); IP += sizeof(DU)),
    CODE("dostr",
         const char *s = (const char*)IP;            // get string pointer
         PUSH(OFF(IP)); IP += STRLEN(s)),
    CODE("dotstr",
         const char *s = (const char*)IP;            // get string pointer
         fout << s;  IP += STRLEN(s)),               // send to output console
    CODE("branch" , IP = MEM(IP)),                   // unconditional branch
    CODE("0branch", IP = POP() ? IP+sizeof(IU) : MEM(IP)), // conditional branch
    CODE("donext",
         if ((rs[-1] -= 1) >= 0) IP = MEM(IP);       // rs[-1]-=1 saved 2000ms/1M cycles
         else { IP += sizeof(IU); rs.pop(); }),
    CODE("does",                                     // CREATE...DOES... meta-program
         IU *ip  = (IU*)PFA(WP);
         while (*ip != DOES) ip++;                 // find DOES
         while (*ip) add_iu(*ip);),                // copy&paste code
    CODE(">r",   rs.push(POP())),
    CODE("r>",   PUSH(rs.pop())),
    CODE("r@",   PUSH(rs[-1])),
    /// @}
    /// @defgroup Stack ops
    /// @brief - from here on, opcode sequence can be freely reordered
    /// @{
    CODE("dup",  PUSH(top)),
    CODE("drop", top = ss.pop()),
    CODE("over", PUSH(ss[-1])),
    CODE("swap", DU n = ss.pop(); PUSH(n)),
    CODE("rot",  DU n = ss.pop(); DU m = ss.pop(); ss.push(n); PUSH(m)),
    CODE("pick", DU i = top; top = ss[-i]),
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
    CODE(".r",      int n = POP(); dot_r(n, POP())),
    CODE("u.r",     int n = POP(); dot_r(n, abs(POP()))),
    CODE(".f",      int n = POP(); fout << setprecision(n) << POP()),
    CODE("key",     PUSH(next_idiom()[0])),
    CODE("emit",    char b = (char)POP(); fout << b),
    CODE("space",   fout << " "),
    CODE("spaces",  for (int n = POP(), i = 0; i < n; i++) fout << " "),
    /// @}
    /// @defgroup Literal ops
    /// @{
    CODE("[",       compile = false),
    CODE("]",       compile = true),
    IMMD("(",       scan(')')),
    IMMD(".(",      fout << scan(')')),
    CODE("\\",      scan('\n')),
    CODE("$\"",
         const char *s = scan('"')+1;        // string skip first blank
         add_w(DOSTR);                       // dostr, (+parameter field)
         add_str(s)),                        // byte0, byte1, byte2, ..., byteN
    IMMD(".\"",
         const char *s = scan('"')+1;        // string skip first blank
         add_w(DOTSTR);                      // dostr, (+parameter field)
         add_str(s)),                        // byte0, byte1, byte2, ..., byteN
    /// @}
    /// @defgroup Branching ops
    /// @brief - if...then, if...else...then
    /// @{
    IMMD("if",      add_w(ZBRAN); PUSH(HERE); add_iu(0)),    // if    ( -- here ) 
    IMMD("else",                                             // else ( here -- there )
         add_w(BRAN);
         IU h=HERE; add_iu(0); SETJMP(POP()) = HERE; PUSH(h)),
    IMMD("then",    SETJMP(POP()) = HERE),                   // then, backfill jump address
    /// @}
    /// @defgroup Loops
    /// @brief  - begin...again, begin...f until, begin...f while...repeat
    /// @{
    IMMD("begin",   PUSH(HERE)),
    IMMD("again",   add_w(BRAN);  add_iu(POP())),            // again    ( there -- ) 
    IMMD("until",   add_w(ZBRAN); add_iu(POP())),            // until    ( there -- ) 
    IMMD("while",   add_w(ZBRAN); PUSH(HERE); add_iu(0)),    // while    ( there -- there here ) 
    IMMD("repeat",  add_w(BRAN);                             // repeat    ( there1 there2 -- ) 
         IU t=POP(); add_iu(POP()); SETJMP(t) = HERE),       // set forward and loop back address
    /// @}
    /// @defgrouop For loops
    /// @brief  - for...next, for...aft...then...next
    /// @{
    IMMD("for" ,    add_w(TOR); PUSH(HERE)),                 // for ( -- here )
    IMMD("next",    add_w(DONEXT); add_iu(POP())),           // next ( here -- )
    IMMD("aft",                                              // aft ( here -- here there )
         POP(); add_w(BRAN);
         IU h=HERE; add_iu(0); PUSH(HERE); PUSH(h)),
    /// @}
    /// @defgrouop Compiler ops
    /// @{
    CODE(":", colon(next_idiom()); compile=true),
    IMMD(";", add_w(EXIT); compile = false),
    CODE("create",
         colon(next_idiom());                                // create a new word on dictionary
         add_w(DOVAR)),                                      // dovar (+parameter field) 
    CODE("variable",                                         // create a variable
         colon(next_idiom());                                // create a new word on dictionary
         DU n = 0;                                           // default value
         add_w(DOVAR);                                       // dovar (+parameter field)
         add_du(n)),                                         // data storage (32-bit integer now)
    CODE("constant",                                         // create a constant
         colon(next_idiom());                                // create a new word on dictionary
         add_w(DOLIT);                                       // dovar (+parameter field)
         add_du(POP())),                                     // data storage (32-bit integer now)
    /// @}
    /// @defgroup metacompiler
    /// @{
    CODE("exec",  CALL(POP())),                              // execute word
    CODE("create",
        colon(next_idiom());                                 // create a new word on dictionary
        add_iu(DOVAR)),                                      // dovar (+ parameter field)
    CODE("to",              // 3 to x                        // alter the value of a constant
        IU w = find(next_idiom());                           // to save the extra @ of a variable
        *(DU*)(PFA(w) + sizeof(IU)) = POP()),
    CODE("is",              // ' y is x                      // alias a word
        IU w = find(next_idiom());                           // can serve as a function pointer
        dict[POP()].pfa = dict[w].pfa),                      // but might leave a dangled block
    CODE("[to]",            // : xx 3 [to] y ;               // alter constant in compile mode
        IU w = *(IU*)IP; IP += sizeof(IU);                   // fetch constant pfa from 'here'
        *(DU*)(PFA(w) + sizeof(IU)) = POP()),
    //
    // be careful with memory access, especially BYTE because
    // it could make access misaligned which slows the access speed by 2x
    //
    CODE("@",     IU w = POP(); PUSH(CELL(w))),              // w -- n
    CODE("!",     IU w = POP(); CELL(w) = POP();),           // n w --
    CODE(",",     DU n = POP(); add_du(n)),
    CODE("allot", DU v = 0; for (int n = POP(), i = 0; i < n; i++) add_du(v)), // n --
    CODE("+!",    IU w = POP(); CELL(w) += POP()),           // n w --
    CODE("?",     IU w = POP(); fout << CELL(w) << " "),     // w --
    /// @}
    /// @defgroup Debug ops
    /// @{
    CODE("here",  PUSH(HERE)),
    CODE("ucase", ucase = POP()),
    CODE("words", words()),
    CODE("'",     IU w = find(next_idiom()); PUSH(w)),
    CODE(".s",    ss_dump()),
    CODE("see",
        IU w = find(next_idiom());
        fout << "[ "; to_s(w); see(PFA(w)); fout << "]" << ENDL),
    CODE("dump",  DU n = POP(); IU a = POP(); mem_dump(a, n)),
    CODE("peek",  DU a = POP(); PUSH(PEEK(a))),
    CODE("poke",  DU a = POP(); POKE(a, POP())),
    CODE("forget",
         IU w = find(next_idiom());
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
    DICT0 = (UFP)dict[EXIT].xt;
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
        //printf("%s=>", idiom);
        int w = find(idiom);                 /// * search through dictionary
        if (w>=0) {                          /// * word found?
            //printf("%s %d\n", dict[w].name, w);
            if (compile && !dict[w].immd) {  /// * in compile mode?
                add_w(w);                    /// * add found word to new colon word
            }
            else CALL(w);                    /// * execute forth word
            continue;
        }
        // try as a number
        char *p;
        int n = static_cast<int>(strtol(idiom, &p, base));
        //printf("%d\n", n);
        if (*p != '\0') {                    /// * not number
            fout << idiom << "? " << ENDL;   ///> display error prompt
            compile = false;                 ///> reset to interpreter mode
            break;                           ///> skip the entire input buffer
        }
        // is a number
        if (compile) {                       /// * add literal when in compile mode
            add_w(DOLIT);                    ///> dovar (+parameter field)
            add_du(n);                       ///> data storage (32-bit integer now)
        }
        else PUSH(n);                        ///> or, add value onto data stack
    }
    if (!compile) ss_dump();
}
///==========================================================================
/// ForthVM front-end handlers
///==========================================================================
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
///
/// ForthVM initalization
///
void ForthVM::init() {
    forth_init();
    //forth_load("/load.txt");    // compile /data/load.txt

    mem_stat();
}
void ForthVM::outer(const char *cmd, void(*callback)(int, const char*)) {
    forth_outer(cmd, callback);
}
void ForthVM::version() {
    LOGF("\n");
    LOGF(APP_NAME);      LOGF(" ");
    LOGF(MAJOR_VERSION); LOGF(".");
    LOGF(MINOR_VERSION);
    LOGF("\n");
}
///
/// memory statistics dump - for heap and stack debugging
///
void ForthVM::mem_stat() {
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
