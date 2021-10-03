/******************************************************************************/
/* ceForth_36.cpp, Version 3.6 : Forth in C                                   */
/******************************************************************************/
/* 28sep21cht   version 3.6                                                   */
/* Primitives/outer coded in C                                                */
/* 01jul19cht   version 3.3                                                   */
/* Macro assembler, Visual Studio 2019 Community                              */
/* 13jul17cht   version 2.3                                                   */
/* True byte code machine with bytecode                                       */
/* Change w to WP, pointing to parameter field                                */
/* 08jul17cht  version 2.2                                                    */
/* Stacks are 256 cell circular buffers                                       */
/* Clean up, delete SP@, SP!, RP@, RP!                                        */
/* 13jun17cht  version 2.1                                                    */
/* Compiled as a C++ console project in Visual Studio Community 2017          */
/******************************************************************************/
#include <string>
#include <iostream>
#include <iomanip>      // setw, setbase, ...

# define  FALSE 0
# define  TRUE  -1
# define  LOGICAL ? -1 : 0
# define  LOWER(x,y) ((unsigned long)(x)<(unsigned long)(y))
# define  pop top = stack[(unsigned char)S--]
# define  push stack[(unsigned char)++S] = top; top =
# define  popR rack[(unsigned char)R--]
# define  pushR rack[(unsigned char)++R]
# define  ALIGN(sz) ((sz) + (-(sz) & 0x3))
# define  analogWrite(c,v,mx) ledcWrite((c),(8191/mx)*min((int)(v),mx))
int  IMEDD = 0x80;

int  P, IP, WP, top, len, nn;
int  lfa, nfa, cfa, pfa;
int  DP, thread, context;
int  ucase = 1, compile = 0, base = 16, loops = 0;
unsigned char R, S, bytecode, c;
int* Pointer;
String idiom, s;

int stack[256] = { 0 };
int rack[256] = { 0 };
int data[16000] = {};
unsigned char* cData = (unsigned char*)data;

void next() { P = data[IP >> 2]; WP = P; IP += 4; }
void nest() { pushR = IP; IP = WP + 4; next(); }
void unnest() { IP = popR; next(); }
void comma(int n) { data[DP >> 2] = n; DP += 4; }
void comma_s(int lex, String s) {
  comma(lex); len = s.length(); cData[DP++] = len;
  for (int i = 0; i < len; i++) { cData[DP++] = s[i]; }
  while (DP & 3) { cData[DP++] = 0; }
}
String next_idiom(char delim = 0) {
  delim = delim ? delim:' '; return Serial.readStringUntil(delim);
}
void dot_r(int n, int v) {
  String s = (String)v;
  for( int i =s.length();i<n;i++) Serial.print(' ');
  Serial.print(v,base);
}
int find(String s) {
  int len_s = s.length();
  nfa = context;
  while (nfa) {
    lfa = nfa - 4;
    len = (int)cData[nfa] & 0x1f;
    if (len_s == len) {
      int success = 1;
      for (int i = 0; i < len; i++) {
        if (s[i] != cData[nfa + 1 + i])
        {success = 0; break;}
      }
      if (success) {cfa = ALIGN(nfa + len + 1);return cfa; }
    }
    nfa = data[lfa >> 2];
  }
  return 0;
}
void words() {
  Serial.println();
  nfa = context; // CONTEXT
  while (nfa) {
    lfa = nfa - 4;
    len = (int)cData[nfa] & 0x1f;
    for (int i = 0; i < len; i++)
      Serial.print((char)cData[nfa + 1 + i]);
    Serial.print(' ');
    nfa = data[lfa >> 2];
  }
  Serial.println();
}
void CheckSum() {
  int i; pushR = P; char sum = 0;
  Serial.print(P,base);Serial.print(": ");
  for (i = 0; i < 16; i++) {
    sum += cData[P];
    if ((int)cData[P]<16) Serial.print(' ');
    Serial.print((int)cData[P++],base);Serial.print(' ');
  }
  if ((sum&0xff)<16) Serial.print(' ');
  Serial.print(sum & 0xff, base);Serial.print("  ");
  P = popR;
  for (i = 0; i < 16; i++) {
    sum = cData[P++] & 0x7f;
    Serial.print((char)(sum < 0x20) ? '_' : sum);
  }
  Serial.println();
}
void dump() {// a n --
  Serial.println();
  len = top / 16; pop; P = top; pop;
  for (int i = 0; i < len; i++) { CheckSum(); }
}
void ss_dump() {
  Serial.print("\n< "); 
  for (int i = S - 4; i < S + 1; i++) { 
  Serial.print(stack[i],base);Serial.print(" "); }
  Serial.print(top,base);Serial.println(" >ok");
}
void(*primitives[120])(void) = {
  /// Stack ops
  /* -1 "ret" */ [] {next(); },
  /* 0 "opn" */ [] {},
  /* 1 "nest" */ [] {nest(); },
  /* 2 "unnest" */ [] {unnest(); },
  /* 3 "dup" */ [] {stack[++S] = top; },
  /* 4 "drop" */ [] {pop; },
  /* 5 "over" */ [] {push stack[(S - 1)]; },
  /* 6 "swap" */ [] {nn = top; top = stack[S];
  stack[S] = nn; },
  /* 7 "rot" */ [] {nn = stack[(S - 1)];
  stack[(S - 1)] = stack[S];
  stack[S] = top; top = nn; },
  /* 8 "pick" */ [] {top = stack[(S - top)]; },
  /* 9 ">r" */ [] {rack[++R] = top; pop; },
  /* 10 "r>" */ [] {push rack[R--]; },
  /* 11 "r@" */ [] {push rack[R]; },
  /// Stack ops - double
  /* 12 "2dup" */ [] {push stack[(S - 1)]; push stack[(S - 1)]; },
  /* 13 "2drop" */ [] {pop; pop; },
  /* 14 "2over"*/ [] {push stack[(S - 3)]; push stack[(S - 3)]; },
  /* 15 "2swap" */ [] {
  int n = top; pop; int m = top; pop; int l = top; pop; int i = top; pop;
  push m; push n; push i; push l; },
  /// ALU ops
  /* 16 "+" */ [] {nn = top; pop; top += nn; },
  /* 17 "-" */ [] {nn = top; pop; top -= nn; },
  /* 18 "*" */ [] {nn = top; pop; top *= nn; },
  /* 19 "/" */ [] {nn = top; pop; top /= nn; },
  /* 20 "mod" */ [] {nn = top; pop; top %= nn; },
  /* 21 "* /" */ [] {nn = top; pop; int m = top; pop;
  int l = top; pop; push(m * l) / nn; },
  /* 22 "/mod" */ [] {nn = top; pop; int m = top; pop;
  push(m % nn); push(m / nn); },
  /* 23 "* /mod" */ [] {nn = top; pop; int m = top; pop;
  int l = top; pop; push((m * l) % nn); push((m * l) / nn); },
  /* 24 "and" */ [] {top &= stack[S--]; },
  /* 25 "or"  */ [] {top |= stack[S--]; },
  /* 26 "xor" */ [] {top ^= stack[S--]; },
  /* 27 "abs" */ [] {top = abs(top); },
  /* 28 "negate" */ [] {top = -top; },
  /* 29 "max" */ [] {nn = top; pop; top = std::max(top, nn); },
  /* 30 "min" */ [] {nn = top; pop; top = std::min(top, nn); },
  /* 31 "2*"  */ [] {top *= 2; },
  /* 32 "2/"  */ [] {top /= 2; },
  /* 33 "1+"  */ [] {top += 1; },
  /* 34 "1-"  */ [] {top += -1; },
  /// Logic ops
  /* 35 "0=" */ [] {top = (top == 0) LOGICAL; },
  /* 36 "0<" */ [] {top = (top < 0) LOGICAL; },
  /* 37 "0>" */ [] {top = (top > 0) LOGICAL; },
  /* 38 "="  */ [] {nn = top; pop; top = (top == nn) LOGICAL; },
  /* 39 ">"  */ [] {nn = top; pop; top = (top > nn) LOGICAL; },
  /* 40 "<"  */ [] {nn = top; pop; top = (top < nn) LOGICAL; },
  /* 41 "<>" */ [] {nn = top; pop; top = (top != nn) LOGICAL; },
  /* 42 ">=" */ [] {nn = top; pop; top = (top >= nn) LOGICAL; },
  /* 43 "<=" */ [] {nn = top; pop; top = (top <= nn) LOGICAL; },
  /// IO ops
  /* 44 "base@" */ [] {push base; },
  /* 45 "base!" */ [] {base = top; pop;  },
  /* 46 "hex" */ [] {base = 16;  },
  /* 47 "decimal" */ [] {base = 10; },
  /* 48 "cr" */ [] {Serial.println(); },
  /* 49 "." */ [] {Serial.print(top,base);Serial.print(" "); pop; },
  /* 50 ".r" */ [] {nn = top; pop; dot_r(nn, top); pop; },
  /* 51 "u.r" */ [] {nn = top; pop; dot_r(nn, abs(top)); pop; },
  /* 52 ".s" */ [] {ss_dump(); },
  /* 53 "key" */ [] {push(next_idiom()[0]); },
  /* 54 "emit" */ [] {char b = (char)top; pop; Serial.print(b); },
  /* 55 "space" */ [] {Serial.print(' '); },
  /* 56 "spaces" */ [] {nn = top; pop; for (int i = 0; i < nn; i++) Serial.print(' '); },
  /// Literal ops
  /* 57 "dostr" */ [] {int p = IP; push p; len = cData[p];
  p += (len + 1); p += (-p & 3); IP = p; },
  /* 58 "dotstr" */ [] {int p = IP; len = cData[p++];
  for (int i = 0; i < len; i++) Serial.print((char)cData[p++]);
  p += (-p & 3); IP = p; },
  /* 59 "dolit" */ [] {push data[IP >> 2]; IP += 4; },
  /* 60 "dovar" */ [] {push nn + 4; },
  /* 61 [  */ [] {compile = 0; },
  /* 62 ]  */ [] {compile = 1; },
  /* 63 (  */ [] {next_idiom(')'); },
  /* 64 .( */ [] {Serial.print(next_idiom(')')); },
  /* 65 \  */ [] {next_idiom('\n'); },
  /* 66 $" */ [] {
  String s = next_idiom('"'); len = s.length();
  nn = find("dostr");
  comma_s(nn, s); },
  /* 67 ." */ [] {
  String s = next_idiom('"'); len = s.length();
  nn = find("dotstr");
  comma_s(nn, s); },
  /// Branching ops
  /* 68 "branch" */ [] { IP = data[IP >> 2]; next(); },
  /* 69 "0branch" */ [] {
  if (top == 0) IP = data[IP >> 2];
  else IP += 4;  pop; next(); },
  /* 70 "donext" */ [] {
  if (rack[R]) {
  rack[R] -= 1; IP = data[IP >> 2];
  }
  else { IP += 4;  R--; }
  next(); },
  /* 71 "if" */ [] {
  comma(find("0branch")); push DP;
  comma(0); },
  /* 72 "else" */ [] {
  comma(find("branch")); data[top >> 2] = DP + 4;
  top = DP; comma(0);  },
  /* 73 "then" */ [] {
  data[top >> 2] = DP; pop; },
  /// Loops
  /* 74 "begin" */ [] { push DP; },
  /* 75 "while" */ [] {
  comma(find("0branch")); push DP;
  comma(0); },
  /* 76 "repeat" */ [] {
  comma(find("branch")); nn = top; pop;
  comma(top); pop; data[nn >> 2] = DP; },
  /* 77 "again" */ [] {
  comma(find("branch"));
  comma(top); pop; },
  /* 78 "until" */ [] {
  comma(find("0branch"));
  comma(top); pop; },
  ///  For loops
  /* 79 "for" */ [] {comma((find(">r"))); push DP; },
  /* 80 "aft" */ [] {pop;
  comma((find("branch"))); comma(0); push DP; push DP - 4; },
  /* 81 "next" */ [] {
  comma(find("donext")); comma(top); pop; },
  ///  Compiler ops
  /* 82 "exit" */ [] {IP = popR; next(); },
  /* 83 "docon" */ [] {push data[(nn + 4) >> 2]; },
  /* 84 ":" */ [] {
  String s = next_idiom();
  thread = DP + 4; comma_s(context, s);
  comma(cData[find("nest")]); compile = 1; },
  /* 85 ";" */ [] {
  context = thread; compile = 0;
  comma(find("unnest")); },
  /* 86 "variable" */ [] {
  String s = next_idiom();
  thread = DP + 4; comma_s(context, s);
  context = thread;
  comma(cData[find("dovar")]); comma(0);
  },
  /* 87 "constant" */ [] {
  String s = next_idiom();
  thread = DP + 4; comma_s(context, s);
  context = thread;
  comma(cData[find("docon")]); comma(top); pop; },
  /* 88 "@" */ [] {top = data[top >> 2]; },
  /* 89 "!" */ [] {int a = top; pop; data[a >> 2] = top; pop; },
  /* 90 "?" */ [] {Serial.print(data[top >> 2],base);Serial.print(" "); pop; },
  /* 91 "+!" */ [] {int a = top; pop; data[a >> 2] += top; pop; },
  /* 92 "allot" */ [] {nn = top; pop;
  for (int i = 0; i < nn; i++) cData[DP++] = 0; },
  /* 93 "," */ [] {comma(top); pop; },
  /// metacompiler
  /* 94 "create" */ [] {
  String s = next_idiom();
  thread = DP + 4; comma_s(context, s);
  context = thread;
  comma(find("nest")); comma(find("dovar")); },
  /* 95 "does" */ [] {
  comma(find("nest")); }, // copy words after "does" to new the word
  /* 96 "to" */ [] {// n -- , compile only
  int n = find(next_idiom());
  data[(cfa + 4) >> 2] = top; pop; },
  /* 97 "is" */ [] {// w -- , execute only
  int n = find(next_idiom());
  data[cfa >> 2] = top; pop; },
  /* 98 "[to]" */ [] {
  int n = data[IP >> 2]; data[(n + 4) >> 2] = top; pop; },
  /// Debug ops
  /* 99 "bye" */ [] {exit(0); },
  /* 100 "here" */ [] {push DP; },
  /* 101 "words" */ [] {words(); },
  /* 102 "dump" */ [] {dump(); },
  /* 103 "'" */ [] {push find(next_idiom()); },
  /* 104 "see" */ [] {
  nn = find(next_idiom());
  for (int i = 0; i < 20; i++) Serial.print(data[(nn >> 2) + i],base);
  Serial.println(); },
  /* 105 "ucase" */ [] {ucase = top; pop; },
  /* 106 "clock" */ [] {push millis(); },
  /* 107 "delay" */ [] {delay(top);pop; },
  /* 108 "poke" */ [] {Pointer = (int*)top; *Pointer = stack[(unsigned char)S--]; pop; },
  /* 109 "peek" */ [] {Pointer = (int*)top; top = *Pointer; },
  /* 110 "pin"  */ [] {top=digitalRead(top); },
  /* 111 "in" */ [] {Pointer = (int*)top; *Pointer = stack[(unsigned char)S--]; pop; },
  /* 112 "out" */ [] {int p = top;pop; digitalWrite(p, top);pop; },
  /* 113 "adc"  */ [] {top = (int) analogRead(top); },
  /* 114 "duty" */ [] {nn=top; pop; analogWrite(nn,top,255); pop; },
  /* 115 "attach"  */ [] {nn=top; pop; ledcAttachPin(top,nn); pop; },
  /* 116 "setup" */ [] {nn=top; pop; int freq=top;pop;ledcSetup(nn,freq,top); pop; },
  /* 117 "tone" */ [] {nn=top; pop; ledcWriteTone(nn,top); pop; },
  /* 118 "boot" */ [] {DP = find("boot") + 4; lfa = nfa; }
};
// outer interpreter
void CODE(int lex, const char seq[]) {
  len = lex & 31;
  comma(lfa); lfa = DP; cData[DP++] = lex;
  for (int i = 0; i < len; i++) { cData[DP++] = seq[i]; }
  while (DP & 3) { cData[DP++] = 0; }
  comma(P++); /// sequential bytecode
  Serial.print(seq);Serial.print(":");Serial.print(P - 1,base);Serial.print(',');
  Serial.print(DP - 4,base);Serial.print(' ');
}
void run(int n) {
  P = n; WP = n; IP = 0; R = 0;
  do {
    bytecode = cData[P++];
    primitives[bytecode](); /// execute colon
  } while (R != 0);
}
void outer() {
  while (true) {
    idiom = Serial.readStringUntil(' ');
    if (idiom.length()==0) return;
    if (idiom[0]=='\n') {ss_dump();return;}
    if (find(idiom)) {
      if (compile && (((int)cData[nfa] & 0x80) == 0))
        comma(cfa);
      else  run(cfa);
    }
    else {
      char* p;
      int n = (int)strtol(idiom.c_str(), &p, base);
     if (*p != '\0') {///  not number
        Serial.print(idiom);Serial.println("? ");
        compile = 0; return;
      }
      else {
        if (compile) { comma(find("dolit")); comma(n); }
        else { push n; }
      }
    }
    if (Serial.peek() == '\0' && !compile) ss_dump();
  } ///  * dump stack and display ok prompt
}
///  Main Program
void setup() {
  Serial.begin(115200);
  delay(100);
  cData = (unsigned char*)data;
  IP = 0; lfa = 0; P = 0;
  S = 0; R = 0;
  Serial.println("Build dictionary");
  // Kernel
  CODE(3, "ret");
  CODE(3, "nop");
  CODE(4, "nest");
  CODE(6, "unnest");
  CODE(3, "dup");
  CODE(4, "drop");
  CODE(4, "over");
  CODE(4, "swap");
  CODE(3, "rot");
  CODE(4, "pick");
  CODE(2, ">r");
  CODE(2, "r>");
  CODE(2, "r@");
  CODE(4, "2dup");
  CODE(5, "2drop");
  CODE(5, "2over");
  CODE(5, "2swap");
  CODE(1, "+");
  CODE(1, "-");
  CODE(1, "*");
  CODE(1, "/");
  CODE(3, "mod");
  CODE(2, "*/");
  CODE(4, "/mod");
  CODE(5, "*/mod");
  CODE(3, "and");
  CODE(2, "or");
  CODE(3, "xor");
  CODE(3, "abs");
  CODE(6, "negate");
  CODE(3, "max");
  CODE(3, "min");
  CODE(2, "2*");
  CODE(2, "2/");
  CODE(2, "1+");
  CODE(2, "1-");
  CODE(2, "0=");
  CODE(2, "0<");
  CODE(2, "0>");
  CODE(1, "=");
  CODE(1, ">");
  CODE(1, "<");
  CODE(2, "<>");
  CODE(2, ">=");
  CODE(2, "<=");
  CODE(5, "base@");
  CODE(5, "base!");
  CODE(3, "hex");
  CODE(7, "decimal");
  CODE(2, "cr");
  CODE(1, ".");
  CODE(2, ".r");
  CODE(3, "u.r");
  CODE(2, ".s");
  CODE(3, "key");
  CODE(4, "emit");
  CODE(5, "space");
  CODE(6, "spaces");
  CODE(5, "dostr");
  CODE(6, "dotstr");
  CODE(5, "dolit");
  CODE(5, "dovar");
  CODE(1 + IMEDD, "[");
  CODE(1, "]");
  CODE(1 + IMEDD, "(");
  CODE(2 + IMEDD, ".(");
  CODE(1 + IMEDD, "\\");
  CODE(2 + IMEDD, "$\"");
  CODE(2 + IMEDD, ".\"");
  CODE(6, "branch");
  CODE(7, "0branch");
  CODE(6, "donext");
  CODE(2 + IMEDD, "if");
  CODE(4 + IMEDD, "else");
  CODE(4 + IMEDD, "then");
  CODE(5 + IMEDD, "begin");
  CODE(5 + IMEDD, "while");
  CODE(6 + IMEDD, "repeat");
  CODE(5 + IMEDD, "again");
  CODE(5 + IMEDD, "until");
  CODE(3 + IMEDD, "for");
  CODE(3 + IMEDD, "aft");
  CODE(4 + IMEDD, "next");
  CODE(4, "exit");
  CODE(5, "docon");
  CODE(1, ":");
  CODE(1 + IMEDD, ";");
  CODE(8, "variable");
  CODE(8, "constant");
  CODE(1, "@");
  CODE(1, "!");
  CODE(1, "?");
  CODE(2, "+!");
  CODE(5, "allot");
  CODE(1, ",");
  CODE(6, "create");
  CODE(4, "does");
  CODE(2, "to");
  CODE(2, "is");
  CODE(4, "[to]");
  CODE(3, "bye");
  CODE(4, "here");
  CODE(5, "words");
  CODE(4, "dump");
  CODE(1, "'");
  CODE(3, "see");
  CODE(5, "ucase");
  CODE(5, "clock");
  CODE(5, "delay");
  CODE(4, "poke");
  CODE(4, "peek");
  CODE(3, "pin");
  CODE(2, "in");
  CODE(3, "out");
  CODE(3, "adc");
  CODE(4, "duty");
  CODE(6, "attach");
  CODE(5, "setup");
  CODE(4, "tone");
  CODE(4, "boot");
  context = DP - 12;
  Serial.print("\n\nPointers DP=");Serial.print(DP,base);Serial.print(" lfa=");
  Serial.print(context,base);Serial.print(" Words=");Serial.println(P,base);
// Setup ESP32 pins
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
// dump dictionary
  Serial.print("\nDump dictionary\n");
  P = 0;
  for (len = 0; len < 110; len++) { CheckSum(); }
  ledcSetup(0,   0, 13);
// Boot Up
  P = 0; WP = 0; IP = 0; S = 0; R = 0;
  top = -1;
  Serial.print("\nceForth v8.5, 30sep21cht\n");
  words();
}
void loop(){
  while(Serial.available()==0){};
  outer();
}
/* End of ceforth_85.cpp */ 
