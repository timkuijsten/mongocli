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

#include "jsonify.h"
#include "shorten.h"
#include "prefix_match.h"

#include <bson.h>
#include <bcon.h>
#include <mongoc.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <histedit.h>
#include <libgen.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#define MAXLINE 1024
#define MAXUSERNAME 100

#define MAXMONGOURL 200
#define MAXDBNAME 200
#define MAXCOLLNAME 200

#define MAXPROMPT 30  /* must support at least 1 + 4 + 1 + 4 + 2 = 12 characters
                         for the minimally shortened version of a prompt.
                         if MAXPROMPT = 12 then "/dbname/collname> " would
                         become "/d..e/c..e> " */
#define MAXPROG 10
#define MAXDOC 16 * 10 * 1024      /* maximum size of a json document */

static char progname[MAXPROG];

/* shell specific user info */
typedef struct {
  char name[MAXUSERNAME];
  char home[PATH_MAX];
} user_t;

typedef struct {
  char dbname[MAXDBNAME];
  char collname[MAXCOLLNAME];
} path_t;

/* mongo specific db info */
typedef struct {
  char url[MAXMONGOURL];
} config_t;

static path_t path;

enum cmd { ILLEGAL = -1, UNKNOWN, AMBIGUOUS, LSDBS, LSCOLLS, CHCOLL, COUNT, UPDATE, INSERT, REMOVE, FIND, AGQUERY, HELP };
enum errors { DBMISSING = 256, COLLMISSING };

#define NCMDS (sizeof cmds / sizeof cmds[0])

const char *cmds[] = {
  "databases",    /* LSDBS,   list all databases */
  "collections",  /* LSCOLLS, list all collections */
  "cd",           /* CHCOLL,  change database and/or collection */
  "count",        /* COUNT */
  "update",       /* UPDATE */
  "insert",       /* INSERT */
  "remove",       /* REMOVE */
  "find",         /* FIND */
  "aggregate",    /* AGQUERY */
  "help",         /* print usage */
  NULL            /* nul terminate this list */
};

static user_t user;
static config_t config;
static char **list_match = NULL; /* contains all ambiguous prefix_match commands */

static char pmpt[MAXPROMPT + 1] = "> ";
char *prompt();
int init_user(user_t *usr);
int set_prompt(const char *dbname, const char *collname);
int read_config(user_t *usr, config_t *cfg);
int idtosel(char *doc, const size_t docsize, const char *sel, const size_t sellen);
long parse_selector(char *doc, size_t docsize, const char *line, int len);
int parse_path(const char *path, path_t *newpath);
int parse_file(FILE *fp, char *line, config_t *cfg);
int parse_cmd(int argc, const char *argv[], const char *line, char **lp);
int exec_cmd(const int cmd, const char **argv, const char *line, int linelen);
int exec_lsdbs(mongoc_client_t *client);
int exec_lscolls(mongoc_client_t *client, char *dbname);
int exec_chcoll(mongoc_client_t *client, const path_t newpath);
int exec_count(mongoc_collection_t *collection, const char *line, int len);
int exec_update(mongoc_collection_t *collection, const char *line);
int exec_insert(mongoc_collection_t *collection, const char *line, int len);
int exec_remove(mongoc_collection_t *collection, const char *line, int len);
int exec_query(mongoc_collection_t *collection, const char *line, int len);
int exec_agquery(mongoc_collection_t *collection, const char *line, int len);

static mongoc_client_t *client;
static mongoc_collection_t *ccoll = NULL; // current collection

void usage(void)
{
  printf("usage: %s [-ps] [/database/collection]\n", progname);
  exit(0);
}

int pretty = 0;

int main(int argc, char **argv)
{
  const char *line, **av;
  char linecpy[MAXLINE], *lp;
  int i, read, status, ac, cc, co, cmd, ch;
  EditLine *e;
  History *h;
  HistEvent he;
  Tokenizer *t;
  path_t newpath = { "", "" };

  char connect_url[MAXMONGOURL] = "mongodb://localhost:27017";

  if (strlcpy(progname, basename(argv[0]), MAXPROG) > MAXPROG)
    errx(1, "program name too long");

  /* default ttys to pretty print */
  if (isatty(STDIN_FILENO))
    pretty = 1;

  while ((ch = getopt(argc, argv, "ps")) != -1)
    switch (ch) {
    case 'p':
      pretty = 1;
      break;
    case 's':
      pretty = 0;
      break;
    case '?':
      usage();
      break;
    }
  argc -= optind;
  argv += optind;

  if (argc > 1)
    usage();

  if (PATH_MAX < 20)
    errx(1, "can't determine PATH_MAX");

  if (init_user(&user) < 0)
    errx(1, "can't initialize user");

  if ((status = read_config(&user, &config)) < 0)
    errx(1, "can't read config file");
  else if (status > 0)
    if (strlcpy(connect_url, config.url, MAXMONGOURL) > MAXMONGOURL)
      errx(1, "url in config too long");
  // else use default

  if ((e = el_init(progname, stdin, stdout, stderr)) == NULL)
    errx(1, "can't initialize editline");
  if ((h = history_init()) == NULL)
    errx(1, "can't initialize history");
  t = tok_init(NULL);

  history(h, &he, H_SETSIZE, 100);
  el_set(e, EL_HIST, history, h);

  el_set(e, EL_PROMPT, prompt);
  el_set(e, EL_EDITOR, "emacs");
  el_set(e, EL_TERMINAL, NULL);
  el_source(e, NULL);

  // setup mongo
  mongoc_init();
  if ((client = mongoc_client_new(connect_url)) == NULL)
    errx(1, "can't connect to mongo");

  if (argc == 1) {
    if (parse_path(argv[0], &newpath) < 0)
      errx(1, "illegal path spec");
    if (exec_chcoll(client, newpath) < 0)
      errx(1, "can't change database or collection");
  }

  while ((line = el_gets(e, &read)) != NULL) {
    if (read > MAXLINE)
      errx(1, "line too long");

    if (line[read - 1] != '\n')
      errx(1, "expected line to end with a newline");

    // tokenize
    tok_reset(t);
    tok_line(t, el_line(e), &ac, &av, &cc, &co);

    if (ac == 0)
      continue;

    // copy without newline
    if (strlcpy(linecpy, line, read) > (size_t)read)
      errx(1, "could not copy line");

    if (history(h, &he, H_ENTER, linecpy) == -1)
      errx(1, "can't enter history");

    cmd = parse_cmd(ac, av, linecpy, &lp);
    switch (cmd) {
    case ILLEGAL:
      warnx("illegal syntax");
      continue;
      break;
    case UNKNOWN:
      warnx("unknown command");
      continue;
      break;
    case AMBIGUOUS:
      // matches more than one command, print list_match
      i = 0;
      while (list_match[i] != NULL)
        printf("%s\n", list_match[i++]);
      continue;
      break;
    case HELP:
      i = 0;
      while (cmds[i] != NULL)
        printf("%s\n", cmds[i++]);
      continue;
      break;
    case DBMISSING:
      warnx("no database selected");
      continue;
      break;
    case COLLMISSING:
      warnx("no collection selected");
      continue;
      break;
    }

    if (exec_cmd(cmd, av, lp, strlen(lp)) == -1)
      warnx("execution failed");
  }

  if (read == -1)
    err(1, NULL);

  if (ccoll != NULL)
    mongoc_collection_destroy(ccoll);
  mongoc_client_destroy(client);
  mongoc_cleanup();

  tok_end(t);
  history_end(h);
  el_end(e);

  free(list_match);

  if (isatty(STDIN_FILENO))
    printf("\n");

  return 0;
}

/*
 * Create a mongo extended JSON id selector document. If selector is 24 hex
 * digits treat it as an object id, otherwise as a literal.
 *
 * doc     - resulting json doc is set in doc
 * dosize  - the size of doc
 * sel     - selector, does not have to be NUL terminated
 * sellen  - length of sel, excluding a terminating NUL character, if any
 *
 * Return 0 on success or -1 on error.
 */
int idtosel(char *doc, const size_t docsize, const char *sel, const size_t sellen)
{
  char *idtpls = "{ \"_id\": \"";
  char *idtple = "\" }";
  char *oidtpls = "{ \"_id\": { \"$oid\": \"";
  char *oidtple = "\" } }";
  char *start, *end;

  if (docsize < 1)
    return -1;
  if (sellen < 1)
    return -1;

  /* if 24 hex chars, assume an object id */
  if (sellen == 24 && (strspn(sel, "0123456789abcdefABCDEF") == 24)) {
    start = oidtpls;
    end = oidtple;
  } else {
    /* otherwise treat as a literal */
    start = idtpls;
    end = idtple;
  }

  if (strlen(start) + sellen + strlen(end) + 1 > docsize)
    return -1;

  if (strlcpy(doc, start, docsize) > docsize)
    return -1;
  strncat(doc, sel, sellen);
  doc[strlen(start) + sellen] = '\0'; // ensure NUL termination
  if (strlcat(doc, end, docsize) > docsize)
    return -1;

  return 0;
}

/*
 * parse json docs or id only specifications
 * return size of parsed length on success or -1 on failure.
 */
long parse_selector(char *doc, size_t docsize, const char *line, int len)
{
  long offset;

  /* support id only selectors */
  const char *ids; /* id start */
  size_t fnb, snb; /* first and second non-blank characters used for id selection */

  offset = 0;

  /* if first non-blank char is not a "{", use it as a literal and convert to an
     id selector */
  fnb = strspn(line, " \t");
  if (line[fnb] != '{') {
    ids = line + fnb; /* id start */
    snb = strcspn(ids, " \t"); /* id end */

    idtosel(doc, docsize, ids, snb);
    offset = fnb + snb;
  } else {
    // try to parse as relaxed json and convert to strict json
    if ((offset = relaxed_to_strict(doc, docsize, line, len, 0)) < 0) {
      warnx("jsonify error: %ld", offset);
      return -1;
    }
  }

  return offset;
}

/*
 * Parse path that consists of a database name and or a collection name. Support
 * both absolute and relative paths.
 * Relative depends on wheter or not a db and collection are set in newpath.
 * Absolute always starts with a / followed by a database name.
 * path must be null terminated.
 * return 0 on success, -1 on failure.
 */
int
parse_path(const char *path, path_t *newpath)
{
  int i, ac;
  const char **av;
  Tokenizer *t;

  if (!strlen(path))
    return 0;

  t = tok_init("/");
  tok_str(t, path, &ac, &av);

  /* check if this is an absolute or a relative path */
  if (path[0] == '/') {
    /* absolute */
    /* reset db and collection selection */
    if (strlcpy(newpath->dbname, "", MAXDBNAME) > MAXDBNAME)
      goto cleanupexit;
    if (strlcpy(newpath->collname, "", MAXCOLLNAME) > MAXCOLLNAME)
      goto cleanupexit;

    if (ac > 0) {
      /* use first component as the name of the database */
      if ((i = strlcpy(newpath->dbname, av[0], MAXDBNAME)) > MAXDBNAME)
        goto cleanupexit;

      /* use everything after the first component as the name of the collection */
      if (ac > 1) {
        /* skip db name and it's leading and trailing slash */
        if ((i = strlcpy(newpath->collname, (char *)path + 1 + i + 1, MAXCOLLNAME)) > MAXCOLLNAME)
          goto cleanupexit;
      }
    }
  } else {
    // relative
    if (strlen(newpath->collname) || strlen(newpath->dbname)) {
      /* use whole path as the name of the new collection */
      if ((i = strlcpy(newpath->collname, path, MAXCOLLNAME)) > MAXCOLLNAME)
        goto cleanupexit;
    } else {
      /* no current dbname or collname set, use first component as the name of the database */
      if ((i = strlcpy(newpath->dbname, av[0], MAXDBNAME)) > MAXDBNAME)
        goto cleanupexit;

      /* use everything after the first component as the name of the collection */
      if (ac > 1) {
        /* skip db name and it's leading and trailing slash */
        if ((i = strlcpy(newpath->collname, (char *)path + i + 1, MAXCOLLNAME)) > MAXCOLLNAME)
          goto cleanupexit;
      }
    }
  }

  tok_end(t);
  return 0;

cleanupexit:
  tok_end(t);
  return -1;
}

// return command code
int parse_cmd(int argc, const char *argv[], const char *line, char **lp)
{
  const char *cmd;

  // check if the first token matches one or more commands
  if (prefix_match(&list_match, cmds, argv[0]) == -1)
    errx(1, "prefix_match error");

  // unknown prefix
  if (list_match[0] == NULL) {
    return UNKNOWN;
  }

  // matches more than one command
  if (list_match[1] != NULL) {
    return AMBIGUOUS;
  }

  // matches exactly one command from cmds
  cmd = list_match[0];

  if (strcmp("databases", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    switch (argc) {
    case 1:
      return LSDBS;
    default:
      return ILLEGAL;
    }
  } else if (strcmp("collections", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    switch (argc) {
    case 1:
      return LSCOLLS;
    default:
      return ILLEGAL;
    }
  } else if (strcmp("cd", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    switch (argc) {
    case 2:
      return CHCOLL;
    default:
      return ILLEGAL;
    }
  } else if (strcmp("help", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    return HELP;
  }

  /* all the other commands need a database and collection
     to be selected */

  if (!strlen(path.dbname))
    return DBMISSING;
  if (!strlen(path.collname))
    return COLLMISSING;

  if (strcmp("count", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    return COUNT;
  } else if (strcmp("update", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    return UPDATE;
  } else if (strcmp("insert", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    return INSERT;
  } else if (strcmp("remove", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    return REMOVE;
  } else if (strcmp("find", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    return FIND;
  } else if (strcmp("aggregate", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    return AGQUERY;
  }

  return UNKNOWN;
}

// execute command with given arguments
// return 0 on success, -1 on failure
int exec_cmd(const int cmd, const char **argv, const char *line, int linelen)
{
  path_t tmppath;

  switch (cmd) {
  case LSDBS:
    return exec_lsdbs(client);
  case ILLEGAL:
    break;
  case LSCOLLS:
    return exec_lscolls(client, path.dbname);
  case CHCOLL:
    if (strlcpy(tmppath.dbname, path.dbname, MAXDBNAME) > MAXDBNAME)
      return -1;
    if (strlcpy(tmppath.collname, path.collname, MAXCOLLNAME) > MAXCOLLNAME)
      return -1;
    if (parse_path(argv[1], &tmppath) < 0)
      return -1;
    return exec_chcoll(client, tmppath);
  case COUNT:
    return exec_count(ccoll, line, linelen);
  case UPDATE:
    return exec_update(ccoll, line);
  case INSERT:
    return exec_insert(ccoll, line, linelen);
  case REMOVE:
    return exec_remove(ccoll, line, linelen);
  case FIND:
    return exec_query(ccoll, line, linelen);
  case AGQUERY:
    return exec_agquery(ccoll, line, linelen);
  }

  return -1;
}

// list database for the given client
// return 0 on success, -1 on failure
int exec_lsdbs(mongoc_client_t *client)
{
  bson_error_t error;
  char **strv;
  int i;

  if ((strv = mongoc_client_get_database_names(client, &error)) == NULL)
    return -1;

  for (i = 0; strv[i]; i++)
    printf("%s\n", strv[i]);

  bson_strfreev(strv);

  return 0;
}

// list collections for the given database
// return 0 on success, -1 on failure
int exec_lscolls(mongoc_client_t *client, char *dbname)
{
  bson_error_t error;
  mongoc_database_t *db;
  char **strv;
  int i;

  if (!strlen(dbname))
    return -1;

  db = mongoc_client_get_database(client, dbname);

  if ((strv = mongoc_database_get_collection_names(db, &error)) == NULL)
    return -1;

  for (i = 0; strv[i]; i++)
    printf("%s\n", strv[i]);

  bson_strfreev(strv);
  mongoc_database_destroy(db);

  return 0;
}

/*
 * change dbname and/or collname, set ccoll and update prompt.
 * return 0 on success, -1 on failure
 */
int
exec_chcoll(mongoc_client_t *client, const path_t newpath)
{
  /* unset current collection */
  if (ccoll != NULL) {
    mongoc_collection_destroy(ccoll);
    ccoll = NULL;
  }

  /* if there is a new collection, change to it */
  if (strlen(newpath.dbname) && strlen(newpath.dbname))
    ccoll = mongoc_client_get_collection(client, newpath.dbname, newpath.collname);

  /* update prompt to show whatever we've changed to */
  set_prompt(newpath.dbname, newpath.collname);

  /* update global references */
  if (strlcpy(path.dbname, newpath.dbname, MAXDBNAME) > MAXDBNAME)
    return -1;
  if (strlcpy(path.collname, newpath.collname, MAXCOLLNAME) > MAXCOLLNAME)
    return -1;

  return 0;
}

// count number of documents in collection
// return 0 on success, -1 on failure
int exec_count(mongoc_collection_t *collection, const char *line, int len)
{
  bson_error_t error;
  int64_t count;
  bson_t query;
  char query_doc[MAXDOC] = "{}"; /* default to all documents */

  if (parse_selector(query_doc, MAXDOC, line, len) == -1)
    return -1;

  // try to parse it as json and convert to bson
  if (!bson_init_from_json(&query, query_doc, -1, &error)) {
    warnx("%s", error.message);
    return -1;
  }

  if ((count = mongoc_collection_count(collection, MONGOC_QUERY_NONE, &query, 0, 0, NULL, &error)) == -1)
    warnx("cursor failed: %s", error.message);

  printf("%lld\n", count);

  return 0;
}

// parse update command, expect two json objects, a selector, and an update doc and exec
int exec_update(mongoc_collection_t *collection, const char *line)
{
  long offset;
  char query_doc[MAXDOC];
  char update_doc[MAXDOC];
  bson_error_t error;
  bson_t query, update;

  // read first json object
  if ((offset = parse_selector(query_doc, MAXDOC, line, strlen(line))) == -1)
    return ILLEGAL;
  if (offset == 0)
    return ILLEGAL;

  // shorten line
  line += offset;

  // read second json object
  if ((offset = relaxed_to_strict(update_doc, MAXDOC, line, strlen(line), 1)) < 0) {
    warnx("jsonify error: %ld", offset);
    return ILLEGAL;
  }
  if (offset == 0)
    return ILLEGAL;

  // shorten line
  line += offset;

  // try to parse the query as json and convert to bson
  if (!bson_init_from_json(&query, query_doc, -1, &error)) {
    warnx("%s", error.message);
    return -1;
  }

  // try to parse the update as json and convert to bson
  if (!bson_init_from_json(&update, update_doc, -1, &error)) {
    warnx("%s", error.message);
    return -1;
  }

  // execute update
  if (!mongoc_collection_update(collection, MONGOC_UPDATE_MULTI_UPDATE, &query, &update, NULL, &error)) {
    warnx("%s", error.message);
    return -1;
  }

  return 0;
}

// parse insert command, expect one json objects, the insert doc and exec
int exec_insert(mongoc_collection_t *collection, const char *line, int len)
{
  long offset;
  char insert_doc[MAXDOC];
  bson_error_t error;
  bson_t doc;

  // read first json object
  if ((offset = parse_selector(insert_doc, MAXDOC, line, len)) == -1)
    return ILLEGAL;
  if (offset == 0)
    return ILLEGAL;

  // try to parse the doc as json and convert to bson
  if (!bson_init_from_json(&doc, insert_doc, -1, &error)) {
    warnx("%s", error.message);
    return -1;
  }

  // execute insert
  if (!mongoc_collection_insert(collection, MONGOC_INSERT_NONE, &doc, NULL, &error)) {
    warnx("%s", error.message);
    return -1;
  }

  return 0;
}

// parse remove command, expect one selector
int exec_remove(mongoc_collection_t *collection, const char *line, int len)
{
  long offset;
  char remove_doc[MAXDOC];
  bson_error_t error;
  bson_t doc;

  // read first json object
  if ((offset = parse_selector(remove_doc, MAXDOC, line, len)) == -1)
    return ILLEGAL;
  if (offset == 0)
    return ILLEGAL;

  // try to parse the doc as json and convert to bson
  if (!bson_init_from_json(&doc, remove_doc, -1, &error)) {
    warnx("%s", error.message);
    return -1;
  }

  // execute remove
  if (!mongoc_collection_remove(collection, MONGOC_REMOVE_NONE, &doc, NULL, &error)) {
    warnx("%s", error.message);
    return -1;
  }

  return 0;
}

// execute a query
// return 0 on success, -1 on failure
int exec_query(mongoc_collection_t *collection, const char *line, int len)
{
  long i;
  mongoc_cursor_t *cursor;
  bson_error_t error;
  size_t rlen;
  const bson_t *doc;
  char *str;
  bson_t query;
  char query_doc[MAXDOC] = "{}"; /* default to all documents */
  struct winsize w;

  if (parse_selector(query_doc, MAXDOC, line, len) == -1)
    return -1;

  // try to parse it as json and convert to bson
  if (!bson_init_from_json(&query, query_doc, -1, &error)) {
    warnx("%s", error.message);
    return -1;
  }

  cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &query, NULL, NULL);

  ioctl(0, TIOCGWINSZ, &w);

  while (mongoc_cursor_next(cursor, &doc)) {
    str = bson_as_json(doc, &rlen);
    if (pretty && rlen > w.ws_col) {
      if ((i = human_readable(query_doc, MAXDOC, str, rlen)) < 0) {
        warnx("jsonify error: %ld", i);
        return -1;
      }
      printf ("%s\n", query_doc);
    } else {
      printf ("%s\n", str);
    }
    bson_free(str);
  }

  if (mongoc_cursor_error(cursor, &error)) {
    warnx("cursor failed: %s", error.message);
    mongoc_cursor_destroy(cursor);
    return -1;
  }

  mongoc_cursor_destroy(cursor);

  return 0;
}

// execute an aggregation pipeline
// return 0 on success, -1 on failure
int exec_agquery(mongoc_collection_t *collection, const char *line, int len)
{
  long i;
  mongoc_cursor_t *cursor;
  bson_error_t error;
  const bson_t *doc;
  char *str;
  bson_t aggr_query;
  char query_doc[MAXDOC];

  // try to parse as relaxed json and convert to strict json
  if ((i = relaxed_to_strict(query_doc, MAXDOC, line, len, 0)) < 0) {
    warnx("jsonify error: %ld", i);
    return -1;
  }

  // try to parse it as json and convert to bson
  if (!bson_init_from_json(&aggr_query, query_doc, -1, &error)) {
    warnx("%s", error.message);
    return -1;
  }

  cursor = mongoc_collection_aggregate(collection, MONGOC_QUERY_NONE, &aggr_query, NULL, NULL);

  while (mongoc_cursor_next(cursor, &doc)) {
    str = bson_as_json(doc, NULL);
    printf ("%s\n", str);
    bson_free(str);
  }

  if (mongoc_cursor_error(cursor, &error)) {
    warnx("cursor failed: %s", error.message);
    mongoc_cursor_destroy(cursor);
    return -1;
  }

  mongoc_cursor_destroy(cursor);

  return 0;
}

char *prompt()
{
  return pmpt;
}

// if too long, shorten first or both components
// global pmpt should have space for MAXPROMPT + 1 bytes
int
set_prompt(const char *dbname, const char *collname)
{
  const int static_chars = 4; /* prompt is of the form "/d/c> " */
  char c1[MAXPROMPT + 1], c2[MAXPROMPT + 1];
  int plen;

  if (strlcpy(c1, dbname, MAXPROMPT) > MAXPROMPT)
    return -1;
  if (strlcpy(c2, collname, MAXPROMPT) > MAXPROMPT)
    return -1;

  plen = static_chars + strlen(c1) + strlen(c2);

  // ensure prompt fits
  if (plen - MAXPROMPT > 0)
    if (shorten_comps(c1, c2, MAXPROMPT - static_chars) < 0)
      errx(1, "can't initialize prompt");

  snprintf(pmpt, MAXPROMPT + 1, "/%s/%s> ", c1, c2);
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
    if (errno == ENOENT)
      return 0;

    return -1;
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
