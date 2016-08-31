/**
 * Copyright (c) 2016 Tim Kuijsten
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <histedit.h>

#include <bson.h>
#include <bcon.h>
#include <mongoc.h>

#define MAXLINE 1024
#define MAXPROMPT 30
#define MAXPROG 10

static char *progname;
static char *dbname;
static char *collname;

static char p[MAXPROMPT];
void exec_pipeline(mongoc_collection_t *collection, bson_t *pipeline);
char *prompt(EditLine *e);
void ferrno(const char *err);
void fatal(const char *err);

void usage(void)
{
  printf("usage: %s database collection\n", progname);
  exit(0);
}

int main(int argc, char **argv)
{
  const char *line;
  int on, read;
  EditLine *e;
  History *h;
  HistEvent he;

  mongoc_client_t *client;
  mongoc_collection_t *collection;
  bson_t aggr_query;
  bson_error_t error;

  progname = basename(argv[0]);

  if (argc != 3)
    usage();

  while (--argc)
    switch (argc) {
    case 1:
      dbname = argv[argc];
      break;
    case 2:
      collname = argv[argc];
      break;
    }

  // set prompt
  snprintf(p, MAXPROMPT, "%s.%s > ", dbname, collname);

  if ((h = history_init()) == NULL)
    fatal("can't initialize history");
  if ((e = el_init(argv[0], stdin, stdout, stderr)) == NULL)
    fatal("can't initialize editline");

  el_set(e, EL_HIST, history, h);
  el_set(e, EL_PROMPT, prompt);
  el_get(e, EL_EDITMODE, &on);
  el_source(e, NULL);

  history(h, &he, H_SETSIZE, 100);

  // setup mongo
  mongoc_init();
  client = mongoc_client_new("mongodb://localhost:27017");
  collection = mongoc_client_get_collection(client, dbname, collname);

  while ((line = el_gets(e, &read)) != NULL) {
    if (strlen(line) == 1) // skip newlines
      continue;

    if (history(h, &he, H_ENTER, line) == -1)
      fatal("can't enter history");

    // try to parse it as json and convert to bson
    if (!bson_init_from_json(&aggr_query, line, -1, &error))
      fprintf(stderr, "%s\n", error.message);
    else
      exec_pipeline(collection, &aggr_query);
  }

  if (read == -1)
    ferrno("read line error");

  mongoc_collection_destroy(collection);
  mongoc_client_destroy(client);
  mongoc_cleanup();

  history_end(h);
  el_end(e);

  return 0;
}

void exec_pipeline(mongoc_collection_t *collection, bson_t *pipeline)
{
  mongoc_cursor_t *cursor;
  bson_error_t error;
  const bson_t *doc;
  char *str;

  cursor = mongoc_collection_aggregate (collection, MONGOC_QUERY_NONE, pipeline, NULL, NULL);

  while (mongoc_cursor_next(cursor, &doc)) {
    str = bson_as_json(doc, NULL);
    printf ("%s\n", str);
    bson_free(str);
  }

  if (mongoc_cursor_error(cursor, &error)) {
    fprintf (stderr, "Cursor Failure: %s\n", error.message);
  }

  mongoc_cursor_destroy(cursor);
}

char *prompt(EditLine *e)
{
  return p;
}

void ferrno(const char *err) {
  perror(err);
  exit(1);
}

void fatal(const char *err) {
  fprintf(stderr, "%s\n", err ? err : "");
  exit(1);
}
