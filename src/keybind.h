/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef IC_KEYBIND_H
#define IC_KEYBIND_H

#include "common.h"
#include "tty.h"

/*
 * Key bindings let a caller override the built-in handling of a key,
 * or attach behavior to a key that has no built-in meaning.  The
 * number of bindings is expected to stay small, so they are kept in
 * a small growable array and found with a linear scan.
 */

typedef struct keybindings_s keybindings_t;

ic_private keybindings_t *keybindings_new(alloc_t *mem);
ic_private void keybindings_free(keybindings_t *kbs);
ic_private bool keybindings_bind(keybindings_t *kbs, code_t key,
        ic_keyaction_fun_t *action, void *arg);
ic_private bool keybindings_unbind(keybindings_t *kbs, code_t key);
ic_private bool keybindings_find(keybindings_t *kbs, code_t key,
        ic_keyaction_fun_t **action, void **arg);

/*
 * A key action's request to replace part of the input around the
 * cursor, mirroring ic_completion_fun_t: delete "delete_before" bytes
 * before the cursor and "delete_after" bytes after it, then insert
 * "text" in their place.  "closure" is editline.c's editor_t, opaque
 * here just like ic_completion_env_s's own closure is opaque to
 * completions.c.
 */
typedef bool (ic_keyaction_replace_fun_t)( ic_env_t *env, void *closure,
        const char *text, long delete_before, long delete_after );

/*
 * A key action's request to open (or, for a single unambiguous match,
 * directly complete) the live completion list for the word at the
 * cursor, the same way TAB does.  "closure" is editline.c's editor_t,
 * opaque here just like "replace"'s.
 */
typedef void (ic_keyaction_open_completions_fun_t)( ic_env_t *env,
        void *closure );

/*
 * The environment passed to a key action callback, mirroring
 * ic_completion_env_s: a snapshot of the current input/cursor plus
 * the bound arg.  "suppressed" points at the current editor's
 * completion-suppression flag so a callback can toggle it without
 * this header needing to know about editor_t.  "replace"/"closure"
 * let a callback edit the input the same way; see
 * ic_keyaction_replace().  "open_completions" lets a callback reveal
 * the live completion list the same way; see
 * ic_keyaction_open_completions().
 */
struct ic_keyaction_env_s {
    ic_env_t    *env;
    const char  *input;
    long        cursor;
    void        *arg;
    bool        *suppressed;
    void        *closure;
    ic_keyaction_replace_fun_t *replace;
    ic_keyaction_open_completions_fun_t *open_completions;
};

#endif /* IC_KEYBIND_H */
