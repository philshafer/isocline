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
 * The environment passed to a key action callback, mirroring
 * ic_completion_env_s: a snapshot of the current input/cursor plus
 * the bound arg.  "suppressed" points at the current editor's
 * completion-suppression flag so a callback can toggle it without
 * this header needing to know about editor_t.
 */
struct ic_keyaction_env_s {
    ic_env_t    *env;
    const char  *input;
    long        cursor;
    void        *arg;
    bool        *suppressed;
};

#endif /* IC_KEYBIND_H */
