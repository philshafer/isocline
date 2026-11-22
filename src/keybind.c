/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "keybind.h"

typedef struct keybinding_s {
    code_t              key;
    ic_keyaction_fun_t  *action;
    void                *arg;
} keybinding_t;

struct keybindings_s {
    keybinding_t    *elems;
    ssize_t         count;
    ssize_t         len;
    alloc_t         *mem;
};

ic_private keybindings_t *
keybindings_new (alloc_t *mem)
{
    keybindings_t *kbs = mem_zalloc_tp(mem, keybindings_t);
    if (kbs == NULL)
        return NULL;

    kbs->mem = mem;
    return kbs;
}

ic_private void
keybindings_free (keybindings_t *kbs)
{
    if (kbs == NULL)
        return;

    mem_free(kbs->mem, kbs->elems);
    mem_free(kbs->mem, kbs);
}

static ssize_t
keybindings_find_index (const keybindings_t *kbs, code_t key)
{
    for (ssize_t i = 0; i < kbs->count; i += 1) {
        if (kbs->elems[i].key == key)
            return i;
    }

    return -1;
}

ic_private bool
keybindings_bind (keybindings_t *kbs, code_t key, ic_keyaction_fun_t *action,
        void *arg)
{
    if (kbs == NULL || action == NULL)
        return false;

    ssize_t idx = keybindings_find_index(kbs, key);
    if (idx >= 0) {
        kbs->elems[idx].action = action;
        kbs->elems[idx].arg = arg;
        return true;
    }

    if (kbs->count >= kbs->len) {
        ssize_t newlen = (kbs->len == 0) ? 8 : kbs->len * 2;
        keybinding_t *newelems = mem_realloc_tp(kbs->mem, keybinding_t,
                kbs->elems, newlen);
        if (newelems == NULL)
            return false;

        kbs->elems = newelems;
        kbs->len = newlen;
    }

    kbs->elems[kbs->count].key = key;
    kbs->elems[kbs->count].action = action;
    kbs->elems[kbs->count].arg = arg;
    kbs->count += 1;
    return true;
}

ic_private bool
keybindings_unbind (keybindings_t *kbs, code_t key)
{
    if (kbs == NULL)
        return false;

    ssize_t idx = keybindings_find_index(kbs, key);
    if (idx < 0)
        return false;

    for (ssize_t i = idx + 1; i < kbs->count; i += 1)
        kbs->elems[i - 1] = kbs->elems[i];

    kbs->count -= 1;
    return true;
}

ic_private bool
keybindings_find (keybindings_t *kbs, code_t key, ic_keyaction_fun_t **action,
        void **arg)
{
    if (kbs == NULL)
        return false;

    ssize_t idx = keybindings_find_index(kbs, key);
    if (idx < 0)
        return false;

    if (action != NULL)
        *action = kbs->elems[idx].action;
    if (arg != NULL)
        *arg = kbs->elems[idx].arg;

    return true;
}

ic_public bool
ic_bind_key (ic_key_t key, ic_keyaction_fun_t *action, void *arg)
{
    ic_env_t *env = ic_get_env();
    if (env == NULL)
        return false;

    return keybindings_bind(env->keybindings, (code_t) key, action, arg);
}

ic_public bool
ic_unbind_key (ic_key_t key)
{
    ic_env_t *env = ic_get_env();
    if (env == NULL)
        return false;

    return keybindings_unbind(env->keybindings, (code_t) key);
}

ic_public const char *
ic_keyaction_input (ic_keyaction_env_t *kenv, long *cursor)
{
    if (kenv == NULL)
        return NULL;

    if (cursor != NULL)
        *cursor = kenv->cursor;

    return kenv->input;
}

ic_public void *
ic_keyaction_arg (const ic_keyaction_env_t *kenv)
{
    return (kenv == NULL) ? NULL : kenv->arg;
}

ic_public void
ic_keyaction_dismiss_completions (ic_keyaction_env_t *kenv)
{
    if (kenv == NULL || kenv->suppressed == NULL)
        return;

    *kenv->suppressed = true;
}

ic_public bool
ic_keyaction_completions_suppressed (const ic_keyaction_env_t *kenv)
{
    if (kenv == NULL || kenv->suppressed == NULL)
        return false;

    return *kenv->suppressed;
}

ic_public bool
ic_keyaction_replace (ic_keyaction_env_t *kenv, const char *text,
        long delete_before, long delete_after)
{
    if (kenv == NULL || kenv->replace == NULL)
        return false;

    return (*kenv->replace)(kenv->env, kenv->closure, text, delete_before,
            delete_after);
}
