/*
 * Clawd Tray - an animated Clawd in the Windows tray that mirrors what Claude Code is doing.
 *
 *   ClawdTray.exe            installed copy: runs the tray icon
 *   ClawdTray-Setup.exe      any other copy: asks to install
 *   ClawdTray.exe --hook     called by Claude Code hooks: reads the hook JSON on stdin,
 *                            tells the tray window, exits. Prints nothing (no tokens).
 *   --tray                   force tray mode from any location (testing)
 *   --install / --uninstall  (add --quiet to skip dialogs)
 *
 * Status changes come from Claude Code hooks (local process, no model call), never from polling.
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "frames.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

#define APP_NAME      L"Clawd Tray"
#define WND_CLASS     L"ClawdTrayWnd"
#define MUTEX_NAME    L"Local\\ClawdTray.Singleton"
#define EXE_NAME      L"ClawdTray.exe"
#define RUN_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define UNINST_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ClawdTray"
#define PREFS_KEY     L"Software\\ClawdTray"
#define OV_CLASS      L"ClawdTrayOverlay"
#define APP_VERSION   L"1.0.0"
#define WM_TRAY       (WM_APP + 1)

/* ------------------------------------------------------------------ events (hook -> tray) */
enum { EV_NONE, EV_START, EV_PROMPT, EV_TOOL_DONE, EV_ASK, EV_PERM, EV_BLOCKED, EV_STOP, EV_FAIL, EV_END };
#define FL_AGENT 1
typedef struct { int ev; int flags; char sid[96]; } HookMsg;
#define COPYDATA_MAGIC 0xC1A3D001

/* ================================================================== tiny JSON (raw-preserving) */
enum { J_RAW, J_STR, J_ARR, J_OBJ };
typedef struct J J;
struct J { int t; const char *raw; int rawlen; int n, cap; const char **kraw; int *klen; J **v; };

static const char *jp, *je;
static J *jnew(int t) { J *j = (J *)calloc(1, sizeof(J)); j->t = t; return j; }
static void jpush(J *o, const char *k, int kl, J *v) {
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 8;
        o->kraw = (const char **)realloc((void *)o->kraw, o->cap * sizeof(char *));
        o->klen = (int *)realloc(o->klen, o->cap * sizeof(int));
        o->v = (J **)realloc(o->v, o->cap * sizeof(J *));
    }
    o->kraw[o->n] = k; o->klen[o->n] = kl; o->v[o->n] = v; o->n++;
}
static void jws(void) { while (jp < je && (*jp == ' ' || *jp == '\t' || *jp == '\n' || *jp == '\r')) jp++; }
static int jskipstr(void) { jp++; while (jp < je) { if (*jp == '\\') { jp += 2; continue; } if (*jp == '"') { jp++; return 1; } jp++; } return 0; }
static J *jval(int depth) {
    jws(); if (jp >= je || depth > 200) return NULL;
    if (*jp == '{' || *jp == '[') {
        int obj = (*jp == '{'); char close = obj ? '}' : ']';
        J *o = jnew(obj ? J_OBJ : J_ARR); jp++; jws();
        if (jp < je && *jp == close) { jp++; return o; }
        for (;;) {
            const char *k = NULL; int kl = 0;
            jws();
            if (obj) {
                if (jp >= je || *jp != '"') return NULL;
                k = jp; if (!jskipstr()) return NULL; kl = (int)(jp - k);
                jws(); if (jp >= je || *jp != ':') return NULL; jp++;
            }
            J *v = jval(depth + 1); if (!v) return NULL;
            jpush(o, k, kl, v); jws();
            if (jp < je && *jp == ',') { jp++; continue; }
            if (jp < je && *jp == close) { jp++; return o; }
            return NULL;
        }
    }
    if (*jp == '"') { J *s = jnew(J_STR); s->raw = jp; if (!jskipstr()) return NULL; s->rawlen = (int)(jp - s->raw); return s; }
    { J *s = jnew(J_RAW); s->raw = jp;
      while (jp < je && !strchr(",}] \t\r\n", *jp)) jp++;
      s->rawlen = (int)(jp - s->raw); return s->rawlen ? s : NULL; }
}
static J *jparse(const char *buf, int len) {
    jp = buf; je = buf + len;
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) jp += 3;
    J *r = jval(0); if (!r) return NULL; jws(); return jp == je ? r : NULL;
}
static int jkeyis(const char *raw, int len, const char *key) {
    int kl = (int)strlen(key); return len == kl + 2 && memcmp(raw + 1, key, kl) == 0;
}
static int jfind(J *o, const char *key) {
    if (!o || o->t != J_OBJ) return -1;
    for (int i = 0; i < o->n; i++) if (jkeyis(o->kraw[i], o->klen[i], key)) return i;
    return -1;
}
static J *jget(J *o, const char *key) { int i = jfind(o, key); return i < 0 ? NULL : o->v[i]; }
static int jstreq(J *s, const char *val) { return s && s->t == J_STR && jkeyis(s->raw, s->rawlen, val); }
static void jstrcpy(J *s, char *out, int cap) {
    out[0] = 0; if (!s || s->t != J_STR) return;
    int n = s->rawlen - 2; if (n > cap - 1) n = cap - 1; if (n < 0) n = 0;
    memcpy(out, s->raw + 1, n); out[n] = 0;
}
static void jremove(J *o, int i) {
    for (int k = i; k < o->n - 1; k++) { o->kraw[k] = o->kraw[k + 1]; o->klen[k] = o->klen[k + 1]; o->v[k] = o->v[k + 1]; }
    o->n--;
}
static J *jrawnew(int t, const char *s) { J *j = jnew(t); j->raw = _strdup(s); j->rawlen = (int)strlen(s); return j; }
static void jset(J *o, const char *key, J *v) {
    int i = jfind(o, key);
    if (i >= 0) { o->v[i] = v; return; }
    char *k = (char *)malloc(strlen(key) + 3); sprintf(k, "\"%s\"", key);
    jpush(o, k, (int)strlen(k), v);
}

typedef struct { char *p; int n, cap; } Buf;
static void bput(Buf *b, const char *s, int n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = (char *)realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void bstr(Buf *b, const char *s) { bput(b, s, (int)strlen(s)); }
static void bind(Buf *b, int ind) { bput(b, "\n", 1); for (int i = 0; i < ind; i++) bput(b, " ", 1); }
/* same layout as JSON.stringify(x, null, 2), which is how Claude Code writes settings.json */
static void jwrite(Buf *b, J *j, int ind) {
    if (j->t == J_RAW || j->t == J_STR) { bput(b, j->raw, j->rawlen); return; }
    int obj = j->t == J_OBJ;
    if (!j->n) { bstr(b, obj ? "{}" : "[]"); return; }
    bstr(b, obj ? "{" : "[");
    for (int i = 0; i < j->n; i++) {
        bind(b, ind + 2);
        if (obj) { bput(b, j->kraw[i], j->klen[i]); bstr(b, ": "); }
        jwrite(b, j->v[i], ind + 2);
        if (i < j->n - 1) bstr(b, ",");
    }
    bind(b, ind); bstr(b, obj ? "}" : "]");
}

/* ================================================================== small helpers */
static WCHAR g_self[MAX_PATH];        /* this exe */
static WCHAR g_installDir[MAX_PATH];  /* %LOCALAPPDATA%\Programs\ClawdTray */
static WCHAR g_installExe[MAX_PATH];
static int   g_quiet;

static void init_paths(void) {
    GetModuleFileNameW(NULL, g_self, MAX_PATH);
    WCHAR la[MAX_PATH]; GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    wsprintfW(g_installDir, L"%s\\Programs\\ClawdTray", la);
    wsprintfW(g_installExe, L"%s\\%s", g_installDir, EXE_NAME);
}
static int msg(UINT flags, const WCHAR *text) {
    if (g_quiet) return (flags & MB_YESNO) ? IDYES : IDOK;
    return MessageBoxW(NULL, text, APP_NAME, flags | MB_SETFOREGROUND);
}
static char *read_file(const WCHAR *path, int *len) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), got = 0;
    char *b = (char *)malloc(sz + 1);
    if (!ReadFile(h, b, sz, &got, NULL)) got = 0;
    CloseHandle(h); b[got] = 0; *len = (int)got; return b;
}
static int write_file_atomic(const WCHAR *path, const char *data, int len) {
    WCHAR tmp[MAX_PATH + 16]; wsprintfW(tmp, L"%s.clawdtmp", path);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0; BOOL ok = WriteFile(h, data, len, &w, NULL) && (int)w == len;
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp); return 0; }
    return MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
}
static void settings_path(WCHAR *out) {
    WCHAR dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", dir, MAX_PATH)) {
        WCHAR up[MAX_PATH]; GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
        wsprintfW(dir, L"%s\\.claude", up);
    }
    CreateDirectoryW(dir, NULL);
    wsprintfW(out, L"%s\\settings.json", dir);
}
static const WCHAR *wbasename(const WCHAR *p) { const WCHAR *b = wcsrchr(p, L'\\'); return b ? b + 1 : p; }

/* ================================================================== hook mode */
static int hook_main(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char *buf = NULL; int len = 0, cap = 0;
    if (in && in != INVALID_HANDLE_VALUE && GetFileType(in) != FILE_TYPE_CHAR) {
        for (;;) {
            if (cap - len < 65536) { cap = cap ? cap * 2 : 131072; buf = (char *)realloc(buf, cap + 1); }
            DWORD got = 0;
            if (!ReadFile(in, buf + len, cap - len, &got, NULL) || !got) break;
            len += got;
        }
    }
    if (!buf) return 0;
    J *root = jparse(buf, len);
    if (!root || root->t != J_OBJ) return 0;

    HookMsg m; memset(&m, 0, sizeof m);
    J *evn = jget(root, "hook_event_name");
    J *tool = jget(root, "tool_name");
    J *tin = jget(root, "tool_input");
    jstrcpy(jget(root, "session_id"), m.sid, sizeof m.sid);
    if (jget(root, "agent_id")) m.flags |= FL_AGENT;

    if (jstreq(evn, "SessionStart")) m.ev = EV_START;
    else if (jstreq(evn, "UserPromptSubmit")) m.ev = EV_PROMPT;
    else if (jstreq(evn, "PostToolUse") || jstreq(evn, "PostToolUseFailure")) m.ev = EV_TOOL_DONE;
    else if (jstreq(evn, "PermissionRequest")) m.ev = EV_PERM;
    else if (jstreq(evn, "Stop")) m.ev = EV_STOP;
    else if (jstreq(evn, "StopFailure")) m.ev = EV_FAIL;
    else if (jstreq(evn, "SessionEnd")) m.ev = EV_END;
    else if (jstreq(evn, "Notification")) {
        J *nt = jget(root, "notification_type");
        if (jstreq(nt, "permission_prompt") || jstreq(nt, "elicitation_dialog")) m.ev = EV_ASK;
    } else if (jstreq(evn, "PreToolUse")) {
        if (jstreq(tool, "AskUserQuestion") || jstreq(tool, "ExitPlanMode")) m.ev = EV_ASK;
        else if (jstreq(tool, "mcp__ccd_session__start_session") && jstreq(jget(tin, "initiation"), "offer")) m.ev = EV_ASK;
        else if (jstreq(tool, "mcp__ccd_turn__wrap_up")) {
            J *bl = jget(tin, "blocked");
            if (bl && bl->t == J_RAW && bl->rawlen == 4 && !memcmp(bl->raw, "true", 4)) m.ev = EV_BLOCKED;
        }
    }
    if (!m.ev) return 0;

    HWND w = FindWindowW(WND_CLASS, NULL);
    if (w) {
        COPYDATASTRUCT cds; cds.dwData = COPYDATA_MAGIC; cds.cbData = sizeof m; cds.lpData = &m;
        DWORD_PTR r; SendMessageTimeoutW(w, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &r);
    }
    return 0;
}

/* ================================================================== settings.json hooks */
static const char *HOOK_EVENTS[][2] = {   /* event, matcher (NULL = all) */
    { "SessionStart", NULL },
    { "UserPromptSubmit", NULL },
    { "PreToolUse", "AskUserQuestion|ExitPlanMode|mcp__ccd_turn__wrap_up|mcp__ccd_session__start_session" },
    { "PostToolUse", NULL },
    { "PostToolUseFailure", NULL },
    { "PermissionRequest", NULL },
    { "Notification", "permission_prompt|elicitation_dialog" },
    { "Stop", NULL },
    { "StopFailure", NULL },
    { "SessionEnd", NULL },
};
static int is_our_hook(J *h) {
    J *c = jget(h, "command");
    if (!c || c->t != J_STR) return 0;
    char tmp[1024]; int n = c->rawlen < 1023 ? c->rawlen : 1023;
    memcpy(tmp, c->raw, n); tmp[n] = 0; _strlwr(tmp);
    return strstr(tmp, "clawdtray.exe") != NULL;
}
/* removes our entries; returns number removed */
static int strip_hooks(J *root) {
    J *hooks = jget(root, "hooks"); int removed = 0;
    if (!hooks || hooks->t != J_OBJ) return 0;
    for (int e = hooks->n - 1; e >= 0; e--) {
        J *arr = hooks->v[e]; if (arr->t != J_ARR) continue;
        for (int g = arr->n - 1; g >= 0; g--) {
            J *grp = arr->v[g]; J *hs = jget(grp, "hooks");
            if (!hs || hs->t != J_ARR) continue;
            for (int k = hs->n - 1; k >= 0; k--) if (is_our_hook(hs->v[k])) { jremove(hs, k); removed++; }
            if (!hs->n) jremove(arr, g);
        }
        if (!arr->n) jremove(hooks, e);
    }
    if (!hooks->n) jremove(root, jfind(root, "hooks"));
    return removed;
}
static void add_hooks(J *root, const WCHAR *exe) {
    char exe8[MAX_PATH * 3], esc[MAX_PATH * 6], cmd[MAX_PATH * 6 + 4];
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, exe8, sizeof exe8, NULL, NULL);
    char *o = esc; for (char *s = exe8; *s; s++) { if (*s == '\\' || *s == '"') *o++ = '\\'; *o++ = *s; } *o = 0;
    sprintf(cmd, "\"%s\"", esc);
    J *hooks = jget(root, "hooks");
    if (!hooks || hooks->t != J_OBJ) { hooks = jnew(J_OBJ); jset(root, "hooks", hooks); }
    for (int i = 0; i < (int)(sizeof HOOK_EVENTS / sizeof HOOK_EVENTS[0]); i++) {
        J *arr = jget(hooks, HOOK_EVENTS[i][0]);
        if (!arr || arr->t != J_ARR) { arr = jnew(J_ARR); jset(hooks, HOOK_EVENTS[i][0], arr); }
        J *h = jnew(J_OBJ);
        jset(h, "type", jrawnew(J_STR, "\"command\""));
        jset(h, "command", jrawnew(J_STR, cmd));
        J *args = jnew(J_ARR); jpush(args, NULL, 0, jrawnew(J_STR, "\"--hook\"")); jset(h, "args", args);
        jset(h, "timeout", jrawnew(J_RAW, "5"));
        J *hs = jnew(J_ARR); jpush(hs, NULL, 0, h);
        J *grp = jnew(J_OBJ);
        if (HOOK_EVENTS[i][1]) { char m[256]; sprintf(m, "\"%s\"", HOOK_EVENTS[i][1]); jset(grp, "matcher", jrawnew(J_STR, m)); }
        jset(grp, "hooks", hs);
        jpush(arr, NULL, 0, grp);
    }
}
/* install=1 adds (replacing old copies), install=0 removes. Returns 1 ok, 0 error (msg filled). */
static int update_settings(int install, WCHAR *err, int errcap) {
    WCHAR path[MAX_PATH]; settings_path(path);
    int len = 0; char *txt = read_file(path, &len);
    J *root;
    if (!txt || !len) {
        if (!install) return 1;
        root = jnew(J_OBJ);
    } else {
        root = jparse(txt, len);
        if (!root || root->t != J_OBJ) {
            _snwprintf(err, errcap, L"Could not read %s (it is not plain JSON), so the hooks were not changed.", path);
            return 0;
        }
        WCHAR bak[MAX_PATH + 32]; wsprintfW(bak, L"%s.clawdtray.bak", path);
        CopyFileW(path, bak, TRUE);   /* keeps the very first backup */
    }
    int removed = strip_hooks(root);
    if (install) add_hooks(root, g_installExe);
    else if (!removed) return 1;
    Buf b = { 0 }; jwrite(&b, root, 0); bstr(&b, "\n");
    if (!write_file_atomic(path, b.p, b.n)) {
        _snwprintf(err, errcap, L"Could not write %s.", path);
        return 0;
    }
    return 1;
}

/* ================================================================== registry */
static void reg_set_str(HKEY root, const WCHAR *key, const WCHAR *name, const WCHAR *val) {
    HKEY k; if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (DWORD)((wcslen(val) + 1) * 2)); RegCloseKey(k);
}
static void reg_set_dw(HKEY root, const WCHAR *key, const WCHAR *name, DWORD v) {
    HKEY k; if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, 4); RegCloseKey(k);
}
static DWORD pref_get(const WCHAR *name, DWORD def) {
    HKEY k; DWORD v = def, sz = 4, type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, PREFS_KEY, 0, KEY_QUERY_VALUE, &k)) return def;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)&v, &sz) || type != REG_DWORD) v = def;
    RegCloseKey(k); return v;
}
static void pref_set(const WCHAR *name, DWORD v) { reg_set_dw(HKEY_CURRENT_USER, PREFS_KEY, name, v); }
static int autostart_enabled(void) {
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k)) return 0;
    LONG r = RegQueryValueExW(k, L"ClawdTray", NULL, NULL, NULL, NULL); RegCloseKey(k); return r == 0;
}
static void set_autostart(int on) {
    if (on) { WCHAR v[MAX_PATH + 4]; wsprintfW(v, L"\"%s\"", g_installExe); reg_set_str(HKEY_CURRENT_USER, RUN_KEY, L"ClawdTray", v); }
    else { HKEY k; if (!RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k)) { RegDeleteValueW(k, L"ClawdTray"); RegCloseKey(k); } }
}

/* ================================================================== install / uninstall */
static void close_running_tray(void) {
    HWND w = FindWindowW(WND_CLASS, NULL); if (!w) return;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(w, WM_CLOSE, 0, 0);
    if (p) { WaitForSingleObject(p, 4000); CloseHandle(p); }
}
static void launch(const WCHAR *exe, const WCHAR *args) {
    ShellExecuteW(NULL, L"open", exe, args, NULL, SW_SHOWNORMAL);
}
static int do_install(void) {
    if (!g_quiet && msg(MB_YESNO | MB_ICONQUESTION,
        L"Install Clawd Tray?\n\n"
        L"\x2022 Copies itself to %LOCALAPPDATA%\\Programs\\ClawdTray\n"
        L"\x2022 Adds status hooks to your Claude Code settings (~/.claude/settings.json, a backup is kept)\n"
        L"\x2022 Starts with Windows (you can turn it off from the tray menu)\n\n"
        L"No tokens are used: the hooks only run this small program on your PC.") != IDYES) return 1;
    close_running_tray();
    CreateDirectoryW(g_installDir, NULL);
    if (lstrcmpiW(g_self, g_installExe) && !CopyFileW(g_self, g_installExe, FALSE)) {
        msg(MB_ICONERROR, L"Could not copy the program to %LOCALAPPDATA%\\Programs\\ClawdTray."); return 2;
    }
    WCHAR err[600] = L""; int hooksOk = update_settings(1, err, 600);
    set_autostart(1);
    WCHAR v[MAX_PATH + 32];
    reg_set_str(HKEY_CURRENT_USER, UNINST_KEY, L"DisplayName", APP_NAME);
    reg_set_str(HKEY_CURRENT_USER, UNINST_KEY, L"DisplayVersion", APP_VERSION);
    reg_set_str(HKEY_CURRENT_USER, UNINST_KEY, L"Publisher", L"Clawd Tray");
    reg_set_str(HKEY_CURRENT_USER, UNINST_KEY, L"InstallLocation", g_installDir);
    wsprintfW(v, L"%s,0", g_installExe); reg_set_str(HKEY_CURRENT_USER, UNINST_KEY, L"DisplayIcon", v);
    wsprintfW(v, L"\"%s\" --uninstall", g_installExe); reg_set_str(HKEY_CURRENT_USER, UNINST_KEY, L"UninstallString", v);
    reg_set_dw(HKEY_CURRENT_USER, UNINST_KEY, L"NoModify", 1);
    reg_set_dw(HKEY_CURRENT_USER, UNINST_KEY, L"NoRepair", 1);
    reg_set_dw(HKEY_CURRENT_USER, UNINST_KEY, L"EstimatedSize", 200);
    launch(g_installExe, NULL);
    if (!hooksOk) { WCHAR t[900]; _snwprintf(t, 900, L"Clawd is running, but the hooks were not added:\n\n%s", err); msg(MB_ICONWARNING, t); return 3; }
    if (!g_quiet && msg(MB_YESNO | MB_ICONINFORMATION,
        L"Done! Clawd is in your tray.\n\n"
        L"New Claude Code sessions will drive it (sessions already open keep their old hooks until restarted).\n\n"
        L"Windows 11 may tuck new tray icons under the ^ arrow. Open the taskbar settings now? "
        L"(Other system tray icons: turn Clawd Tray on, and Claude off if you want it replaced.)") == IDYES)
        launch(L"ms-settings:taskbar", NULL);
    return 0;
}
static void schedule_self_delete(void) {
    WCHAR cmd[MAX_PATH * 3], sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    wsprintfW(cmd, L"\"%s\\cmd.exe\" /c ping -n 3 127.0.0.1 >nul & del /f /q \"%s\" & rmdir \"%s\"", sys, g_installExe, g_installDir);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}
static int do_uninstall(int fromTray) {
    if (msg(MB_YESNO | MB_ICONQUESTION, L"Uninstall Clawd Tray?\n\nThis removes its hooks from your Claude Code settings, the autostart entry and the program.") != IDYES) return 1;
    if (!fromTray) close_running_tray();
    WCHAR err[600] = L""; int ok = update_settings(0, err, 600);
    set_autostart(0);
    RegDeleteKeyW(HKEY_CURRENT_USER, UNINST_KEY);
    RegDeleteKeyW(HKEY_CURRENT_USER, PREFS_KEY);
    schedule_self_delete();
    if (!ok) { WCHAR t[900]; _snwprintf(t, 900, L"Uninstalled, but the hooks could not be removed:\n\n%s", err); msg(MB_ICONWARNING, t); }
    else msg(MB_ICONINFORMATION, L"Clawd Tray was uninstalled.");
    return 0;
}

/* ================================================================== tray: sessions & status */
enum { S_IDLE, S_DONE, S_ERR, S_WORK, S_ASK };
typedef struct { char sid[96]; int st; DWORD last; int blocked; DWORD permAt; } Sess;
#define MAXS 64
static Sess g_s[MAXS]; static int g_ns;
static int g_preview = -1;         /* menu preview override, -1 = live */

#define STALE_MS   (20 * 60 * 1000)  /* "working" with no hook event for 20 min -> idle (e.g. interrupted turn) */
#define PERM_DELAY 2500              /* PermissionRequest shows "?" only if not resolved quickly */

static Sess *sess_get(const char *sid, int create) {
    for (int i = 0; i < g_ns; i++) if (!strcmp(g_s[i].sid, sid)) return &g_s[i];
    if (!create) return NULL;
    if (g_ns == MAXS) {   /* recycle the quietest session */
        int o = 0; for (int i = 1; i < g_ns; i++) if (g_s[i].last < g_s[o].last) o = i;
        g_s[o] = g_s[--g_ns];
    }
    Sess *s = &g_s[g_ns++]; memset(s, 0, sizeof *s); strncpy(s->sid, sid, sizeof s->sid - 1); return s;
}
static int aggregate(void) {
    int best = S_IDLE;
    static const int rank[] = { 0, 1, 2, 3, 4 };
    for (int i = 0; i < g_ns; i++) if (rank[g_s[i].st] > rank[best]) best = g_s[i].st;
    return best;
}

/* ================================================================== tray: icons & animation */
static HWND g_wnd; static HICON g_icons[NFRAMES]; static int g_iconSize = 16;
static NOTIFYICONDATAW g_nid; static int g_shown = -1; static UINT g_taskbarCreated;
static WCHAR g_tip[128];

static HICON make_icon(int f, int S) {
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = S; bi.bmiHeader.biHeight = -S;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL; HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    unsigned int *px = (unsigned int *)bits;
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        int sx = x * 16 / S, sy = y * 16 / S;
        unsigned char b = FRAMES[f][sy * 8 + (sx >> 1)];
        int idx = (sx & 1) ? (b & 15) : (b >> 4);
        unsigned int c = PALETTE[idx];   /* ARGB, straight alpha (fully opaque or clear) */
        px[y * S + x] = c;
    }
    void *mbits = calloc(1, ((S + 15) / 16) * 2 * S);   /* all-zero mask: the alpha channel does the work */
    HBITMAP mask = CreateBitmap(S, S, 1, 1, mbits); free(mbits);
    ICONINFO ii; ii.fIcon = TRUE; ii.xHotspot = ii.yHotspot = 0; ii.hbmMask = mask; ii.hbmColor = color;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return ic;
}
static void build_icons(void) {
    g_iconSize = GetSystemMetrics(SM_CXSMICON); if (g_iconSize < 16) g_iconSize = 16;
    for (int i = 0; i < NFRAMES; i++) { if (g_icons[i]) DestroyIcon(g_icons[i]); g_icons[i] = make_icon(i, g_iconSize); }
}
static int g_trayAdded;
static void tray_add(void) {
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid; g_nid.hWnd = g_wnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icons[g_shown < 0 ? F_FRONT : g_shown];
    lstrcpynW(g_nid.szTip, g_tip[0] ? g_tip : L"Clawd", 128);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) ? 1 : 0;
}
static void tray_remove(void) { if (g_trayAdded) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_trayAdded = 0; } }

/* ------------------------------------------------------------------ big Clawd on the taskbar
 * Tray icons are capped at the small-icon size (16 px at 100%). For a bigger Clawd we add a tiny
 * per-pixel-alpha child window to the taskbar, just left of the notification area. Being a child of
 * the taskbar it hides with it (auto-hide, full-screen apps) and needs no z-order fights.          */
static HWND g_ov, g_tt; static int g_ovScale = 2, g_ovPx, g_trayPref;
static HDC g_ovDC; static HBITMAP g_ovBmp, g_ovOld; static unsigned int *g_ovBits;
static RECT g_ovRect;
static void ack(void); static void open_claude(void); static void show_menu(void);
#define TIMER_OV 5
#define TIMER_TRIM 6

static int screen_dpi(void) { HDC dc = GetDC(NULL); int d = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc); return d ? d : 96; }
static void ov_draw(void) {
    if (!g_ov || !g_ovBits) return;
    int S = g_ovPx, f = g_shown < 0 ? F_FRONT : g_shown;
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        int sx = x * 16 / S, sy = y * 16 / S;
        unsigned char b = FRAMES[f][sy * 8 + (sx >> 1)];
        g_ovBits[y * S + x] = PALETTE[(sx & 1) ? (b & 15) : (b >> 4)];   /* opaque or fully clear: already premultiplied */
    }
    POINT src = { 0, 0 }; SIZE sz = { S, S };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_ov, NULL, NULL, &sz, g_ovDC, &src, 0, &bf, ULW_ALPHA);
}
/* keeps the window parented to the current taskbar and tucked left of the tray; returns 1 if shown */
static int ov_place(void) {
    if (!g_ov) return 0;
    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!tb || GetParent(g_ov) != tb) return 0;
    RECT tr; GetWindowRect(tb, &tr);
    int th = tr.bottom - tr.top, tw = tr.right - tr.left, dpi = screen_dpi();
    if (tw < th || th < g_ovPx) { ShowWindow(g_ov, SW_HIDE); return 0; }   /* vertical or too-thin taskbar */
    int right = tw - 360 * dpi / 96;
    HWND tn = FindWindowExW(tb, NULL, L"TrayNotifyWnd", NULL); RECT nr;
    if (tn && GetWindowRect(tn, &nr) && nr.left > tr.left + tw / 2) right = nr.left - tr.left;
    RECT r; r.left = right - g_ovPx - 4 * dpi / 96; r.top = (th - g_ovPx) / 2 - g_ovPx / 16;
    if (r.top < 0) r.top = 0;
    r.right = r.left + g_ovPx; r.bottom = r.top + g_ovPx;
    if (memcmp(&r, &g_ovRect, sizeof r) || !IsWindowVisible(g_ov)) {
        g_ovRect = r;
        SetWindowPos(g_ov, HWND_TOP, r.left, r.top, g_ovPx, g_ovPx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    return 1;
}
static void ov_destroy(void) {
    if (g_tt) { DestroyWindow(g_tt); g_tt = NULL; }
    if (g_ov) { HWND w = g_ov; g_ov = NULL; DestroyWindow(w); }
    if (g_ovDC) { SelectObject(g_ovDC, g_ovOld); DeleteObject(g_ovBmp); DeleteDC(g_ovDC); g_ovDC = NULL; g_ovBits = NULL; }
    memset(&g_ovRect, 0, sizeof g_ovRect);
}
static void ov_tooltip(void) {
    typedef BOOL (WINAPI *ICCX)(const INITCOMMONCONTROLSEX *);
    HMODULE cc = LoadLibraryW(L"comctl32.dll");   /* loaded here only, so the --hook path stays lean */
    ICCX icc = cc ? (ICCX)GetProcAddress(cc, "InitCommonControlsEx") : NULL;
    if (!icc) return;
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_WIN95_CLASSES }; icc(&ic);
    g_tt = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                           CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_tt) return;
    TTTOOLINFOW ti; memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti; ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS; ti.hwnd = g_ov; ti.uId = (UINT_PTR)g_ov; ti.lpszText = g_tip;
    SendMessageW(g_tt, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}
static void sync_tray_icon(void) {   /* small tray icon: when the big one is off/failed, or if asked to keep it */
    int want = !g_ov || !IsWindowVisible(g_ov) || g_trayPref;
    if (want && !g_trayAdded) tray_add(); else if (!want) tray_remove();
}
static void ov_create(void) {
    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);
    int px = 16 * g_ovScale * screen_dpi() / 96;
    if (g_ov && (!IsWindow(g_ov) || GetParent(g_ov) != tb || px != g_ovPx)) ov_destroy();
    if (!g_ovScale || !tb) { ov_destroy(); sync_tray_icon(); return; }
    if (!g_ov) {
        ov_destroy();   /* frees a stale bitmap/tooltip left by a taskbar restart */
        g_ovPx = px;
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = px; bi.bmiHeader.biHeight = -px;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        HDC sdc = GetDC(NULL); g_ovDC = CreateCompatibleDC(sdc);
        g_ovBmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&g_ovBits, NULL, 0);
        ReleaseDC(NULL, sdc); g_ovOld = (HBITMAP)SelectObject(g_ovDC, g_ovBmp);
        g_ov = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, OV_CLASS, L"Clawd",
                               WS_CHILD | WS_CLIPSIBLINGS, 0, 0, px, px, tb, NULL, GetModuleHandleW(NULL), NULL);
        if (g_ov) { ov_draw(); ov_tooltip(); }
    }
    if (g_ov && !ov_place()) { /* keep the window; the timer retries placement */ }
    sync_tray_icon();
}
static LRESULT CALLBACK ov_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONUP: ack(); open_claude(); SetTimer(g_wnd, TIMER_TRIM, 5000, NULL); return 0;
    case WM_RBUTTONUP: show_menu(); return 0;
    case WM_DESTROY: if (w == g_ov) { g_ov = NULL; if (g_wnd) SetTimer(g_wnd, TIMER_OV, 1000, NULL); } return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static void show_frame(int f) {
    if (f == g_shown) return;
    g_shown = f;
    if (g_trayAdded) { g_nid.uFlags = NIF_ICON; g_nid.hIcon = g_icons[f]; Shell_NotifyIconW(NIM_MODIFY, &g_nid); }
    if (g_ov) ov_draw();
}
static void set_tip(const WCHAR *t) {
    if (!wcscmp(t, g_tip)) return;
    lstrcpynW(g_tip, t, 128); lstrcpynW(g_nid.szTip, t, 128);
    if (g_trayAdded) { g_nid.uFlags = NIF_TIP; Shell_NotifyIconW(NIM_MODIFY, &g_nid); }
    if (g_tt && g_ov) {
        TTTOOLINFOW ti; memset(&ti, 0, sizeof ti);
        ti.cbSize = sizeof ti; ti.hwnd = g_ov; ti.uId = (UINT_PTR)g_ov; ti.lpszText = g_tip;
        SendMessageW(g_tt, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
}

/* visual modes */
enum { V_IDLE, V_DONE, V_ERR, V_WORK, V_ASK };
static int g_target = V_IDLE;        /* what we want to show */
static int g_laptopOut = 0;          /* is the laptop currently out */
static const unsigned short (*g_seq)[2]; static int g_seqLen, g_seqPos, g_seqIsTakeOut;  /* running transition */
static int g_loopPos, g_loopMode = -1;
#define TIMER_ANIM 1
#define TIMER_HOUSE 2
#define TIMER_PERM 3
#define TIMER_ACK 4
#define NSEQ(s) ((int)(sizeof(s) / sizeof(s[0])))

static void arm(UINT ms) { SetTimer(g_wnd, TIMER_ANIM, ms, NULL); }
static UINT rnd(UINT a, UINT b) { return a + (UINT)(rand() % (int)(b - a + 1)); }

static void loop_step(void) {
    /* static poses blink now and then; ASK bobs its "?"; WORK types */
    int m = g_target;
    if (g_loopMode != m) { g_loopMode = m; g_loopPos = 0; }
    int p = g_loopPos++;
    switch (m) {
    case V_WORK:
        show_frame(SEQ_TYPE[p % NSEQ(SEQ_TYPE)][0]); arm(SEQ_TYPE[p % NSEQ(SEQ_TYPE)][1]); break;
    case V_ASK: {
        int ph = p % 12;   /* bob 5 times, then a blink */
        if (ph == 10) { show_frame(F_ASK_BLINK); arm(130); }
        else if (ph == 11) { show_frame(F_ASK); arm(600); }
        else { show_frame((ph & 1) ? F_ASK_UP : F_ASK); arm(600); }
        break; }
    default: {
        int base = m == V_DONE ? F_DONE : m == V_ERR ? F_ERR : F_FRONT;
        int blink = m == V_DONE ? F_DONE_BLINK : m == V_ERR ? F_ERR_BLINK : F_BLINK;
        if (m == V_DONE) { if (p == 0) { show_frame(F_DONE_POP); arm(140); break; } p--; }   /* little "ta-da" pop */
        if (p & 1) { show_frame(blink); arm(120); }                         /* eyes closed */
        else { show_frame(base); arm(p == 0 || p == 2 ? rnd(1500, 3000) : (rand() % 5 ? rnd(2500, 6500) : 180)); } /* sometimes a double blink */
        break; }
    }
}
static void plan(void) {
    if (g_seq) return;                               /* let the running transition finish */
    int needLaptop = (g_target == V_WORK);
    if (needLaptop != g_laptopOut) {
        g_seqIsTakeOut = needLaptop;
        g_seq = needLaptop ? SEQ_TAKE_OUT : SEQ_PUT_AWAY;
        g_seqLen = needLaptop ? NSEQ(SEQ_TAKE_OUT) : NSEQ(SEQ_PUT_AWAY);
        g_seqPos = 0; g_loopMode = -1;
        KillTimer(g_wnd, TIMER_ANIM); SendMessageW(g_wnd, WM_TIMER, TIMER_ANIM, 0);
        return;
    }
    if (g_loopMode != g_target) { KillTimer(g_wnd, TIMER_ANIM); loop_step(); }
}
static void anim_tick(void) {
    if (g_seq) {
        if (g_seqPos < g_seqLen) { show_frame(g_seq[g_seqPos][0]); arm(g_seq[g_seqPos][1]); g_seqPos++; return; }
        g_laptopOut = g_seqIsTakeOut; g_seq = NULL; g_loopMode = -1;
        plan();
        return;
    }
    loop_step();
}

/* ================================================================== tray: status -> visuals */
static int is_claude_window(HWND w) {
    if (!w) return 0;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return 0;
    WCHAR path[MAX_PATH]; DWORD n = MAX_PATH; int yes = 0;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) yes = !lstrcmpiW(wbasename(path), L"claude.exe");
    CloseHandle(p); return yes;
}
static void refresh(void) {
    int agg = g_preview >= 0 ? g_preview : aggregate();
    int nw = 0, na = 0, nd = 0, ne = 0;
    for (int i = 0; i < g_ns; i++) { int s = g_s[i].st; nw += s == S_WORK; na += s == S_ASK; nd += s == S_DONE; ne += s == S_ERR; }
    WCHAR t[128];
    if (g_preview >= 0) wsprintfW(t, L"Clawd \x2014 preview (right-click \x2192 Back to live)");
    else if (!nw && !na && !nd && !ne) wsprintfW(t, L"Clawd \x2014 idle");
    else {
        WCHAR *o = t; o += wsprintfW(o, L"Clawd \x2014 ");
        if (na) o += wsprintfW(o, L"%d need%s you, ", na, na == 1 ? L"s" : L"");
        if (nw) o += wsprintfW(o, L"%d working, ", nw);
        if (ne) o += wsprintfW(o, L"%d error, ", ne);
        if (nd) o += wsprintfW(o, L"%d done, ", nd);
        o[-2] = 0;
    }
    set_tip(t);
    int prevTarget = g_target;
    g_target = agg == S_WORK ? V_WORK : agg == S_ASK ? V_ASK : agg == S_DONE ? V_DONE : agg == S_ERR ? V_ERR : V_IDLE;
    if (g_target != prevTarget) {
        /* finished while you're already looking at Claude: show the check briefly, then relax */
        if ((g_target == V_DONE) && g_preview < 0 && is_claude_window(GetForegroundWindow())) SetTimer(g_wnd, TIMER_ACK, 9000, NULL);
        plan();
    }
    int needHouse = 0; for (int i = 0; i < g_ns; i++) if (g_s[i].st == S_WORK) needHouse = 1;
    if (needHouse) SetTimer(g_wnd, TIMER_HOUSE, 60000, NULL); else KillTimer(g_wnd, TIMER_HOUSE);
}
static void ack(void) {   /* you've seen it: clear checks / errors */
    for (int i = 0; i < g_ns; i++) if (g_s[i].st == S_DONE || g_s[i].st == S_ERR) g_s[i].st = S_IDLE;
    KillTimer(g_wnd, TIMER_ACK); refresh();
}
static void on_hook(const HookMsg *m) {
    g_preview = -1;
    Sess *s = sess_get(m->sid, m->ev != EV_END);
    if (!s) { refresh(); return; }
    DWORD now = GetTickCount(); s->last = now;
    switch (m->ev) {
    case EV_START: break;
    case EV_PROMPT: s->st = S_WORK; s->blocked = 0; s->permAt = 0; break;
    case EV_TOOL_DONE:
        s->permAt = 0;
        if (s->st == S_ASK) s->st = S_WORK;
        else if (s->st != S_WORK && !(m->flags & FL_AGENT)) s->st = S_WORK;
        break;
    case EV_ASK: s->st = S_ASK; s->permAt = 0; break;
    case EV_PERM:
        if (s->st != S_ASK) { s->st = S_WORK; s->permAt = now ? now : 1; SetTimer(g_wnd, TIMER_PERM, PERM_DELAY + 50, NULL); }
        break;
    case EV_BLOCKED: s->blocked = 1; break;
    case EV_STOP: s->st = s->blocked ? S_ASK : S_DONE; s->blocked = 0; s->permAt = 0; break;
    case EV_FAIL: s->st = S_ERR; s->blocked = 0; s->permAt = 0; break;
    case EV_END: *s = g_s[--g_ns]; break;
    }
    refresh();
}
static void house(void) {
    DWORD now = GetTickCount();
    for (int i = 0; i < g_ns; i++) if (g_s[i].st == S_WORK && now - g_s[i].last > STALE_MS) g_s[i].st = S_IDLE;
    refresh();
}
static void perm_check(void) {
    DWORD now = GetTickCount(); int pending = 0;
    for (int i = 0; i < g_ns; i++) if (g_s[i].permAt) {
        if (now - g_s[i].permAt >= PERM_DELAY) { g_s[i].st = S_ASK; g_s[i].permAt = 0; } else pending = 1;
    }
    if (!pending) KillTimer(g_wnd, TIMER_PERM);
    refresh();
}

/* ================================================================== tray: open Claude */
typedef struct { HWND best; int bestScore; } FindCtx;
static BOOL CALLBACK find_claude(HWND w, LPARAM lp) {
    FindCtx *c = (FindCtx *)lp;
    if (GetWindow(w, GW_OWNER)) return TRUE;
    WCHAR cls[64]; GetClassNameW(w, cls, 64);
    if (wcscmp(cls, L"Chrome_WidgetWin_1")) return TRUE;
    if (!is_claude_window(w)) return TRUE;
    WCHAR title[64]; GetWindowTextW(w, title, 64);
    int score = (IsWindowVisible(w) ? 2 : 0) + (!wcscmp(title, L"Claude") ? 1 : 0) + (title[0] ? 1 : 0);
    if (score > c->bestScore) { c->bestScore = score; c->best = w; }
    return TRUE;
}
static void open_claude(void) {
    FindCtx c = { NULL, 0 }; EnumWindows(find_claude, (LPARAM)&c);
    if (c.best && c.bestScore >= 2) {
        if (IsIconic(c.best)) ShowWindow(c.best, SW_RESTORE); else ShowWindow(c.best, SW_SHOW);
        SetForegroundWindow(c.best);
    } else {
        launch(L"claude://", NULL);   /* the desktop app's own link handler brings it up */
    }
}

/* ================================================================== tray: menu & window */
enum { ID_OPEN = 100, ID_RESET, ID_AUTOSTART, ID_TASKBAR, ID_UNINSTALL, ID_EXIT,
       ID_PV_IDLE, ID_PV_WORK, ID_PV_DONE, ID_PV_ASK, ID_PV_ERR, ID_PV_LIVE,
       ID_SZ_OFF, ID_SZ_2, ID_SZ_3, ID_KEEP_TRAY };

static void set_scale(int s) {
    g_ovScale = s; pref_set(L"TaskbarScale", (DWORD)s);
    if (s) SetTimer(g_wnd, TIMER_OV, 4000, NULL); else KillTimer(g_wnd, TIMER_OV);
    ov_create();
}
static void show_menu(void) {
    HMENU m = CreatePopupMenu(), pv = CreatePopupMenu(), sz = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, g_tip);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Open Claude");
    AppendMenuW(pv, MF_STRING, ID_PV_IDLE, L"Idle (blinking)");
    AppendMenuW(pv, MF_STRING, ID_PV_WORK, L"Working (laptop)");
    AppendMenuW(pv, MF_STRING, ID_PV_DONE, L"Done (check)");
    AppendMenuW(pv, MF_STRING, ID_PV_ASK, L"Needs your input (?)");
    AppendMenuW(pv, MF_STRING, ID_PV_ERR, L"Error (!)");
    AppendMenuW(pv, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pv, MF_STRING | (g_preview < 0 ? MF_CHECKED : 0), ID_PV_LIVE, L"Back to live status");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pv, L"Preview animations");
    AppendMenuW(sz, MF_STRING | (g_ovScale == 2 ? MF_CHECKED : 0), ID_SZ_2, L"2\x00D7 on the taskbar");
    AppendMenuW(sz, MF_STRING | (g_ovScale == 3 ? MF_CHECKED : 0), ID_SZ_3, L"3\x00D7 on the taskbar");
    AppendMenuW(sz, MF_STRING | (g_ovScale == 0 ? MF_CHECKED : 0), ID_SZ_OFF, L"Small (tray icon only)");
    AppendMenuW(sz, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sz, MF_STRING | (g_trayPref ? MF_CHECKED : 0) | (g_ovScale ? 0 : MF_GRAYED), ID_KEEP_TRAY, L"Also keep the small tray icon");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sz, L"Size");
    AppendMenuW(m, MF_STRING, ID_RESET, L"Reset status");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING, ID_TASKBAR, L"Show/hide tray icons\x2026");
    AppendMenuW(m, MF_STRING, ID_UNINSTALL, L"Uninstall\x2026");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_wnd, NULL);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    SetTimer(g_wnd, TIMER_TRIM, 5000, NULL);
    switch (cmd) {
    case ID_SZ_OFF: set_scale(0); break;
    case ID_SZ_2: set_scale(2); break;
    case ID_SZ_3: set_scale(3); break;
    case ID_KEEP_TRAY: g_trayPref = !g_trayPref; pref_set(L"TrayIcon", (DWORD)g_trayPref); sync_tray_icon(); break;
    case ID_OPEN: ack(); open_claude(); break;
    case ID_RESET: g_preview = -1; for (int i = 0; i < g_ns; i++) { g_s[i].st = S_IDLE; g_s[i].permAt = 0; g_s[i].blocked = 0; } refresh(); break;
    case ID_AUTOSTART: set_autostart(!autostart_enabled()); break;
    case ID_TASKBAR: launch(L"ms-settings:taskbar", NULL); break;
    case ID_UNINSTALL: if (do_uninstall(1) == 0) DestroyWindow(g_wnd); break;
    case ID_EXIT: DestroyWindow(g_wnd); break;
    case ID_PV_IDLE: g_preview = S_IDLE; refresh(); break;
    case ID_PV_WORK: g_preview = S_WORK; refresh(); break;
    case ID_PV_DONE: g_preview = S_DONE; refresh(); break;
    case ID_PV_ASK: g_preview = S_ASK; refresh(); break;
    case ID_PV_ERR: g_preview = S_ERR; refresh(); break;
    case ID_PV_LIVE: g_preview = -1; refresh(); break;
    }
}
static void CALLBACK on_foreground(HWINEVENTHOOK h, DWORD ev, HWND w, LONG o, LONG c, DWORD t, DWORD tm) {
    (void)h; (void)ev; (void)o; (void)c; (void)t; (void)tm;
    if (g_preview < 0 && (g_target == V_DONE || g_target == V_ERR) && is_claude_window(w)) ack();
}
static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COPYDATA: {
        COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
        if (cd && cd->dwData == COPYDATA_MAGIC && cd->cbData == sizeof(HookMsg)) {
            HookMsg hm; memcpy(&hm, cd->lpData, sizeof hm); hm.sid[sizeof hm.sid - 1] = 0;
            on_hook(&hm);
        }
        return TRUE; }
    case WM_TIMER:
        if (wp == TIMER_ANIM) { KillTimer(w, TIMER_ANIM); anim_tick(); }
        else if (wp == TIMER_HOUSE) house();
        else if (wp == TIMER_PERM) perm_check();
        else if (wp == TIMER_ACK) { KillTimer(w, TIMER_ACK); ack(); }
        else if (wp == TIMER_OV) { if (g_ov) ov_place(); else ov_create(); sync_tray_icon(); }
        else if (wp == TIMER_TRIM) { KillTimer(w, TIMER_TRIM); SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) { ack(); open_claude(); SetTimer(w, TIMER_TRIM, 5000, NULL); }
        else if (lp == WM_RBUTTONUP) show_menu();
        return 0;
    case WM_APP + 2:   /* status probe for tests: frame | target<<8 | laptop<<12 | sessions<<16 | big<<24 | tray<<25 */
        return (LRESULT)((g_shown & 0xFF) | (g_target << 8) | (g_laptopOut << 12) | (g_ns << 16) |
                         ((g_ov && IsWindowVisible(g_ov)) << 24) | (g_trayAdded << 25));
    case WM_APP + 3:   /* another copy was started: make sure we're visible */
        ov_create(); sync_tray_icon(); return 0;
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE:
        if (GetSystemMetrics(SM_CXSMICON) != g_iconSize) { build_icons(); int f = g_shown; g_shown = -1; show_frame(f < 0 ? F_FRONT : f); }
        if (g_ovScale) { ov_create(); if (g_ov) ov_place(); }
        return 0;
    case WM_DESTROY:
        KillTimer(w, TIMER_OV); ov_destroy(); tray_remove(); PostQuitMessage(0); return 0;
    }
    if (m == g_taskbarCreated && m) { g_trayAdded = 0; ov_create(); sync_tray_icon(); return 0; }   /* Explorer restarted */
    return DefWindowProcW(w, m, wp, lp);
}
static int tray_main(void) {
    HANDLE mx = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(WND_CLASS, NULL); if (other) PostMessageW(other, WM_APP + 3, 0, 0);
        return 0;
    }
    srand(GetTickCount());
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = WND_CLASS;
    RegisterClassW(&wc);
    WNDCLASSW oc; memset(&oc, 0, sizeof oc);
    oc.lpfnWndProc = ov_proc; oc.hInstance = wc.hInstance; oc.lpszClassName = OV_CLASS; oc.hCursor = LoadCursorW(NULL, IDC_HAND);
    RegisterClassW(&oc);
    g_ovScale = (int)pref_get(L"TaskbarScale", 2); if (g_ovScale != 0 && g_ovScale != 3) g_ovScale = 2;
    g_trayPref = pref_get(L"TrayIcon", 0) ? 1 : 0;
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, WND_CLASS, L"Clawd Tray", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    ChangeWindowMessageFilterEx(g_wnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    build_icons();
    lstrcpyW(g_tip, L"Clawd \x2014 idle");
    g_shown = F_FRONT;
    ov_create();                                   /* also adds the small tray icon when needed */
    if (g_ovScale) SetTimer(g_wnd, TIMER_OV, 4000, NULL);
    g_loopMode = -1; plan();
    HWINEVENTHOOK fh = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, on_foreground, 0, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);   /* drop startup pages */
    MSG msgv;
    while (GetMessageW(&msgv, NULL, 0, 0) > 0) { TranslateMessage(&msgv); DispatchMessageW(&msgv); }
    if (fh) UnhookWinEvent(fh);
    if (mx) CloseHandle(mx);
    return 0;
}

/* ================================================================== entry */
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmd, int show) {
    (void)hi; (void)hp; (void)show;
    int argc = 0; WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int hook = 0, tray = 0, inst = 0, uninst = 0, addh = 0, remh = 0;
    for (int i = 1; i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--hook")) hook = 1;
        else if (!lstrcmpiW(argv[i], L"--add-hooks")) addh = 1;
        else if (!lstrcmpiW(argv[i], L"--remove-hooks")) remh = 1;
        else if (!lstrcmpiW(argv[i], L"--tray")) tray = 1;
        else if (!lstrcmpiW(argv[i], L"--install")) inst = 1;
        else if (!lstrcmpiW(argv[i], L"--uninstall")) uninst = 1;
        else if (!lstrcmpiW(argv[i], L"--quiet")) g_quiet = 1;
    }
    (void)cmd;
    if (hook) return hook_main();       /* hot path: no DPI/GDI setup at all */
    init_paths();
    SetProcessDPIAware();
    if (addh || remh) {   /* only (re)write the hooks in settings.json; exit code 0 = ok */
        WCHAR err[600] = L""; int ok = update_settings(addh, err, 600);
        if (!ok) msg(MB_ICONERROR, err);
        return ok ? 0 : 4;
    }
    if (uninst) return do_uninstall(0);
    if (inst) return do_install();
    if (tray || !lstrcmpiW(g_self, g_installExe)) return tray_main();
    return do_install();
}
