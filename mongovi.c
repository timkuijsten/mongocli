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
#include <fcntl.h>
#include <unistd.h>
#include <histedit.h>
#include <libgen.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bson.h>
#include <bcon.h>
#include <mongoc.h>

#include "common.h"
#include "jsonify.h"

#define MAXLINE 1024
#define MAXUSERNAME 100

#define MAXMONGOURL 200

#define MAXPROMPT 30  // must support at least 1 + 4 + 1 + 4 + 3 = 13 characters for the shortened version of a prompt:
                      // "/dbname/collname > " would become "/d..e/c..e > " if MAXPROMPT = 13
#define MAXPROG 10
#define MAXQUERY 16 * 1024

static char *progname;

static char *dbname;
static char *collname;

/* shell specific user info */
typedef struct {
  char name[MAXUSERNAME];
  char home[PATH_MAX];
} user_t;

/* mongo specific db info */
typedef struct {
  char url[MAXMONGOURL];
} config_t;

static user_t user;
static config_t config;

static char p[MAXPROMPT + 1];
void exec_pipeline(mongoc_collection_t *collection, bson_t *pipeline);
char *prompt(EditLine *e);
int init_user(user_t *usr);
int set_prompt(char *dbname, char *collname);
int read_config(user_t *usr, config_t *cfg);
int parse_file(FILE *fp, char *line, config_t *cfg);

void usage(void)
{
  printf("usage: %s database collection\n", progname);
  exit(0);
}

int main(int argc, char **argv)
{
  const char *line;
  int on, read, len, status;
  EditLine *e;
  History *h;
  HistEvent he;

  mongoc_client_t *client;
  mongoc_collection_t *collection;
  bson_t aggr_query;
  bson_error_t error;

  char query_doc[MAXQUERY];
  char connect_url[MAXMONGOURL] = "mongodb://localhost:27017";

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

  if (PATH_MAX < 20)
    fatal("can't determine PATH_MAX");

  if (init_user(&user) < 0)
    fatal("can't initialize user");

  if ((status = read_config(&user, &config)) < 0)
    fatal("can't read config file");
  else if (status > 0)
    if (strlcpy(connect_url, config.url, MAXMONGOURL) > MAXMONGOURL)
      fatal("url in config too long");
  // else use default

  set_prompt(dbname, collname);

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
  if ((client = mongoc_client_new(connect_url)) == NULL)
    fatal("can't connect to mongo");

  collection = mongoc_client_get_collection(client, dbname, collname);

  while ((line = el_gets(e, &read)) != NULL) {
    len = strlen(line);
    if (len == 1) // skip lines with a newline only
      continue;

    if (history(h, &he, H_ENTER, line) == -1)
      fatal("can't enter history");

    // try to parse as loose json and convert to strict json
    if (from_loose_to_strict(query_doc, MAXQUERY, (char *)line, len - 1) == -1)
      fatal("jsonify error");

    // try to parse it as json and convert to bson
    if (!bson_init_from_json(&aggr_query, query_doc, -1, &error))
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

  printf("\n");
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

// if too long, shorten first or both components
// global p should have space for MAXPROMPT + 1 bytes
int
set_prompt(char *dbname, char *collname)
{
  const int static_chars = 5; // prompt is of the form "/d/c > "
  char c1[MAXPROMPT + 1], c2[MAXPROMPT + 1];
  int plen, i;

  strlcpy(c1, dbname, MAXPROMPT);
  strlcpy(c2, collname, MAXPROMPT);

  plen = static_chars + strlen(c1) + strlen(c2);

  // ensure prompt fits
  if (plen - MAXPROMPT > 0)
    if (shorten_comps(c1, c2, MAXPROMPT - static_chars) < 0)
      fatal("can't initialize prompt");

  snprintf(p, MAXPROMPT + 1, "/%s/%s > ", c1, c2);
  return 0;
}

// set username and home dir
// return 0 on success or -1 on failure.
int
init_user(user_t *usr)
{
  struct passwd *pw;

  if ((pw = getpwuid(getuid())) == NULL)
    return -1; // user not found
  if (strlcpy(usr->name, pw->pw_name, MAXUSERNAME) >= MAXUSERNAME)
    return -1; // username truncated
  if (strlcpy(usr->home, pw->pw_dir, PATH_MAX) >= PATH_MAX)
    return -1; // home dir truncated

  return 0;
}

// try to read ~/.mongovi and set cfg
// return 1 if config is read and set, 0 if no config is found or -1 on failure.
int
read_config(user_t *usr, config_t *cfg)
{
  const char *file = ".mongovi";
  char tmppath[PATH_MAX + 1], *line;
  FILE *fp;

  line = NULL;

  if (strlcpy(tmppath, usr->home, PATH_MAX) >= PATH_MAX)
    return -1;
  if (strlcat(tmppath, "/", PATH_MAX) >= PATH_MAX)
    return -1;
  if (strlcat(tmppath, file, PATH_MAX) >= PATH_MAX)
    return -1;

  if ((fp = fopen(tmppath, "re")) == NULL) {
    if (errno != ENOENT) {
      printf("errno %d\n", errno);
      ferrno(strerror(errno));
      return -1;
    } else {
      return 0;
    }
  }

  if (parse_file(fp, line, cfg) < 0) {
    if (line != NULL)
      free(line);
    fclose(fp);
    return -1;
  }

  free(line);
  fclose(fp);
  return 1;
}

// read the credentials from a users config file
// return 0 on success or -1 on failure.
int
parse_file(FILE *fp, char *line, config_t *cfg)
{
  size_t linesize = 0;
  ssize_t linelen = 0;

  // expect url on first line
  if ((linelen = getline(&line, &linesize, fp)) < 0)
    return -1;
  if (linelen > MAXMONGOURL)
    return -1;
  if (strlcpy(cfg->url, line, MAXMONGOURL) >= MAXMONGOURL)
    return -1;
  cfg->url[linelen - 1] = '\0'; // trim newline

  return 0;
}
