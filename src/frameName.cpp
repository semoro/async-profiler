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


FrameName::FrameName(int style, Mutex& thread_names_lock, ThreadMap& thread_names, bool use_bci) :
    _cache(),
    _style(style),
    _thread_names_lock(thread_names_lock),
    _thread_names(thread_names),
    _use_bci(use_bci)
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
        // Trim 'L' and ';' off the class descriptor like 'Ljava/lang/Object;'
        result = javaClassName(class_name + 1, strlen(class_name) - 2, _style);
        strcat(result, ".");
        strcat(result, method_name);
        if (_style & STYLE_SIGNATURES) strcat(result, method_sig);
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

    int array_dimension = 0;
    while (*symbol == '[') {
        array_dimension++;
        symbol++;
    }

    if (array_dimension == 0) {
        strncpy(result, symbol, length);
        result[length] = 0;
    } else {
        switch (*symbol) {
            case 'B': strcpy(result, "byte");    break;
            case 'C': strcpy(result, "char");    break;
            case 'I': strcpy(result, "int");     break;
            case 'J': strcpy(result, "long");    break;
            case 'S': strcpy(result, "short");   break;
            case 'Z': strcpy(result, "boolean"); break;
            case 'F': strcpy(result, "float");   break;
            case 'D': strcpy(result, "double");  break;
            default:
                length -= array_dimension + 2;
                strncpy(result, symbol + 1, length);
                result[length] = 0;
        }

        do {
            strcat(result, "[]");
        } while (--array_dimension > 0);
    }

    if (style & STYLE_SIMPLE) {
        for (char* s = result; *s; s++) {
            if (*s == '/') result = s + 1;
        }
    }

    if (style & STYLE_DOTTED) {
        for (char* s = result; *s; s++) {
            if (*s == '/') *s = '.';
        }
    }

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

            return javaFrameName(frame);
        }
    }
}

const char* FrameName::javaMethodNameCached(jmethodID method) {
    JMethodCache::iterator it = _cache.lower_bound(method);
    if (it != _cache.end() && it->first == method) {
        return it->second.c_str();
    }


    const char* newName = javaMethodName(method);
    it = _cache.insert(it, JMethodCache::value_type(method, newName));
    return it->second.c_str();
}

const char *FrameName::javaFrameName(ASGCT_CallFrame &frame) {

    jvmtiEnv* jvmti = VM::jvmti();
    jvmtiError err;

    const char* name = javaMethodNameCached(frame.method_id);
    
    if (_use_bci) {

        char* file_name = NULL;
        int entry_count = 0;
        jvmtiLineNumberEntry* line_number_table = NULL;
        jclass method_class;
        jmethodID method = frame.method_id;


        char* result = _buf;
        strcpy(result, name);

        if ((err = jvmti->GetMethodDeclaringClass(method, &method_class)) == 0 &&
            (err = jvmti->GetSourceFileName(method_class, &file_name)) == 0 &&
            (err = jvmti->GetLineNumberTable(method, &entry_count, &line_number_table)) == 0) {

            int line_number = -1;
            for (int i = entry_count - 1; i >= 0; i--) {
                if (line_number_table[i].start_location <= frame.bci) {
                    line_number = line_number_table[i].line_number;
                    break;
                }
            }


            if (line_number == -1 && file_name[0] == 'I') {
                printf("Fail in %s, %d", file_name, line_number);
            }
            strcat(result, "_$[");
            strcat(result, file_name);
            char ln_buf[32];
            snprintf(ln_buf, sizeof(ln_buf), ":%d]$", line_number);
            strcat(result, ln_buf);
        } else {
            printf("GetSource, err: %d\n", err);
        }
        jvmti->Deallocate((unsigned char*)file_name);
        jvmti->Deallocate((unsigned char*)line_number_table);

        return result;
    } else {
        return name;
    }
}
