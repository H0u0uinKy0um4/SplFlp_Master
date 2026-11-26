#pragma once

// Minimal Arduino surface needed to compile ModuleRuntime and ContractSnapshot
// on a host. Only used by the native test environment.
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

class String {
  public:
    String() = default;
    String(char c) : value(1, c) {}
    String(const char* s) : value(s ? s : "") {}
    String(const std::string& s) : value(s) {}
    String(int v) : value(std::to_string(v)) {}
    String(unsigned int v) : value(std::to_string(v)) {}
    String(long v) : value(std::to_string(v)) {}
    String(unsigned long v) : value(std::to_string(v)) {}
    String(long long v) : value(std::to_string(v)) {}
    String(unsigned long long v) : value(std::to_string(v)) {}

    String& operator=(const char* s) {
        value = s ? s : "";
        return *this;
    }
    String& operator+=(const String& other) {
        value += other.value;
        return *this;
    }
    String& operator+=(const char* other) {
        value += other;
        return *this;
    }
    String& operator+=(char c) {
        value += c;
        return *this;
    }

    int length() const { return static_cast<int>(value.size()); }
    const char* c_str() const { return value.c_str(); }
    char operator[](int index) const { return value[index]; }
    char& operator[](int index) { return value[index]; }

    int indexOf(char c, int from = 0) const {
        if (from < 0) from = 0;
        if (from > static_cast<int>(value.size())) return -1;
        const size_t found = value.find(c, static_cast<size_t>(from));
        return found == std::string::npos ? -1 : static_cast<int>(found);
    }

    String substring(int begin) const {
        if (begin < 0) begin = 0;
        if (begin > static_cast<int>(value.size())) return String("");
        return String(value.substr(static_cast<size_t>(begin)));
    }

    String substring(int begin, int end) const {
        if (begin < 0) begin = 0;
        if (end > static_cast<int>(value.size()))
            end = static_cast<int>(value.size());
        if (end < begin) return String("");
        return String(value.substr(static_cast<size_t>(begin),
                                   static_cast<size_t>(end - begin)));
    }

    int toInt() const { return std::atoi(value.c_str()); }

    bool startsWith(const String& prefix) const {
        return value.compare(0, prefix.value.size(), prefix.value) == 0;
    }

    bool endsWith(const char* suffix) const {
        const std::string s(suffix ? suffix : "");
        if (s.size() > value.size()) return false;
        return value.compare(value.size() - s.size(), s.size(), s) == 0;
    }

    void trim() {
        const char* whitespace = " \t\r\n";
        const size_t first = value.find_first_not_of(whitespace);
        if (first == std::string::npos) {
            value.clear();
            return;
        }
        const size_t last = value.find_last_not_of(whitespace);
        value = value.substr(first, last - first + 1);
    }

    std::string value;
};

inline String operator+(const String& a, const String& b) {
    String result = a;
    result += b;
    return result;
}
inline String operator+(const char* a, const String& b) {
    String result(a);
    result += b;
    return result;
}
inline String operator+(const String& a, const char* b) {
    String result = a;
    result += b;
    return result;
}
inline String operator+(char a, const String& b) {
    String result(a);
    result += b;
    return result;
}
inline String operator+(const String& a, char b) {
    String result = a;
    result += b;
    return result;
}
inline bool operator==(const String& a, const String& b) {
    return a.value == b.value;
}
inline bool operator!=(const String& a, const String& b) { return !(a == b); }

unsigned long millis();
void delay(unsigned long ms);
void yield();

struct SerialShim {
    void printf(const char* format, ...);
    void print(const String& value);
    void println(const String& value);
};
extern SerialShim Serial;

template <typename T, typename U>
auto min(T a, U b) -> decltype(a < b ? a : b) {
    return a < b ? a : b;
}
template <typename T, typename U>
auto max(T a, U b) -> decltype(a > b ? a : b) {
    return a > b ? a : b;
}

#define SERIAL_8N1 0
