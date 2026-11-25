/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

//-------------------------------------------------------------
// Completion menu: this file is included in editline.c
//-------------------------------------------------------------

// return true iff the buffer or cursor position was actually changed;
// clean up any dirtiness in the undo stack. update eb->pos if appropriate
static bool edit_completion_commit(editor_t* eb, ssize_t newpos) {
  if (newpos == IC_COMP_APPLY_FAIL) {
    editor_undo_restore(eb, false);
    return false;
  }
  if (newpos == IC_COMP_APPLY_NOOP) {
    editor_undo_forget(eb);
    return false;
  }

  eb->pos = newpos;
  return true;
}

// return true if anything changed
static bool edit_complete(ic_env_t* env, editor_t* eb, ssize_t idx) {
  editor_start_modify(eb);
  ssize_t newpos = completions_apply(env->completions, idx, eb->input, eb->pos);
  bool buf_or_pos_changed = edit_completion_commit(eb, newpos);

  // trigger a refresh if the buffer or cursor position changed
  if (buf_or_pos_changed)
    edit_refresh(env, eb);
  // or when choosing between menu items, even if the current item is the same
  // as the buffer, because we need to highlight that item
  else if (newpos == IC_COMP_APPLY_NOOP && completions_count(env->completions) > 1)
    edit_refresh(env, eb);

  return buf_or_pos_changed;
}

// commit the currently selected live-list candidate into the input; unlike
// edit_generate_completions() this never falls into the blocking menu.
// list_selected is a "real" (informational-row-excluded) index, so it is
// offset by the informational row count to land on the matching array
// entry; see completions_add_info().
static bool edit_complete_live_selected(ic_env_t* env, editor_t* eb) {
  if (eb->list_count <= 0 || eb->list_selected < 0) return false;
  ssize_t idx = completions_count_info(env->completions) + eb->list_selected;
  bool changed = edit_complete(env, eb, idx);
  eb->list_count = 0;
  eb->list_selected = -1;
  eb->list_open = false;
  return changed;
}

// commit the longest prefix common to every live-list candidate (not just
// the selected one) and hide the list; returns whether that prefix landed
// on a word boundary in every candidate, so the caller knows whether it is
// safe to also insert a separator
static bool edit_complete_live_prefix(ic_env_t* env, editor_t* eb) {
  if (eb->list_count <= 0) return false;
  editor_start_modify(eb);
  bool full_token = false;
  ssize_t newpos = completions_apply_longest_prefix(env->completions, eb->input, eb->pos, &full_token);
  edit_completion_commit(eb, newpos);
  eb->list_count = 0;
  eb->list_selected = -1;
  eb->list_open = false;
  sbuf_clear(eb->extra);
  edit_refresh(env, eb);
  return full_token;
}

// reveal the live completion list for the word at the cursor, or
// complete it directly when there's only one candidate; shared by the
// TAB/alt-? handling in edit_line() and ic_keyaction_open_completions().
// An informational-only list (no real candidates, just a row added by
// completions_add_info(), such as sdb's "<[Enter]> Execute this
// command") is also worth opening, since it has something to show even
// though there is nothing to complete.
static void edit_open_completions(ic_env_t* env, editor_t* eb) {
  if (eb->list_count == 1) {
    if (edit_complete_live_prefix(env, eb))
      edit_insert_char(env, eb, ' ');
  }
  else if (eb->list_count > 1
           || completions_count_info(env->completions) > 0) {
    eb->list_open = true;
    edit_refresh_hint(env, eb);
  }
  else {
    term_beep(env->term);
  }
}

static void edit_complete_longest_prefix(ic_env_t* env, editor_t* eb ) {
  editor_start_modify(eb);
  ssize_t newpos = completions_apply_longest_prefix( env->completions, eb->input, eb->pos, NULL );
  edit_completion_commit(eb, newpos);
}

ic_private void sbuf_append_tagged( stringbuf_t* sb, const char* tag, const char* content ) {
  sbuf_appendf(sb, "[%s]", tag);  
  sbuf_append(sb,content);
  sbuf_append(sb,"[/]");
}

static void editor_append_completion(ic_env_t* env, editor_t* eb, ssize_t idx, ssize_t width, bool numbered, bool selected, const char* selected_style ) {
  const char* help = NULL;
  const char* display = completions_get_display(env->completions, idx, &help);
  if (display == NULL) return;
  if (numbered) {
    sbuf_appendf(eb->extra, "[ic-info]%s%zd [/]", (selected ? (tty_is_utf8(env->tty) ? "\xE2\x86\x92" : "*") : " "), 1 + idx);
    width -= 3;
  }
  else if (env->complete_no_numbers) {
    // plain two-space indent with the name padded to a fixed column,
    // instead of the classic numbered-menu layout
    sbuf_append(eb->extra, "  ");
    width = 15;
  }

  // when a row is selected and the whole-row-inverse style is on, the
  // name and help render together under a single style tag so help
  // loses its usual dim styling too
  bool whole_row = (selected && !numbered && env->complete_inverse_selection);

  if (width > 0) {
    sbuf_appendf(eb->extra, "[width=\"%zd;left; ;on\"]", width );
  }
  if (selected) {
    sbuf_appendf(eb->extra, "[%s]", selected_style);
  }
  sbuf_append(eb->extra, display);
  if (selected && !whole_row) { sbuf_appendf(eb->extra,"[/%s]", selected_style); }
  if (help != NULL) {
    sbuf_append(eb->extra, "  ");
    if (whole_row)
      sbuf_append(eb->extra, help);
    else
      sbuf_append_tagged(eb->extra, "ic-info", help );
  }
  if (selected && whole_row) { sbuf_appendf(eb->extra,"[/%s]", selected_style); }
  if (width > 0) { sbuf_append(eb->extra,"[/width]"); }
}

// recompute the live completion list for the current word and set eb->hint
// to the selected candidate's remaining text; hides both when completions
// are suppressed for this word (ic_keyaction_dismiss_completions())
static void edit_update_live_list(ic_env_t* env, editor_t* eb) {
  if (eb->completions_suppressed) {
    eb->list_count = 0;
    eb->list_open = false;
    return;
  }

  // "count" excludes any informational (display-only) rows added via
  // completions_add_info(); "infocnt" is just those.  Array indices into
  // env->completions therefore run [0, infocnt) for informational rows
  // and [infocnt, infocnt + count) for real candidates, since sorting
  // always puts the (empty-replacement) informational rows first.
  ssize_t count = completions_generate(env, env->completions, sbuf_string(eb->input), eb->pos, IC_MAX_COMPLETIONS_TO_TRY);
  ssize_t infocnt = completions_count_info(env->completions);
  if (count <= 0 && infocnt <= 0) {
    eb->list_count = 0;
    eb->list_open = false;
    return;
  }
  completions_sort(env->completions);

  // a single, unambiguous match is always "selected" -- there is nothing
  // else to toggle to; with two or more, a selection out of range (the
  // set narrowed or widened since the last keystroke) goes back to "none"
  if (count == 1)
    eb->list_selected = 0;
  else if (eb->list_selected >= count)
    eb->list_selected = -1;
  eb->list_count = count;

  const bool show_list = (eb->list_open && (count > 1 || infocnt > 0));

  if (eb->list_selected >= 0) {
    const char* help = NULL;
    const char* hint = completions_get_hint(env->completions, infocnt + eb->list_selected, &help);
    if (hint != NULL) {
      sbuf_replace(eb->hint, hint);
      editor_append_hint_help(eb, help);
    }
  }
  else {
    // nothing selected: no dim hint follows the cursor
    sbuf_clear(eb->hint);
    sbuf_clear(eb->hint_help);
  }

  sbuf_clear(eb->extra);
  if (!show_list) return;

  if (env->complete_no_numbers)
    sbuf_append(eb->extra, "[ic-info]Possible completions:[/]\n");

  for (ssize_t i = 0; i < infocnt; i++) {
    if (i > 0) sbuf_append(eb->extra, "\n");
    editor_append_completion(env, eb, i, -1, false, false, "ic-selected");
  }

  if (count <= 0)
    return;

  ssize_t termh = term_get_height(env->term);
  ssize_t maxrows = termh / 3;
  if (maxrows < 1) maxrows = 1;
  ssize_t first = 0;
  if (count > maxrows) {
    ssize_t sel = (eb->list_selected < 0 ? 0 : eb->list_selected);
    first = sel - maxrows + 1;
    if (first < 0) first = 0;
    if (first + maxrows > count) first = count - maxrows;
  }
  ssize_t last = (count > maxrows ? first + maxrows - 1 : count - 1);

  for (ssize_t i = first; i <= last; i++) {
    if (i > first || infocnt > 0) sbuf_append(eb->extra, "\n");
    editor_append_completion(env, eb, infocnt + i, -1, false, i == eb->list_selected, "ic-selected");
  }
}

// 2 and 3 column output up to 80 wide
#define IC_DISPLAY2_MAX    34
#define IC_DISPLAY2_COL    (3+IC_DISPLAY2_MAX)
#define IC_DISPLAY2_WIDTH  (2*IC_DISPLAY2_COL + 2)    // 75

#define IC_DISPLAY3_MAX    21
#define IC_DISPLAY3_COL    (3+IC_DISPLAY3_MAX)
#define IC_DISPLAY3_WIDTH  (3*IC_DISPLAY3_COL + 2*2)  // 76

static void editor_append_completion2(ic_env_t* env, editor_t* eb, ssize_t col_width, ssize_t idx1, ssize_t idx2, ssize_t selected ) {
  editor_append_completion(env, eb, idx1, col_width, true, (idx1 == selected), "ic-emphasis" );
  sbuf_append( eb->extra, "  ");
  editor_append_completion(env, eb, idx2, col_width, true, (idx2 == selected), "ic-emphasis" );
}

static void editor_append_completion3(ic_env_t* env, editor_t* eb, ssize_t col_width, ssize_t idx1, ssize_t idx2, ssize_t idx3, ssize_t selected ) {
  editor_append_completion(env, eb, idx1, col_width, true, (idx1 == selected), "ic-emphasis" );
  sbuf_append( eb->extra, "  ");
  editor_append_completion(env, eb, idx2, col_width, true, (idx2 == selected), "ic-emphasis" );
  sbuf_append( eb->extra, "  ");
  editor_append_completion(env, eb, idx3, col_width, true, (idx3 == selected), "ic-emphasis" );
}

static ssize_t edit_completions_max_width( ic_env_t* env, ssize_t count ) {
  ssize_t max_width = 0;
  for( ssize_t i = 0; i < count; i++) {
    const char* help = NULL;
    ssize_t w = bbcode_column_width(env->bbcode, completions_get_display(env->completions, i, &help));
    if (help != NULL) {
      w += 2 + bbcode_column_width(env->bbcode, help);
    }
    if (w > max_width) {
      max_width = w;
    }
  }
  return max_width;
}

static void edit_completion_menu(ic_env_t* env, editor_t* eb, bool more_available) {
  ssize_t count = completions_count(env->completions);
  ssize_t count_displayed = count;
  assert(count > 1);
  ssize_t selected = (env->complete_nopreview ? 0 : -1); // select first or none
  ssize_t percolumn = count;

again:
  // show first 9 (or 8) completions
  sbuf_clear(eb->extra);
  ssize_t twidth = term_get_width(env->term) - 1;
  ssize_t colwidth;
  if (count > 3 && ((colwidth = 3 + edit_completions_max_width(env, 9))*3 + 2*2) < twidth) {
    // display as a 3 column block
    count_displayed = (count > 9 ? 9 : count);
    percolumn = 3;
    for (ssize_t rw = 0; rw < percolumn; rw++) {
      if (rw > 0) sbuf_append(eb->extra, "\n");
      editor_append_completion3(env, eb, colwidth, rw, percolumn+rw, (2*percolumn)+rw, selected);
    }
  }
  else if (count > 4 && ((colwidth = 3 + edit_completions_max_width(env, 8))*2 + 2) < twidth) {
    // display as a 2 column block if some entries are too wide for three columns
    count_displayed = (count > 8 ? 8 : count);
    percolumn = (count_displayed <= 6 ? 3 : 4);
    for (ssize_t rw = 0; rw < percolumn; rw++) {
      if (rw > 0) sbuf_append(eb->extra, "\n");
      editor_append_completion2(env, eb, colwidth, rw, percolumn+rw, selected);
    }
  }
  else {
    // display as a list
    count_displayed = (count > 9 ? 9 : count);
    percolumn = count_displayed;
    for (ssize_t i = 0; i < count_displayed; i++) {
      if (i > 0) sbuf_append(eb->extra, "\n");
      editor_append_completion(env, eb, i, -1, true /* numbered */, selected == i, "ic-emphasis");
    }
  }
  if (count > count_displayed) {
    if (more_available) {
      sbuf_append(eb->extra, "\n[ic-info](press page-down (or ctrl-j) to see all further completions)[/]");
    }
    else {
      sbuf_appendf(eb->extra, "\n[ic-info](press page-down (or ctrl-j) to see all %zd completions)[/]", count );
    }
  }
  if (!env->complete_nopreview && selected >= 0 && selected <= count_displayed) {
    if (edit_complete(env,eb,selected))
      editor_undo_restore(eb,false);
  }
  else {
    edit_refresh(env, eb);
  }

  // read here; if not a valid key, push it back and return to main event loop
  code_t c = tty_read(env->tty);
  if (tty_term_resize_event(env->tty)) {
    edit_resize(env, eb);
  }
  sbuf_clear(eb->extra);
  
  // direct selection?
  if (c >= '1' && c <= '9') {
    ssize_t i = (c - '1');
    if (i < count) {
      selected = i;
      c = KEY_ENTER;
    }
  }

  // process commands
  if (c == KEY_DOWN || c == KEY_TAB) {
    selected++;
    if (selected >= count_displayed) {
      //term_beep(env->term);
      selected = 0;
    }
    goto again;
  }
  else if (c == KEY_UP || c == KEY_SHIFT_TAB) {
    selected--;
    if (selected < 0) {
      selected = count_displayed - 1;
      //term_beep(env->term);
    }
    goto again;
  }
  else if (c == KEY_F1) {
    edit_show_help(env, eb);
    goto again;
  }
  else if (c == KEY_ESC) {
    completions_clear(env->completions);
    edit_refresh(env,eb);
    c = 0; // ignore and return
  }
  else if (c == ' ') {
    // SPACE always completes to the current entry (defaulting to the
    // first one if none was explicitly highlighted yet via the arrow
    // keys) and then inserts a literal space after it, so the user
    // can immediately continue typing the next token. Otherwise it is
    // just an ordinary character and the generic "accept the preview
    // and keep typing" branch below would insert it verbatim instead
    // of completing the menu selection.
    ssize_t isel = (selected >= 0 ? selected : 0);
    assert(isel < count);
    c = 0;
    if (edit_complete(env, eb, isel)) {
      edit_insert_char(env, eb, ' ');
    }
  }
  else if (selected >= 0 && (c == KEY_ENTER || c == KEY_RIGHT || c == KEY_END)) /* || c == KEY_TAB*/ {
    // select the current entry
    assert(selected < count);
    c = 0;
    if (edit_complete(env, eb, selected) && env->complete_autotab) {
      tty_code_pushback(env->tty,KEY_EVENT_AUTOTAB); // immediately try to complete again
    }
  }
  else if (!env->complete_nopreview && !code_is_virt_key(c)) {
    // if in preview mode, select the current entry and exit the menu
    assert(selected < count);
    edit_complete(env, eb, selected); 
  }
  else if ((c == KEY_PAGEDOWN || c == KEY_LINEFEED) && count > 9) {
    // show all completions
    c = 0;
    if (more_available) {
      // generate all entries (up to the max (= 1000))
      count = completions_generate(env, env->completions, sbuf_string(eb->input), eb->pos, IC_MAX_COMPLETIONS_TO_SHOW);
    }
    rowcol_t rc;
    edit_get_rowcol(env,eb,&rc);
    edit_clear(env,eb);
    edit_write_prompt(env,eb,0,false);
    term_writeln(env->term, "");
    for(ssize_t i = 0; i < count; i++) {
      const char* display = completions_get_display(env->completions, i, NULL);
      if (display != NULL) {
        bbcode_println(env->bbcode, display);
      }
    }
    if (count >= IC_MAX_COMPLETIONS_TO_SHOW) {
      bbcode_println(env->bbcode, "[ic-info]... and more.[/]");
    }
    else {
      bbcode_printf(env->bbcode, "[ic-info](%zd possible completions)[/]\n", count );
    }
    for(ssize_t i = 0; i < rc.row+1; i++) {
      term_write(env->term, " \n");
    }
    eb->cur_rows = 0;
    edit_refresh(env,eb);      
  }
  else {
    edit_refresh(env,eb);
  }
  // done
  completions_clear(env->completions);      
  if (c != 0) tty_code_pushback(env->tty,c);
}

static void edit_generate_completions(ic_env_t* env, editor_t* eb, bool autotab) {
  debug_msg( "edit: complete: %zd: %s\n", eb->pos, sbuf_string(eb->input) );
  if (eb->pos < 0) return;
  ssize_t count = completions_generate(env, env->completions, sbuf_string(eb->input), eb->pos, IC_MAX_COMPLETIONS_TO_TRY);
  bool more_available = (count >= IC_MAX_COMPLETIONS_TO_TRY);
  if (count <= 0) {
    // no completions
    if (!autotab) { term_beep(env->term); }
  }
  else if (count == 1) {
    // complete if only one match    
    if (edit_complete(env,eb,0 /*idx*/) && env->complete_autotab) {
      tty_code_pushback(env->tty,KEY_EVENT_AUTOTAB);
    }    
  }
  else {
    //term_beep(env->term); 
    if (!more_available) { 
      edit_complete_longest_prefix(env,eb);
    }    
    completions_sort(env->completions);
    edit_completion_menu( env, eb, more_available);    
  }
}
