// state_io.h - one reader/writer for save states, so that saving and loading are the SAME code path (a field can not be
// saved without also being loaded). The file is plain text:  "[section]" lines, "name=value" lines, and "name:count" followed
// by hex lines for byte/word blocks. Lines starting with '#' and blank lines are ignored.
//
//   StateIO io;                       save:  call the state functions, then io.str() is the file text
//   StateIO io(text, false);          load, pass 1 (dry run): validates names / counts / numbers, changes nothing
//   StateIO io(text, true);           load, pass 2: same calls, now the values are stored
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

class StateIO {
public:
    StateIO() : saving_(true) {}
    StateIO(const std::string& text, bool apply) : saving_(false), apply_(apply), in_(&text) {}

    bool saving() const { return saving_; }
    bool ok() const { return err_.empty(); }
    const std::string& error() const { return err_; }
    const std::string& str() const { return out_; }
    bool applying() const { return saving_ || apply_; }     // false only in the dry-run pass

    void comment(const char* t) { if (saving_) { out_ += "# "; out_ += t; out_ += "\n"; } }
    void section(const char* name) {
        if (saving_) { out_ += "["; out_ += name; out_ += "]\n"; return; }
        std::string l; if (!line(l)) return;
        if (l != std::string("[") + name + "]") fail(std::string("expected section [") + name + "], found: " + l);
    }

    void set_error(const std::string& m) { fail(m); }
    // like val(), but a loaded value is stored even in the dry-run pass (for header fields that are checked before applying)
    template <class T> void val_in(const char* name, T& v) { bool old = apply_; apply_ = true; val(name, v); apply_ = old; }

    template <class T> void val(const char* name, T& v) {
        static_assert(std::is_integral<T>::value, "integral only");
        if (saving_) {
            char b[40];
            if (std::is_same<T, bool>::value) std::snprintf(b, sizeof b, "%d", (int)v);
            else if (std::is_signed<T>::value) std::snprintf(b, sizeof b, "%lld", (long long)v);
            else std::snprintf(b, sizeof b, "%llu", (unsigned long long)v);
            put(name, b); return;
        }
        std::string s; if (!field(name, s)) return;
        char* e = nullptr; errno_ = 0;
        long long x = std::is_signed<T>::value ? std::strtoll(s.c_str(), &e, 10) : (long long)std::strtoull(s.c_str(), &e, 10);
        if (s.empty() || *e) { fail(std::string("bad number for ") + name + ": " + s); return; }
        if (apply_) v = std::is_same<T, bool>::value ? (T)(x != 0) : (T)x;
    }
    void val(const char* name, float& v) {
        if (saving_) { char b[40]; std::snprintf(b, sizeof b, "%.9g", (double)v); put(name, b); return; }
        std::string s; if (!field(name, s)) return;
        char* e = nullptr; float x = std::strtof(s.c_str(), &e);
        if (s.empty() || *e) { fail(std::string("bad number for ") + name + ": " + s); return; }
        if (apply_) v = x;
    }
    void val(const char* name, double& v) {
        if (saving_) { char b[40]; std::snprintf(b, sizeof b, "%.17g", v); put(name, b); return; }
        std::string s; if (!field(name, s)) return;
        char* e = nullptr; double x = std::strtod(s.c_str(), &e);
        if (s.empty() || *e) { fail(std::string("bad number for ") + name + ": " + s); return; }
        if (apply_) v = x;
    }

    void bytes(const char* name, uint8_t* p, size_t n) { block(name, p, n, 1); }
    void words(const char* name, uint16_t* p, size_t n) { block(name, p, n, 2); }
    void u32list(const char* name, std::vector<uint32_t>& v) {      // variable length list of 32-bit numbers, one line
        if (saving_) {
            std::string s = std::to_string(v.size()) + ":";
            char b[16]; for (uint32_t x : v) { std::snprintf(b, sizeof b, " %x", x); s += b; }
            put(name, s.c_str()); return;
        }
        std::string s; if (!field(name, s)) return;
        char* e = nullptr; unsigned long n = std::strtoul(s.c_str(), &e, 10);
        if (*e != ':' || n > 100000) { fail(std::string("bad list for ") + name); return; }
        std::vector<uint32_t> t; ++e;
        for (unsigned long i = 0; i < n; ++i) { char* e2 = nullptr; unsigned long x = std::strtoul(e, &e2, 16); if (e2 == e) { fail(std::string("short list for ") + name); return; } t.push_back((uint32_t)x); e = e2; }
        v = t;                                                      // lists are only read for checking, always safe to assign
    }

private:
    bool saving_ = false, apply_ = false;
    const std::string* in_ = nullptr; size_t pos_ = 0; int lineno_ = 0, errno_ = 0;
    std::string out_, err_;

    void fail(const std::string& m) { if (err_.empty()) err_ = "line " + std::to_string(lineno_) + ": " + m; }
    void put(const char* name, const char* v) { out_ += name; out_ += "="; out_ += v; out_ += "\n"; }
    bool line(std::string& l) {                                     // next meaningful line, trimmed
        if (!ok()) return false;
        while (pos_ < in_->size()) {
            size_t nl = in_->find('\n', pos_); if (nl == std::string::npos) nl = in_->size();
            std::string s = in_->substr(pos_, nl - pos_); pos_ = nl + 1; ++lineno_;
            size_t b = s.find_first_not_of(" \t\r"), e = s.find_last_not_of(" \t\r");
            if (b == std::string::npos || s[b] == '#') continue;
            l = s.substr(b, e - b + 1); return true;
        }
        fail("unexpected end of file"); return false;
    }
    bool field(const char* name, std::string& v) {
        std::string l; if (!line(l)) return false;
        std::string pre = std::string(name) + "=";
        if (l.compare(0, pre.size(), pre) != 0) { fail(std::string("expected '") + name + "=', found: " + l.substr(0, 40)); return false; }
        v = l.substr(pre.size()); return true;
    }
    static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
    void block(const char* name, void* p, size_t n, int w) {        // n items of w bytes (1 or 2); hex, 32 items per line
        const size_t per = 32;
        if (saving_) {
            out_ += name; out_ += ":" + std::to_string(n) + "\n";
            char b[8];
            for (size_t i = 0; i < n; i += per) {
                for (size_t j = i; j < n && j < i + per; ++j) {
                    if (w == 1) std::snprintf(b, sizeof b, "%02x", ((uint8_t*)p)[j]); else std::snprintf(b, sizeof b, "%04x", ((uint16_t*)p)[j]);
                    out_ += b;
                }
                out_ += "\n";
            }
            return;
        }
        std::string l; if (!line(l)) return;
        std::string pre = std::string(name) + ":" + std::to_string(n);
        if (l != pre) { fail(std::string("expected '") + pre + "', found: " + l.substr(0, 40)); return; }
        size_t got = 0;
        while (got < n) {
            if (!line(l)) return;
            size_t digits = 0; for (char c : l) { if (hexv(c) < 0) { fail(std::string("bad hex digit in ") + name); return; } ++digits; }
            if (digits % (2 * w)) { fail(std::string("partial item in ") + name); return; }
            size_t items = digits / (2 * w);
            if (got + items > n) { fail(std::string("too much data in ") + name); return; }
            for (size_t k = 0; k < items; ++k) {
                unsigned v = 0; for (int q = 0; q < 2 * w; ++q) v = v * 16 + hexv(l[k * 2 * w + q]);
                if (apply_) { if (w == 1) ((uint8_t*)p)[got + k] = (uint8_t)v; else ((uint16_t*)p)[got + k] = (uint16_t)v; }
            }
            got += items;
        }
    }
};
