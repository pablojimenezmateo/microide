// The plugin sandbox's string.find / string.match / string.gmatch / string.gsub:
// Lua 5.4.8's own pattern matcher, made to honour the plugin call's time budget.
//
// Why: the per-call watchdog (LuaRuntime::TimeoutHook) is a count hook, and a
// count hook only runs between Lua VM instructions. A pattern match is ONE call
// into C, so a backtracking pattern on a long subject ran unbounded under it --
// `([%w_]+)%s*%b()` on a 300 000-character line is ~10^11 steps -- while the UI
// thread waited on the plugin worker for the answer (soak 7ff52075dc30: the
// editor stopped answering in `tests-discover`). The matcher below counts its
// own steps and checks the same deadline every kStepsPerDeadlineCheck, raising
// the watchdog's own error; everything else is the reference implementation,
// so patterns mean exactly what they mean in Lua.
//
// Raising: the functions here hold no C++ object with a destructor, so a raise
// from inside the matcher's recursion (lua_error) skips nothing; luaL_error is
// banned in src/plugin, so messages are pushed with lua_pushfstring/PushMessage.
//
// Adapted from lstrlib.c, Lua 5.4.8:
//   Copyright (C) 1994-2025 Lua.org, PUC-Rio.
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in
//   all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//   SOFTWARE.

#include "plugin/LuaBudgetedPatterns.h"

#include "plugin/LuaRuntime.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstring>

namespace microide::plugin {
namespace {

constexpr int kMaxCaptures = 32;          // LUA_MAXCAPTURES
constexpr int kMaxMatchDepth = 200;       // MAXCCALLS
constexpr ptrdiff_t kCapUnfinished = -1;  // CAP_UNFINISHED
constexpr ptrdiff_t kCapPosition = -2;    // CAP_POSITION
constexpr char kEsc = '%';                // L_ESC
constexpr const char* kSpecials = "^$*+?.([%-";
// How many matcher steps run between two looks at the clock: a few microseconds
// of matching, so a spent budget is noticed within a millisecond of it ending.
constexpr unsigned kStepsPerDeadlineCheck = 4096;

inline int Uchar(char c) { return static_cast<unsigned char>(c); }

struct MatchState {
  const char* src_init;
  const char* src_end;
  const char* p_end;
  lua_State* L;
  int matchdepth;
  unsigned char level;
  unsigned steps;
  struct {
    const char* init;
    ptrdiff_t len;
  } capture[kMaxCaptures];
};

[[noreturn]] void Raise(lua_State* L, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  lua_pushvfstring(L, format, arguments);
  va_end(arguments);
  lua_error(L);
  __builtin_unreachable();
}

// The watchdog, for the work no count hook sees.
void Step(MatchState* ms) {
  if (++ms->steps % kStepsPerDeadlineCheck == 0 && LuaRuntime::CallBudgetSpent(ms->L)) {
    Raise(ms->L, "%s", LuaRuntime::kTimeBudgetMessage);
  }
}

const char* Match(MatchState* ms, const char* s, const char* p);

int CheckCapture(MatchState* ms, int l) {
  l -= '1';
  if (l < 0 || l >= ms->level || ms->capture[l].len == kCapUnfinished) {
    Raise(ms->L, "invalid capture index %%%d", l + 1);
  }
  return l;
}

int CaptureToClose(MatchState* ms) {
  int level = ms->level;
  for (level--; level >= 0; level--) {
    if (ms->capture[level].len == kCapUnfinished) {
      return level;
    }
  }
  Raise(ms->L, "invalid pattern capture");
}

const char* ClassEnd(MatchState* ms, const char* p) {
  switch (*p++) {
    case kEsc: {
      if (p == ms->p_end) {
        Raise(ms->L, "malformed pattern (ends with '%%')");
      }
      return p + 1;
    }
    case '[': {
      if (*p == '^') {
        p++;
      }
      do {
        if (p == ms->p_end) {
          Raise(ms->L, "malformed pattern (missing ']')");
        }
        if (*(p++) == kEsc && p < ms->p_end) {
          p++;
        }
      } while (*p != ']');
      return p + 1;
    }
    default:
      return p;
  }
}

int MatchClass(int c, int cl) {
  int res;
  switch (std::tolower(cl)) {
    case 'a': res = std::isalpha(c); break;
    case 'c': res = std::iscntrl(c); break;
    case 'd': res = std::isdigit(c); break;
    case 'g': res = std::isgraph(c); break;
    case 'l': res = std::islower(c); break;
    case 'p': res = std::ispunct(c); break;
    case 's': res = std::isspace(c); break;
    case 'u': res = std::isupper(c); break;
    case 'w': res = std::isalnum(c); break;
    case 'x': res = std::isxdigit(c); break;
    case 'z': res = (c == 0); break;
    default: return cl == c;
  }
  return std::islower(cl) ? res : !res;
}

int MatchBracketClass(int c, const char* p, const char* ec) {
  int sig = 1;
  if (*(p + 1) == '^') {
    sig = 0;
    p++;
  }
  while (++p < ec) {
    if (*p == kEsc) {
      p++;
      if (MatchClass(c, Uchar(*p))) {
        return sig;
      }
    } else if (*(p + 1) == '-' && p + 2 < ec) {
      p += 2;
      if (Uchar(*(p - 2)) <= c && c <= Uchar(*p)) {
        return sig;
      }
    } else if (Uchar(*p) == c) {
      return sig;
    }
  }
  return !sig;
}

int SingleMatch(MatchState* ms, const char* s, const char* p, const char* ep) {
  if (s >= ms->src_end) {
    return 0;
  }
  const int c = Uchar(*s);
  switch (*p) {
    case '.': return 1;
    case kEsc: return MatchClass(c, Uchar(*(p + 1)));
    case '[': return MatchBracketClass(c, p, ep - 1);
    default: return Uchar(*p) == c;
  }
}

const char* MatchBalance(MatchState* ms, const char* s, const char* p) {
  if (p >= ms->p_end - 1) {
    Raise(ms->L, "malformed pattern (missing arguments to '%%b')");
  }
  if (*s != *p) {
    return nullptr;
  }
  const int b = *p;
  const int e = *(p + 1);
  int cont = 1;
  while (++s < ms->src_end) {
    if (*s == e) {
      if (--cont == 0) {
        return s + 1;
      }
    } else if (*s == b) {
      cont++;
    }
  }
  return nullptr;
}

const char* MaxExpand(MatchState* ms, const char* s, const char* p, const char* ep) {
  ptrdiff_t i = 0;
  while (SingleMatch(ms, s + i, p, ep)) {
    i++;
  }
  while (i >= 0) {
    const char* res = Match(ms, s + i, ep + 1);
    if (res != nullptr) {
      return res;
    }
    i--;
  }
  return nullptr;
}

const char* MinExpand(MatchState* ms, const char* s, const char* p, const char* ep) {
  for (;;) {
    const char* res = Match(ms, s, ep + 1);
    if (res != nullptr) {
      return res;
    }
    if (!SingleMatch(ms, s, p, ep)) {
      return nullptr;
    }
    s++;
  }
}

const char* StartCapture(MatchState* ms, const char* s, const char* p, ptrdiff_t what) {
  const int level = ms->level;
  if (level >= kMaxCaptures) {
    Raise(ms->L, "too many captures");
  }
  ms->capture[level].init = s;
  ms->capture[level].len = what;
  ms->level = static_cast<unsigned char>(level + 1);
  const char* res = Match(ms, s, p);
  if (res == nullptr) {
    ms->level--;
  }
  return res;
}

const char* EndCapture(MatchState* ms, const char* s, const char* p) {
  const int l = CaptureToClose(ms);
  ms->capture[l].len = s - ms->capture[l].init;
  const char* res = Match(ms, s, p);
  if (res == nullptr) {
    ms->capture[l].len = kCapUnfinished;
  }
  return res;
}

const char* MatchCapture(MatchState* ms, const char* s, int l) {
  l = CheckCapture(ms, l);
  const size_t len = static_cast<size_t>(ms->capture[l].len);
  if (static_cast<size_t>(ms->src_end - s) >= len && std::memcmp(ms->capture[l].init, s, len) == 0) {
    return s + len;
  }
  return nullptr;
}

const char* Match(MatchState* ms, const char* s, const char* p) {
  if (ms->matchdepth-- == 0) {
    Raise(ms->L, "pattern too complex");
  }
  Step(ms);
init:
  if (p != ms->p_end) {
    switch (*p) {
      case '(': {
        if (*(p + 1) == ')') {
          s = StartCapture(ms, s, p + 2, kCapPosition);
        } else {
          s = StartCapture(ms, s, p + 1, kCapUnfinished);
        }
        break;
      }
      case ')': {
        s = EndCapture(ms, s, p + 1);
        break;
      }
      case '$': {
        if (p + 1 != ms->p_end) {
          goto dflt;
        }
        s = (s == ms->src_end) ? s : nullptr;
        break;
      }
      case kEsc: {
        switch (*(p + 1)) {
          case 'b': {
            s = MatchBalance(ms, s, p + 2);
            if (s != nullptr) {
              p += 4;
              Step(ms);
              goto init;
            }
            break;
          }
          case 'f': {
            p += 2;
            if (*p != '[') {
              Raise(ms->L, "missing '[' after '%%f' in pattern");
            }
            const char* ep = ClassEnd(ms, p);
            const char previous = (s == ms->src_init) ? '\0' : *(s - 1);
            if (!MatchBracketClass(Uchar(previous), p, ep - 1) && MatchBracketClass(Uchar(*s), p, ep - 1)) {
              p = ep;
              goto init;
            }
            s = nullptr;
            break;
          }
          case '0': case '1': case '2': case '3': case '4':
          case '5': case '6': case '7': case '8': case '9': {
            s = MatchCapture(ms, s, Uchar(*(p + 1)));
            if (s != nullptr) {
              p += 2;
              goto init;
            }
            break;
          }
          default:
            goto dflt;
        }
        break;
      }
      default:
      dflt: {
        const char* ep = ClassEnd(ms, p);
        if (!SingleMatch(ms, s, p, ep)) {
          if (*ep == '*' || *ep == '?' || *ep == '-') {
            p = ep + 1;
            goto init;
          }
          s = nullptr;
        } else {
          switch (*ep) {
            case '?': {
              const char* res = Match(ms, s + 1, ep + 1);
              if (res != nullptr) {
                s = res;
              } else {
                p = ep + 1;
                goto init;
              }
              break;
            }
            case '+':
              s++;
              [[fallthrough]];
            case '*':
              s = MaxExpand(ms, s, p, ep);
              break;
            case '-':
              s = MinExpand(ms, s, p, ep);
              break;
            default:
              s++;
              p = ep;
              goto init;
          }
        }
        break;
      }
    }
  }
  ms->matchdepth++;
  return s;
}

const char* MemFind(const char* s1, size_t l1, const char* s2, size_t l2) {
  if (l2 == 0) {
    return s1;
  }
  if (l2 > l1) {
    return nullptr;
  }
  l2--;
  l1 = l1 - l2;
  const char* init = nullptr;
  while (l1 > 0 && (init = static_cast<const char*>(std::memchr(s1, *s2, l1))) != nullptr) {
    init++;
    if (std::memcmp(init, s2 + 1, l2) == 0) {
      return init - 1;
    }
    l1 -= static_cast<size_t>(init - s1);
    s1 = init;
  }
  return nullptr;
}

size_t GetOneCapture(MatchState* ms, int i, const char* s, const char* e, const char** cap) {
  if (i >= ms->level) {
    if (i != 0) {
      Raise(ms->L, "invalid capture index %%%d", i + 1);
    }
    *cap = s;
    return static_cast<size_t>(e - s);
  }
  const ptrdiff_t capl = ms->capture[i].len;
  *cap = ms->capture[i].init;
  if (capl == kCapUnfinished) {
    Raise(ms->L, "unfinished capture");
  }
  if (capl == kCapPosition) {
    lua_pushinteger(ms->L, (ms->capture[i].init - ms->src_init) + 1);
  }
  return static_cast<size_t>(capl);
}

void PushOneCapture(MatchState* ms, int i, const char* s, const char* e) {
  const char* cap = nullptr;
  const ptrdiff_t l = static_cast<ptrdiff_t>(GetOneCapture(ms, i, s, e, &cap));
  if (l != kCapPosition) {
    lua_pushlstring(ms->L, cap, static_cast<size_t>(l));
  }
}

int PushCaptures(MatchState* ms, const char* s, const char* e) {
  const int nlevels = (ms->level == 0 && s != nullptr) ? 1 : ms->level;
  luaL_checkstack(ms->L, nlevels, "too many captures");
  for (int i = 0; i < nlevels; i++) {
    PushOneCapture(ms, i, s, e);
  }
  return nlevels;
}

int NoSpecials(const char* p, size_t l) {
  size_t upto = 0;
  do {
    if (std::strpbrk(p + upto, kSpecials) != nullptr) {
      return 0;
    }
    upto += std::strlen(p + upto) + 1;
  } while (upto <= l);
  return 1;
}

void PrepState(MatchState* ms, lua_State* L, const char* s, size_t ls, const char* p, size_t lp) {
  ms->L = L;
  ms->matchdepth = kMaxMatchDepth;
  ms->steps = 0;
  ms->src_init = s;
  ms->src_end = s + ls;
  ms->p_end = p + lp;
}

void RePrepState(MatchState* ms) { ms->level = 0; }

size_t PosRelatI(lua_Integer pos, size_t len) {
  if (pos > 0) {
    return static_cast<size_t>(pos);
  }
  if (pos == 0) {
    return 1;
  }
  if (pos < -static_cast<lua_Integer>(len)) {
    return 1;
  }
  return len + static_cast<size_t>(pos) + 1;
}

int FindAux(lua_State* L, int find) {
  size_t ls = 0;
  size_t lp = 0;
  const char* s = luaL_checklstring(L, 1, &ls);
  const char* p = luaL_checklstring(L, 2, &lp);
  const size_t init = PosRelatI(luaL_optinteger(L, 3, 1), ls) - 1;
  if (init > ls) {
    lua_pushnil(L);
    return 1;
  }
  if (find && (lua_toboolean(L, 4) || NoSpecials(p, lp))) {
    const char* s2 = MemFind(s + init, ls - init, p, lp);
    if (s2 != nullptr) {
      lua_pushinteger(L, (s2 - s) + 1);
      lua_pushinteger(L, static_cast<lua_Integer>((s2 - s) + static_cast<ptrdiff_t>(lp)));
      return 2;
    }
  } else {
    MatchState ms;
    const char* s1 = s + init;
    const int anchor = (*p == '^');
    if (anchor) {
      p++;
      lp--;
    }
    PrepState(&ms, L, s, ls, p, lp);
    do {
      RePrepState(&ms);
      const char* res = Match(&ms, s1, p);
      if (res != nullptr) {
        if (find) {
          lua_pushinteger(L, (s1 - s) + 1);
          lua_pushinteger(L, res - s);
          return PushCaptures(&ms, nullptr, nullptr) + 2;
        }
        return PushCaptures(&ms, s1, res);
      }
    } while (s1++ < ms.src_end && !anchor);
  }
  lua_pushnil(L);
  return 1;
}

int Find(lua_State* L) { return FindAux(L, 1); }
int MatchFn(lua_State* L) { return FindAux(L, 0); }

struct GMatchState {
  const char* src;
  const char* p;
  const char* lastmatch;
  MatchState ms;
};

int GMatchAux(lua_State* L) {
  auto* gm = static_cast<GMatchState*>(lua_touserdata(L, lua_upvalueindex(3)));
  gm->ms.L = L;
  // Each call is a call of its own budget: the steps start over with the deadline.
  gm->ms.steps = 0;
  for (const char* src = gm->src; src <= gm->ms.src_end; src++) {
    RePrepState(&gm->ms);
    const char* e = Match(&gm->ms, src, gm->p);
    if (e != nullptr && e != gm->lastmatch) {
      gm->src = gm->lastmatch = e;
      return PushCaptures(&gm->ms, src, e);
    }
  }
  return 0;
}

int GMatch(lua_State* L) {
  size_t ls = 0;
  size_t lp = 0;
  const char* s = luaL_checklstring(L, 1, &ls);
  const char* p = luaL_checklstring(L, 2, &lp);
  size_t init = PosRelatI(luaL_optinteger(L, 3, 1), ls) - 1;
  lua_settop(L, 2);
  auto* gm = static_cast<GMatchState*>(lua_newuserdatauv(L, sizeof(GMatchState), 0));
  if (init > ls) {
    init = ls + 1;
  }
  PrepState(&gm->ms, L, s, ls, p, lp);
  gm->src = s + init;
  gm->p = p;
  gm->lastmatch = nullptr;
  lua_pushcclosure(L, GMatchAux, 3);
  return 1;
}

void AddS(MatchState* ms, luaL_Buffer* b, const char* s, const char* e) {
  size_t l = 0;
  lua_State* L = ms->L;
  const char* news = lua_tolstring(L, 3, &l);
  const char* p = nullptr;
  while ((p = static_cast<const char*>(std::memchr(news, kEsc, l))) != nullptr) {
    luaL_addlstring(b, news, static_cast<size_t>(p - news));
    p++;
    if (*p == kEsc) {
      luaL_addchar(b, *p);
    } else if (*p == '0') {
      luaL_addlstring(b, s, static_cast<size_t>(e - s));
    } else if (std::isdigit(Uchar(*p))) {
      const char* cap = nullptr;
      const ptrdiff_t resl = static_cast<ptrdiff_t>(GetOneCapture(ms, *p - '1', s, e, &cap));
      if (resl == kCapPosition) {
        luaL_addvalue(b);
      } else {
        luaL_addlstring(b, cap, static_cast<size_t>(resl));
      }
    } else {
      Raise(L, "invalid use of '%c' in replacement string", kEsc);
    }
    l -= static_cast<size_t>(p + 1 - news);
    news = p + 1;
  }
  luaL_addlstring(b, news, l);
}

int AddValue(MatchState* ms, luaL_Buffer* b, const char* s, const char* e, int tr) {
  lua_State* L = ms->L;
  switch (tr) {
    case LUA_TFUNCTION: {
      lua_pushvalue(L, 3);
      const int n = PushCaptures(ms, s, e);
      lua_call(L, n, 1);
      break;
    }
    case LUA_TTABLE: {
      PushOneCapture(ms, 0, s, e);
      // __index honoured, as Lua's gsub does (the architecture lint's exemption).
      lua_gettable(L, 3);
      break;
    }
    default: {
      AddS(ms, b, s, e);
      return 1;
    }
  }
  if (!lua_toboolean(L, -1)) {
    lua_pop(L, 1);
    luaL_addlstring(b, s, static_cast<size_t>(e - s));
    return 0;
  }
  if (!lua_isstring(L, -1)) {
    Raise(L, "invalid replacement value (a %s)", luaL_typename(L, -1));
  }
  luaL_addvalue(b);
  return 1;
}

int GSub(lua_State* L) {
  size_t srcl = 0;
  size_t lp = 0;
  const char* src = luaL_checklstring(L, 1, &srcl);
  const char* p = luaL_checklstring(L, 2, &lp);
  const char* lastmatch = nullptr;
  const int tr = lua_type(L, 3);
  const lua_Integer max_s = luaL_optinteger(L, 4, static_cast<lua_Integer>(srcl) + 1);
  const int anchor = (*p == '^');
  lua_Integer n = 0;
  int changed = 0;
  MatchState ms;
  luaL_Buffer b;
  luaL_argexpected(L, tr == LUA_TNUMBER || tr == LUA_TSTRING || tr == LUA_TFUNCTION || tr == LUA_TTABLE, 3,
                   "string/function/table");
  luaL_buffinit(L, &b);
  if (anchor) {
    p++;
    lp--;
  }
  PrepState(&ms, L, src, srcl, p, lp);
  while (n < max_s) {
    RePrepState(&ms);
    const char* e = Match(&ms, src, p);
    if (e != nullptr && e != lastmatch) {
      n++;
      changed = AddValue(&ms, &b, src, e, tr) | changed;
      src = lastmatch = e;
    } else if (src < ms.src_end) {
      luaL_addchar(&b, *src++);
    } else {
      break;
    }
    if (anchor) {
      break;
    }
  }
  if (!changed) {
    lua_pushvalue(L, 1);
  } else {
    luaL_addlstring(&b, src, static_cast<size_t>(ms.src_end - src));
    luaL_pushresult(&b);
  }
  lua_pushinteger(L, n);
  return 2;
}

}  // namespace

void InstallBudgetedPatternFunctions(lua_State* state) {
  // The string library's table is also the strings' metatable __index, so both
  // `string.match(s, p)` and `s:match(p)` reach these.
  lua_getglobal(state, "string");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return;
  }
  const luaL_Reg functions[] = {
      {"find", Find}, {"match", MatchFn}, {"gmatch", GMatch}, {"gsub", GSub}, {nullptr, nullptr}};
  luaL_setfuncs(state, functions, 0);
  lua_pop(state, 1);
}

}  // namespace microide::plugin

#endif  // MICROIDE_HAS_LUA_PLUGINS
