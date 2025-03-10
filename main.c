﻿#include <Windows.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>

#include "argparse.h"

static const char* const usages[] = {
        "sshpass [options] command arguments",
        NULL,
};

typedef struct {
    enum { PWT_STDIN, PWT_FILE, PWT_FD, PWT_PASS } pwtype;
    union {
        const char* filename;
        int64_t fd;
        const char* password;
    } pwsrc;

    const char* passPrompt;
    int verbose;

    char* cmd;
} Args;

typedef struct {
    Args args;

    HANDLE pipeIn;
    HANDLE pipeOut;

    HANDLE stdOut;

    HANDLE events[2];
} Context;

static void ParseArgs(int argc, const char* argv[], Context* ctx);
static void WritePass(Context* ctx);
static HRESULT CreatePseudoConsoleAndPipes(HPCON* hpcon, Context* ctx);
static HRESULT InitializeStartupInfoAttachedToPseudoConsole(STARTUPINFOEXA* startupInfo,
                                                            HPCON hpcon);
static void __cdecl PipeListener(LPVOID);
static void __cdecl InputHandlerThread(LPVOID);

int main(int argc, const char* argv[]) {
    Context ctx;

    ParseArgs(argc, argv, &ctx);

    HRESULT hr = E_UNEXPECTED;

    ctx.pipeIn = INVALID_HANDLE_VALUE;
    ctx.pipeOut = INVALID_HANDLE_VALUE;
    ctx.stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    ctx.events[0] = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (ctx.events[0] == NULL) {
        return EXIT_FAILURE;
    }

    DWORD consoleMode = 0;
    GetConsoleMode(ctx.stdOut, &consoleMode);
    SetConsoleMode(ctx.stdOut, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    HPCON hpcon = INVALID_HANDLE_VALUE;

    hr = CreatePseudoConsoleAndPipes(&hpcon, &ctx);
    if (S_OK == hr) {
        HANDLE pipeListener = (HANDLE) _beginthread(PipeListener, 0, &ctx);

        STARTUPINFOEXA startupInfo = {0};
        if (S_OK == InitializeStartupInfoAttachedToPseudoConsole(&startupInfo, hpcon)) {
            PROCESS_INFORMATION cmdProc;
            hr = CreateProcessA(NULL, (char*) ctx.args.cmd, NULL, NULL, FALSE,
                                EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                                &startupInfo.StartupInfo, &cmdProc)
                            ? S_OK
                            : GetLastError();

            if (S_OK == hr) {
                ctx.events[1] = cmdProc.hThread;

                HANDLE inputHandler = (HANDLE) _beginthread(InputHandlerThread, 0, &ctx);

                WaitForMultipleObjects(sizeof(ctx.events) / sizeof(HANDLE), ctx.events, FALSE,
                                        INFINITE);
            }

            CloseHandle(cmdProc.hThread);
            CloseHandle(cmdProc.hProcess);

            DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
            free(startupInfo.lpAttributeList);
        }

        ClosePseudoConsole(hpcon);

        if (INVALID_HANDLE_VALUE != ctx.pipeOut) {
            CloseHandle(ctx.pipeOut);
        }
        if (INVALID_HANDLE_VALUE != ctx.pipeIn) {
            CloseHandle(ctx.pipeIn);
        }

        CloseHandle(ctx.events[0]);
    } 
    return S_OK == hr ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void ParseArgs(int argc, const char* argv[], Context* ctx) {
    const char* filename = NULL;
    int64_t number = 0;
    const char* strpass = NULL;
    int envPass = 0;

    const char* passPrompt = NULL;
    int verbose = 0;

    struct argparse_option options[] = {
            OPT_HELP(),
            OPT_GROUP("Password options: With no options - password will be taken from stdin"),
            OPT_STRING('f', NULL, &filename, "Take password to use from file", NULL, 0, 0),
            OPT_INTEGER('d', NULL, &number, "Use number as file descriptor for getting password",
                        NULL, 0, 0),
            OPT_STRING('p', NULL, &strpass, "Provide password as argument (security unwise)", NULL,
                       0, 0),
            OPT_BOOLEAN('e', NULL, &envPass, "Password is passed as env-var \"SSHPASS\"", NULL, 0,
                        0),
            OPT_GROUP("Other options: "),
            OPT_STRING('P', NULL, &passPrompt,
                       "Which string should sshpass search for to detect a password prompt", NULL,
                       0, 0),
            OPT_BOOLEAN('v', NULL, &verbose, "Be verbose about what you're doing", NULL, 0, 0),
            OPT_END(),
    };

    struct argparse argparse;
    argparse_init(&argparse, options, usages, ARGPARSE_STOP_AT_NON_OPTION);
    argc = argparse_parse(&argparse, argc, argv);
    if (argc == 0) {
        argparse_usage(&argparse);
        exit(EXIT_FAILURE);
    }

    ctx->args.verbose = verbose;
    if (filename != NULL) {
        ctx->args.pwtype = PWT_FILE;
        ctx->args.pwsrc.filename = filename;
    } else if (number != 0) {
        ctx->args.pwtype = PWT_FD;
        ctx->args.pwsrc.fd = number;
    } else if (strpass != NULL) {
        ctx->args.pwtype = PWT_PASS;
        ctx->args.pwsrc.password = strpass;
    } else if (envPass != 0) {
        ctx->args.pwtype = PWT_PASS;
        ctx->args.pwsrc.password = getenv("SSHPASS");
    } else {
        ctx->args.pwtype = PWT_STDIN;
    }

    if (passPrompt != NULL) {
        ctx->args.passPrompt = passPrompt;
    } else {
        ctx->args.passPrompt = "password:";
    }

    int cmdLen = 0;
    for (int i = 0; i < argc; i++) {
        cmdLen += strlen(argv[i]) + 2;
    }

    ctx->args.cmd = malloc(sizeof(char) * cmdLen);
    memset(ctx->args.cmd, 0, sizeof(char) * cmdLen);
    for (int i = 0; i < argc; i++) {
        StringCchCatA(ctx->args.cmd, sizeof(char) * cmdLen, argv[i]);
        StringCchCatA(ctx->args.cmd, sizeof(char) * cmdLen, " ");
    }

    if (ctx->args.verbose) {
        fprintf(stdout, "cmd: %s\n", ctx->args.cmd);
    }
}

static HRESULT CreatePseudoConsoleAndPipes(HPCON* hpcon, Context* ctx) {
    HRESULT hr = E_UNEXPECTED;
    HANDLE pipePtyIn = INVALID_HANDLE_VALUE;
    HANDLE pipePtyOut = INVALID_HANDLE_VALUE;

    if (CreatePipe(&pipePtyIn, &ctx->pipeOut, NULL, 0) &&
        CreatePipe(&ctx->pipeIn, &pipePtyOut, NULL, 0)) {
        COORD consoleSize;

        CONSOLE_SCREEN_BUFFER_INFO csbi;
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        if (GetConsoleScreenBufferInfo(hConsole, &csbi)) {
            consoleSize.X = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            consoleSize.Y = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        } else {
            consoleSize.X = 120;
            consoleSize.Y = 25;
        }
        hr = CreatePseudoConsole(consoleSize, pipePtyIn, pipePtyOut, 0, hpcon);

        if (INVALID_HANDLE_VALUE != pipePtyOut) {
            CloseHandle(pipePtyOut);
        }

        if (INVALID_HANDLE_VALUE != pipePtyIn) {
            CloseHandle(pipePtyIn);
        }
    }
    return hr;
}

static HRESULT InitializeStartupInfoAttachedToPseudoConsole(STARTUPINFOEXA* startupInfo,
                                                            HPCON hpcon) {
    HRESULT hr = E_UNEXPECTED;
    if (startupInfo == NULL) {
        return hr;
    }

    size_t attrListSize;
    startupInfo->StartupInfo.cb = sizeof(STARTUPINFOEXA);

    InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);

    startupInfo->lpAttributeList = malloc(attrListSize);
    if (startupInfo->lpAttributeList == NULL) {
        return hr;
    }

    if (!InitializeProcThreadAttributeList(startupInfo->lpAttributeList, 1, 0, &attrListSize)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    hr = UpdateProcThreadAttribute(startupInfo->lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpcon, sizeof(HPCON), NULL,
                                   NULL)
                 ? S_OK
                 : HRESULT_FROM_WIN32(GetLastError());

    return hr;
}

static BOOL IsWaitInputPass(Context* ctx, const char* buffer, DWORD len) {
    char* pos = strstr(buffer, ctx->args.passPrompt);
    if (pos == NULL) {
        return FALSE;
    }
    return TRUE;
}

typedef enum { INIT, VERIFY, EXEC, END } State;

static State ProcessOutput(Context* ctx, const char* buffer, DWORD len, State state) {
    State nextState;
    switch (state) {
    case INIT: {
        if (!IsWaitInputPass(ctx, buffer, len)) {
            nextState = INIT;
        } else {
            WritePass(ctx);
            nextState = VERIFY;
        }
    } break;
    case VERIFY: {
        if (IsWaitInputPass(ctx, buffer, len)) {
            fprintf(stderr, "Password is error!");
            nextState = END;
        } else {
            fprintf(stdout, "%s", buffer);
            nextState = EXEC;
        }
    } break;
    case EXEC: {
        fprintf(stdout, "%s", buffer);
        nextState = EXEC;
    } break;
    case END: {
        nextState = END;
    } break;
    }
    return nextState;
}

#define BUFFER_SIZE 1024

static void __cdecl PipeListener(LPVOID arg) {
    Context* ctx = arg;

    char buffer[BUFFER_SIZE + 1] = {0};

    DWORD bytesRead;

    BOOL fRead = FALSE;

    State state = INIT;

    while (1) {
        fRead = ReadFile(ctx->pipeIn, buffer, BUFFER_SIZE, &bytesRead, NULL);
        if (!fRead || bytesRead == 0) {
            break;
        }
        buffer[bytesRead] = 0;
        state = ProcessOutput(ctx, buffer, bytesRead, state);
        if (state == END) {
            break;
        }
    }
    SetEvent(ctx->events[0]);
}

static void WritePassHandle(Context* ctx, HANDLE src) {
    int done = 0;

    while (!done) {
        char buffer[40] = {0};
        int i;
        DWORD bytesRead;
        ReadFile(src, buffer, sizeof(buffer), &bytesRead, NULL);
        done = (bytesRead < 1);
        for (i = 0; i < bytesRead && !done; ++i) {
            if (buffer[i] == '\r' || buffer[i] == '\n') {
                done = 1;
                break;
            } else {
                WriteFile(ctx->pipeOut, buffer + i, 1, NULL, NULL);
            }
        }
    }
    WriteFile(ctx->pipeOut, "\n", 1, NULL, NULL);
}

static void WritePass(Context* ctx) {
    switch (ctx->args.pwtype) {
    case PWT_STDIN:
        WritePassHandle(ctx, GetStdHandle(STD_INPUT_HANDLE));
        break;
    case PWT_FD:
        WritePassHandle(ctx, (HANDLE) ctx->args.pwsrc.fd);
        break;
    case PWT_FILE: {
        HANDLE file = CreateFileA(ctx->args.pwsrc.filename, GENERIC_READ, FILE_SHARE_READ, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            WritePassHandle(ctx, file);
            CloseHandle(file);
        }
    } break;
    case PWT_PASS: {
        WriteFile(ctx->pipeOut, ctx->args.pwsrc.password, strlen(ctx->args.pwsrc.password), NULL,
                  NULL);
        WriteFile(ctx->pipeOut, "\n", 1, NULL, NULL);

    } break;
    }
}

static void __cdecl InputHandlerThread(LPVOID arg) {
    Context* ctx = arg;
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;

    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, (mode & ~ENABLE_LINE_INPUT) & ~ENABLE_ECHO_INPUT);

    char buffer;
    DWORD bytesRead, bytesWritten;

    while (1) {
        if (!ReadFile(hStdin, &buffer, 1, &bytesRead, NULL) || bytesRead == 0) {
            break;
        }

        if (!WriteFile(ctx->pipeOut, &buffer, 1, &bytesWritten, NULL)) {
            break;
        }
    }

    SetConsoleMode(hStdin, mode);
}