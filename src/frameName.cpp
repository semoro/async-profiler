/*
 * Copyright 2017 Andrei Pangin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cxxabi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "frameName.h"
#include "arguments.h"
#include "vmStructs.h"


FrameName::FrameName(int style, Mutex& thread_names_lock, ThreadMap& thread_names) :
    _cache(),
    _style(style),
    _thread_names_lock(thread_names_lock),
    _thread_names(thread_names)
{
    // Require printf to use standard C format regardless of system locale
    _saved_locale = uselocale(newlocale(LC_NUMERIC_MASK, "C", (locale_t)0));
    memset(_buf, 0, sizeof(_buf));
}

FrameName::~FrameName() {
    freelocale(uselocale(_saved_locale));
}

const char* FrameName::cppDemangle(const char* name) {
    if (name != NULL && name[0] == '_' && name[1] == 'Z') {
        int status;
        char* demangled = abi::__cxa_demangle(name, NULL, NULL, &status);
        if (demangled != NULL) {
            strncpy(_buf, demangled, sizeof(_buf) - 1);
            free(demangled);
            return _buf;
        }
    }
    return name;
}

char* FrameName::javaMethodName(jmethodID method) {
    jclass method_class;
    char* class_name = NULL;
    char* method_name = NULL;
    char* method_sig = NULL;
    char* result;

    jvmtiEnv* jvmti = VM::jvmti();
    jvmtiError err;

    if ((err = jvmti->GetMethodName(method, &method_name, &method_sig, NULL)) == 0 &&
        (err = jvmti->GetMethodDeclaringClass(method, &method_class)) == 0 &&
        (err = jvmti->GetClassSignature(method_class, &class_name, NULL)) == 0) {

        result = javaClassName(class_name, strlen(class_name), _style);
        strcat(result, ".");
        appendMethodName(result, method_name);
        if (_style & STYLE_SIGNATURES) {
            appendDemangledJvmSignature(result, method_sig, _style);
        }
        if (_style & STYLE_ANNOTATE) strcat(result, "_[j]");
    } else {
        snprintf(_buf, sizeof(_buf), "[jvmtiError %d]", err);
        result = _buf;
    }

    jvmti->Deallocate((unsigned char*)class_name);
    jvmti->Deallocate((unsigned char*)method_sig);
    jvmti->Deallocate((unsigned char*)method_name);

    return result;
}

char* FrameName::javaClassName(const char* symbol, int length, int style) {
    char* result = _buf;

    result[0] = '\0';
    appendDemangledJvmSignature(result, symbol, style);

    return result;
}

const char* FrameName::name(ASGCT_CallFrame& frame) {
    if (frame.method_id == NULL) {
        return "[unknown]";
    }

    switch (frame.bci) {
        case BCI_NATIVE_FRAME:
            return cppDemangle((const char*)frame.method_id);

        case BCI_SYMBOL: {
            VMSymbol* symbol = (VMSymbol*)frame.method_id;
            char* class_name = javaClassName(symbol->body(), symbol->length(), _style | STYLE_DOTTED);
            return strcat(class_name, _style & STYLE_DOTTED ? "" : "_[i]");
        }

        case BCI_SYMBOL_OUTSIDE_TLAB: {
            VMSymbol* symbol = (VMSymbol*)((uintptr_t)frame.method_id ^ 1);
            char* class_name = javaClassName(symbol->body(), symbol->length(), _style | STYLE_DOTTED);
            return strcat(class_name, _style & STYLE_DOTTED ? " (out)" : "_[k]");
        }

        case BCI_THREAD_ID: {
            int tid = (int)(uintptr_t)frame.method_id;
            MutexLocker ml(_thread_names_lock);
            ThreadMap::iterator it = _thread_names.find(tid);
            if (it != _thread_names.end()) {
                snprintf(_buf, sizeof(_buf), "[%s tid=%d]", it->second.c_str(), tid);
            } else {
                snprintf(_buf, sizeof(_buf), "[tid=%d]", tid);
            }
            return _buf;
        }

        case BCI_ERROR: {
            snprintf(_buf, sizeof(_buf), "[%s]", (const char*)frame.method_id);
            return _buf;
        }

        default: {
            JMethodCache::iterator it = _cache.lower_bound(frame.method_id);
            if (it != _cache.end() && it->first == frame.method_id) {
                return it->second.c_str();
            }

            const char* newName = javaMethodName(frame.method_id);
            _cache.insert(it, JMethodCache::value_type(frame.method_id, newName));
            return newName;
        }
    }
}



char* appendStr(char* out, const char* str) {
    size_t len = strlen(str);
    strncat(out, str, len);
    return out + len;
}


char* FrameName::processJvmName(char* out, const char *signature, int& s_pos, int style) {


    char* initial_out = out;
    while(true) {
        char cq = signature[s_pos];
        switch (cq) {
            case NULL:
            case ';':
                return out;
            case ',':
                out = appendStr(out, "\\,");
                break;
            case '\\':
                out = appendStr(out, "\\\\");
                break;
            case '(':
                out = appendStr(out, "\\(");
                break;
            case ')':
                out = appendStr(out, "\\)");
                break;
            case '/':
                if (style & STYLE_SIMPLE) {
                    memset(initial_out, NULL, initial_out - out);
                    break;
                }
                if (style & STYLE_DOTTED) {
                    cq = '.';
                }
            default:
                out = strncat(out, &cq, 1);
                out++;
                break;
        }
        s_pos++;
    }
}

const char* primitiveType(char c) {
    switch (c) {
        case 'B': return "byte";
        case 'C': return "char";
        case 'D': return "double";
        case 'F': return "float";
        case 'I': return "int";
        case 'J': return "long";
        case 'S': return "short";
        case 'V': return "void";
        case 'Z': return "boolean";
        default:
            return "WTF!";
    }
}


void FrameName::appendDemangledJvmSignature(char* out, const char *signature, int style) {
    out = out + strlen(out); // shift to working pos
    if (signature != NULL) {
        int s_pos = 0;
        int arrayDim = 0;
        bool in_args = false;
        while (true) {
            char c = signature[s_pos];

            switch (c) {
                case 0:
                    return;
                case '[':
                    arrayDim++;
                    s_pos++;
                    continue;
                case 'L':
                    s_pos++;
                    out = processJvmName(out, signature, s_pos, style);
                    break;
                case '(':
                    out = appendStr(out, "(");
                    in_args = true;
                    s_pos++;
                    continue;
                case ')':
                    out = appendStr(out, ") ");
                    in_args = false;
                    s_pos++;
                    continue;
                default:
                    out = appendStr(out, primitiveType(c));
                    break;
            }
            for (;arrayDim > 0; arrayDim--) {
                out = appendStr(out, "[]");
            }
            s_pos++;
            if (in_args && signature[s_pos] != ')') out = appendStr(out, ", ");
        }
    }
}

void FrameName::appendMethodName(char *out, const char *name) {
    int s_pos = 0;
    processJvmName(out, name, s_pos, 0);
}
