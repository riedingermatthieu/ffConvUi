/*
 * filter_chain.c - a stream's filter chain as the user edits it.
 */
#include "filter_chain.h"

#include <string.h>

#include <libavutil/bprint.h>
#include <libavutil/mem.h>

struct FilterChain {
    int                refs;
    GPtrArray         *entries;   /* FilterEntry* */
    char              *text;      /* custom filtergraph, or NULL */
    FilterChainChanged changed;
    void              *user;
    gpointer           dialog;
};

static void entry_free(gpointer data)
{
    FilterEntry *e = data;

    av_dict_free(&e->options);
    g_free(e);
}

FilterChain *filter_chain_new(void)
{
    FilterChain *c = g_new0(FilterChain, 1);

    c->refs    = 1;
    c->entries = g_ptr_array_new_with_free_func(entry_free);
    return c;
}

FilterChain *filter_chain_ref(FilterChain *c)
{
    c->refs++;
    return c;
}

void filter_chain_unref(FilterChain *c)
{
    if (!c || --c->refs > 0)
        return;
    g_ptr_array_free(c->entries, TRUE);
    g_free(c->text);
    g_free(c);
}

void filter_chain_set_listener(FilterChain *c, FilterChainChanged cb, void *user)
{
    c->changed = cb;
    c->user    = user;
}

void filter_chain_changed(FilterChain *c)
{
    if (c->changed)
        c->changed(c->user);
}

int filter_chain_length(const FilterChain *c)
{
    return c->entries->len;
}

FilterEntry *filter_chain_get(FilterChain *c, int i)
{
    return i >= 0 && i < (int)c->entries->len ? g_ptr_array_index(c->entries, i) : NULL;
}

FilterEntry *filter_chain_append(FilterChain *c, const AVFilter *f)
{
    FilterEntry *e = g_new0(FilterEntry, 1);

    e->filter  = f;
    e->enabled = TRUE;
    g_ptr_array_add(c->entries, e);
    return e;
}

void filter_chain_remove(FilterChain *c, int i)
{
    if (i >= 0 && i < (int)c->entries->len)
        g_ptr_array_remove_index(c->entries, i);
}

void filter_chain_move(FilterChain *c, int from, int to)
{
    gpointer e;

    if (from < 0 || from >= (int)c->entries->len || to < 0 || to >= (int)c->entries->len || from == to)
        return;
    e = g_ptr_array_steal_index(c->entries, from);
    g_ptr_array_insert(c->entries, to, e);
}

const char *filter_chain_text(const FilterChain *c)
{
    return c->text;
}

void filter_chain_set_text(FilterChain *c, const char *text)
{
    g_free(c->text);
    c->text = g_strdup(text);
}

int filter_chain_active(const FilterChain *c)
{
    int n = 0;

    if (c->text)
        return *c->text ? 1 : 0;
    for (guint i = 0; i < c->entries->len; i++)
        n += ((FilterEntry *)g_ptr_array_index(c->entries, i))->enabled;
    return n;
}

int filter_chain_job_index(const FilterChain *c, int i)
{
    int n = 0;

    if (i < 0 || i >= (int)c->entries->len || !((FilterEntry *)g_ptr_array_index(c->entries, i))->enabled)
        return -1;
    for (int k = 0; k < i; k++)
        n += ((FilterEntry *)g_ptr_array_index(c->entries, k))->enabled;
    return n;
}

int filter_chain_entry_index(const FilterChain *c, int job_index)
{
    for (guint i = 0; i < c->entries->len; i++)
        if (((FilterEntry *)g_ptr_array_index(c->entries, i))->enabled && job_index-- == 0)
            return i;
    return -1;
}

int filter_chain_to_job(const FilterChain *c, JobStream *js)
{
    if (c->text) {
        if (*c->text && !(js->filter_string = av_strdup(c->text)))
            return AVERROR(ENOMEM);
        return 0;
    }
    for (guint i = 0; i < c->entries->len; i++) {
        const FilterEntry *e = g_ptr_array_index(c->entries, i);
        JobFilter *jf;

        if (!e->enabled)
            continue;
        if (!(jf = job_stream_add_filter(js, e->filter->name)) ||
            av_dict_copy(&jf->options, e->options, 0) < 0)
            return AVERROR(ENOMEM);
    }
    return 0;
}

char *filter_chain_string(const FilterChain *c)
{
    ConvJob *job = job_alloc();
    JobStream *js = job ? job_add_stream(job, 0, JOB_TRANSCODE) : NULL;
    AVBPrint bp;
    char *s;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    if (js && filter_chain_to_job(c, js) >= 0)
        job_stream_filter_string(js, &bp);
    s = g_strdup(bp.str);
    av_bprint_finalize(&bp, NULL);
    job_free(&job);
    return s;
}

char *filter_entry_summary(const FilterEntry *e)
{
    GString *g = g_string_new(NULL);
    const AVDictionaryEntry *d = NULL;

    while ((d = av_dict_iterate(e->options, d)))
        g_string_append_printf(g, "%s%s=%s", g->len ? ":" : "", d->key, d->value);
    return g_string_free(g, FALSE);
}

void filter_chain_set_dialog(FilterChain *c, gpointer window)
{
    c->dialog = window;
}

gpointer filter_chain_get_dialog(const FilterChain *c)
{
    return c->dialog;
}
