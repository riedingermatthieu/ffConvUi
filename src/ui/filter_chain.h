/*
 * filter_chain.h - a stream's filter chain as the user edits it.
 *
 * Either a list of filters (name, the options the user set, on/off), or,
 * once the user edits the filtergraph text directly, that text. The chain
 * outlives the stream row showing it (it is handed over when the rows are
 * rebuilt for another container), so it carries its own change listener:
 * the row currently showing it.
 */
#ifndef UI_FILTER_CHAIN_H
#define UI_FILTER_CHAIN_H

#include <glib.h>
#include <libavfilter/avfilter.h>
#include <libavutil/dict.h>

#include "job.h"

typedef struct FilterEntry {
    const AVFilter *filter;
    AVDictionary   *options;
    gboolean        enabled;
} FilterEntry;

typedef struct FilterChain FilterChain;

typedef void (*FilterChainChanged)(void *user);

FilterChain *filter_chain_new(void);
FilterChain *filter_chain_ref(FilterChain *c);
void         filter_chain_unref(FilterChain *c);

/* The row showing the chain (NULL: nobody). */
void filter_chain_set_listener(FilterChain *c, FilterChainChanged cb, void *user);
/* Tell the listener the chain changed. */
void filter_chain_changed(FilterChain *c);

int          filter_chain_length(const FilterChain *c);
FilterEntry *filter_chain_get(FilterChain *c, int i);
FilterEntry *filter_chain_append(FilterChain *c, const AVFilter *f);
void         filter_chain_remove(FilterChain *c, int i);
void         filter_chain_move(FilterChain *c, int from, int to);

/* Custom text (a filtergraph string) overrides the list; NULL = back to the list. */
const char  *filter_chain_text(const FilterChain *c);
void         filter_chain_set_text(FilterChain *c, const char *text);

/* Filters that take effect: enabled entries, or 1 when there is custom text. */
int          filter_chain_active(const FilterChain *c);

/* Index in the job's filter list of entry i (disabled entries are left
 * out), -1 if it is disabled; and the other way round. */
int          filter_chain_job_index(const FilterChain *c, int i);
int          filter_chain_entry_index(const FilterChain *c, int job_index);

/* Put the chain in a job stream (filters or filter_string). */
int          filter_chain_to_job(const FilterChain *c, JobStream *js);
/* The filtergraph string the job will use ("" if none); g_free it. */
char        *filter_chain_string(const FilterChain *c);

/* "w=640:h=-2" (g_free it). */
char        *filter_entry_summary(const FilterEntry *e);

/* The dialog editing the chain, closed when the chain is dropped. */
void         filter_chain_set_dialog(FilterChain *c, gpointer window);
gpointer     filter_chain_get_dialog(const FilterChain *c);

#endif /* UI_FILTER_CHAIN_H */
