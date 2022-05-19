#ifndef __ESP32FORTH_SRC_EFORTH_H
#define __ESP32FORTH_SRC_EFORTH_H
#include <stdint.h>     // uintxx_t
///
/// control whether lambda capture parameter
/// Note:
///    LAMBDA_OK       0 cut 80ms/1M cycles
///    RANGE_CHECK     0 cut 100ms/1M cycles
///    INLINE            cut 545ms/1M cycles
///
#define LAMBDA_OK       0
#define RANGE_CHECK     0
#define INLINE          __attribute__((always_inline))
///
/// memory block configuation
///
#define E4_SS_SZ        64
#define E4_RS_SZ        64
#define E4_DICT_SZ      2048
#define E4_PMEM_SZ      (64*1024)
///
/// logical units (instead of physical) for type check and portability
///
typedef uint16_t  IU;    // instruction pointer unit
typedef int32_t   DU;    // data unit
typedef uint16_t  U16;   // unsigned 16-bit integer
typedef uint8_t   U8;    // byte, unsigned character
typedef uintptr_t UFP;   // function pointer
///
/// alignment macros
///
#define ALIGN(sz)       ((sz) + (-(sz) & 0x1))
#define ALIGN16(sz)     ((sz) + (-(sz) & 0xf))
///
/// Arduino specific macros
///
#define analogWrite(c,v,mx) ledcWrite((c),(8191/mx)*min((int)(v),mx))
#define ENDL                endl; fout_cb(fout.str().length(), fout.str().c_str()); fout.str("")
///
/// translate ESP32 String to/from Forth input/output streams (in C++ string)
///
#define LOGF(s)  Serial.print(F(s))
#define LOG(v)   Serial.print(v)
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

    List()  { v = N ? new T[N] : 0; }      /// dynamically allocate array memory
    ~List() { if (N) delete[] v;   }       /// free the memory

    List &operator=(T *a)   INLINE { v = a; return *this; }
    T    &operator[](int i) INLINE { return i < 0 ? v[idx + i] : v[i]; }

#if RANGE_CHECK
    T pop()     INLINE {
        if (idx>0) return v[--idx];
        throw "ERR: List empty";
    }
    T push(T t) INLINE {
        if (idx<N) return v[max=idx++] = t;
        throw "ERR: List full";
    }
#else  // RANGE_CHECK
    T pop()     INLINE { return v[--idx]; }
    T push(T t) INLINE { return v[max=idx++] = t; }
#endif // RANGE_CHECK
    void push(T *a, int n) INLINE { for (int i=0; i<n; i++) push(*(a+i)); }
    void merge(List& a)    INLINE { for (int i=0; i<a.idx; i++) push(a[i]);}
    void clear(int i=0)    INLINE { idx=i; }
};
///
/// universal functor and Code class
/// Note:
///   * 8-byte on 32-bit machine, 16-byte on 64-bit machine
///
#if LAMBDA_OK
struct fop { virtual void operator()(IU) = 0; };
template<typename F>
struct XT : fop {           // universal functor (no STD dependency)
    F fp;
    XT(F &f) : fp(f) {}
    void operator()(IU c) INLINE { fp(c); }
};
struct Code {
    const char *name = 0;   /// name field
    union {                 /// either a primitive or colon word
        fop xt = 0;         /// lambda pointer (4-byte aligned, thus bit[0,1]=0)
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
#define CODE(s, g) { s, [](int c){ g; }, 0 }
#define IMMD(s, g) { s, [](int c){ g; }, 1 }
#else  // LAMBDA_OK
typedef void (*fop)();      /// function pointer
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
#define CODE(s, g) { s, []{ g; }, 0 }
#define IMMD(s, g) { s, []{ g; }, 1 }
#endif // LAMBDA_OK

class ForthVM {
public:
    void   init();
    void   outer(const char *cmd, void(*callback)(int, const char*));
    void   version();
    void   mem_stat();
};
#endif // __ESP32FORTH_SRC_EFORTH_H
