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

#include "mongovi.h"

static char progname[MAXPROG];

static path_t path;

static user_t user;
static config_t config;
static char **list_match = NULL; /* contains all ambiguous prefix_match commands */

static char pmpt[MAXPROMPT + 1] = "/> ";

static mongoc_client_t *client;
static mongoc_collection_t *ccoll = NULL; // current collection

int pretty = 0;

#define NCMDS (sizeof cmds / sizeof cmds[0])
#define MAXCMDNAM (sizeof cmds) /* broadly define maximum length of a command name */

const char *cmds[] = {
  "aggregate",    /* AGQUERY */
  "cd",           /* CHCOLL,  change database and/or collection */
  "collections",  /* LSCOLLS, list all collections */
  "count",        /* COUNT */
  "databases",    /* LSDBS,   list all databases */
  "find",         /* FIND */
  "help",         /* print usage */
  "insert",       /* INSERT */
  "ls",           /* LSARG, LSDBS, LSCOLLS or LSIDS */
  "remove",       /* REMOVE */
  "update",       /* UPDATE */
  "upsert",       /* UPSERT */
  NULL            /* nul terminate this list */
};

void
usage(void)
{
  printf("usage: %s [-ps] [/database/collection]\n", progname);
  exit(0);
}

int
main_init(int argc, char **argv)
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

  /* load user defaults */
  el_source(e, NULL);

  el_set(e, EL_ADDFN, "complete", "Context sensitive argument completion", complete);
  el_set(e, EL_BIND, "\t", "complete", NULL);

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

    if (read == 0)
      goto done; /* happens on Ubuntu 12.04 without tty */

    if (line[read - 1] != '\n')
      errx(1, "expected line to end with a newline");

    // tokenize
    tok_reset(t);
    if (tok_line(t, el_line(e), &ac, &av, &cc, &co) != 0)
      errx(1, "can't tokenize line");

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

 done:
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
 * tab complete command line
 *
 * if empty, print all commands
 * if matches more than one command, print all with matching prefix
 * if matches exactly one command and not complete, complete
 * if command is complete and needs args, look at that
 */
unsigned char
complete(EditLine *e, int ch)
{
  char cmd[MAXCMDNAM];
  const LineInfo *li;
  Tokenizer *t;
  const char **av;
  int i, ret, ac, cc, co;
  size_t cmdlen;

  /* default exit code to error */
  ret = CC_ERROR;

  li = el_line(e);

  /* tokenize */
  t = tok_init(NULL);
  tok_line(t, li, &ac, &av, &cc, &co);

  /* empty, print all commands */
  if (ac == 0) {
    i = 0;
    printf("\n");
    while (cmds[i] != NULL)
      printf("%s\n", cmds[i++]);
    ret = CC_REDISPLAY;
    goto cleanup;
  }

  /* init cmd */
  if (strlcpy(cmd, av[0], MAXCMDNAM) > MAXCMDNAM)
    goto cleanup;

  switch (cc) {
  case 0: /* on command */
    if (complete_cmd(e, cmd, co) < 0)
      goto cleanup;
    cmdlen = strlen(cmd);
    ret = CC_REDISPLAY;
    break;
  case 1: /* on argument, try to complete cd and ls */
    if (strcmp(cmd, "cd") == 0 || strcmp(cmd, "ls") == 0)
      if (complete_path(e, ac <= 1 ? "" : av[1], co) < 0) {
        warnx("complete_path error");
        goto cleanup;
      }
    ret = CC_REDISPLAY;
    goto cleanup;
  default:
    /* ignore subsequent words */
    ret = CC_NORM;
    goto cleanup;
  }

 cleanup:
  tok_end(t);

  return ret;
}

/*
 * tab complete command
 *
 * if matches more than one command, print all
 * if matches exactly one command and not complete, complete
 *
 * return 0 on success or -1 on failure
 */
int
complete_cmd(EditLine *e, const char *tok, int co)
{
  const char *cmd; /* completed command */
  int i;
  size_t cmdlen;

  /* check if cmd matches one or more commands */
  if (prefix_match((const char ***)&list_match, cmds, tok) == -1)
    errx(1, "prefix_match error");

  /* unknown prefix */
  if (list_match[0] == NULL)
    return 0;

  /* matches more than one command, print list_match */
  if (list_match[1] != NULL) {
    i = 0;
    printf("\n");
    while (list_match[i] != NULL)
      printf("%s\n", list_match[i++]);
    return 0;
  }

  /* matches exactly one command from cmds */
  cmd = list_match[0];

  /* complete the command if it's not complete yet
   * but only if the cursor is on a blank */
  cmdlen = strlen(cmd);
  if (cmdlen > strlen(tok)) {
    switch (tok[co]) {
    case ' ':
    case '\0':
    case '\n':
    case '\t':
      if (el_insertstr(e, cmd + strlen(tok)) < 0)
        return -1;
      if (el_insertstr(e, " ") < 0)
        return -1;
      break;
    }
  }

  return 0;
}

/*
 * tab complete path. relative paths depend on current context.
 *
 * if empty, print all possible arguments
 * if matches more than one component, print all with matching prefix and zip up
 * if matches exactly one component and not complete, complete
 *
 * npath is the new path
 * cp is cursor position in npath
 * return 0 on success or -1 on failure
 */
int
complete_path(EditLine *e, const char *npath, int cp)
{
  enum complete { CNONE, CDB, CCOLL };
  path_t tmppath;
  char *c, *found;
  int i;
  bson_error_t error;
  char **strv;
  char **matches = NULL;
  size_t pathlen;
  enum complete compl;
  mongoc_database_t *db;

  if (strlcpy(tmppath.dbname, path.dbname, MAXDBNAME) > MAXDBNAME)
    return -1;
  if (strlcpy(tmppath.collname, path.collname, MAXCOLLNAME) > MAXCOLLNAME)
    return -1;

  if (parse_path(npath, &tmppath) < 0)
    errx(1, "illegal path spec");

  compl = CNONE;

  /* figure out if either the database or the collection has to be completed */
  if (npath[0] == '/') {
    /* complete absolute path
     * complete db, unless another "/" is found and the cursor is on or after
     * it.
     */
    compl = CDB;
    if ((c = strchr(npath + 1, '/')) != NULL) {
      i = c - npath;
      if (i <= cp)
        compl = CCOLL;
    }
  } else {
    /* relative path, check current context */
    if (strlen(path.collname) || strlen(path.dbname)) {
      compl = CCOLL;
    } else {
      /* complete db, unless another "/" is found and the cursor is on or after
       * it.
       */
      compl = CDB;
      if ((c = strchr(npath, '/')) != NULL) {
        i = c - npath;
        if (i <= cp)
          compl = CCOLL;
      }
    }
  }

  switch (compl) {
  case CDB: /* complete database */
    /* if tmppath.dbname is empty, print all databases */
    if (!strlen(tmppath.dbname)) {
      printf("\n");
      return exec_lsdbs(client, NULL);
    }

    /* otherwise get a list of matching prefixes */
    if ((strv = mongoc_client_get_database_names(client, &error)) == NULL)
      errx(1, "%d.%d %s", error.domain, error.code, error.message);

    /* check if this matches one or more entries */
    if (prefix_match((const char ***)&matches, (const char **)strv, tmppath.dbname) == -1)
      errx(1, "prefix_match error");

    /* unknown prefix */
    if (matches[0] == NULL)
      break;

    /* matches more than one entry */
    if (matches[1] != NULL) {
      i = 0;
      printf("\n");
      while (matches[i] != NULL)
        printf("%s\n", matches[i++]);

      /* ensure path is completed to the longest common prefix */
      i = common_prefix((const char **)matches);
      matches[0][i] = 0;
    }

    /* matches exactly one entry or prefix */
    found = matches[0];

    /* complete the entry if it's not complete yet
     * but only if the cursor is on a blank */
    pathlen = strlen(found);
    if (pathlen >= strlen(tmppath.dbname)) {
      switch (npath[cp]) {
      case ' ':
      case '\0':
      case '\n':
      case '\t':
        if (pathlen > strlen(tmppath.dbname)) {
          if (el_insertstr(e, found + strlen(tmppath.dbname)) < 0) {
            free(matches);
            bson_strfreev(strv);
            return -1;
          }
        }
        /* append "/" if exactly one command matched */
        if (matches[1] == NULL) {
          if (el_insertstr(e, "/") < 0) {
            free(matches);
            bson_strfreev(strv);
            return -1;
          }
        }
        break;
      }
    }
    break;
  case CCOLL: /* complete collection */
    /* if tmppath.collname is empty, print all collections */
    if (!strlen(tmppath.collname)) {
      printf("\n");
      return exec_lscolls(client, tmppath.dbname);
    }

    /* otherwise get a list of matching prefixes */
    db = mongoc_client_get_database(client, tmppath.dbname);

    if ((strv = mongoc_database_get_collection_names(db, &error)) == NULL)
      errx(1, "%d.%d %s", error.domain, error.code, error.message);

    mongoc_database_destroy(db);

    /* check if this matches one or more entries */
    if (prefix_match((const char ***)&matches, (const char **)strv, tmppath.collname) == -1)
      errx(1, "prefix_match error");

    /* unknown prefix */
    if (matches[0] == NULL)
      break;

    /* matches more than one entry */
    if (matches[1] != NULL) {
      i = 0;
      printf("\n");
      while (matches[i] != NULL)
        printf("%s\n", matches[i++]);

      /* ensure path is completed to the longest common prefix */
      i = common_prefix((const char **)matches);
      matches[0][i] = 0;
    }

    /* matches exactly one entry */
    found = matches[0];

    /* complete the entry if it's not complete yet
     * but only if the cursor is on a blank or '/' */
    pathlen = strlen(found);
    if (pathlen >= strlen(tmppath.collname)) {
      switch (npath[cp]) {
      case ' ':
      case '\0':
      case '\n':
      case '\t':
        if (pathlen > strlen(tmppath.collname)) {
          if (el_insertstr(e, found + strlen(tmppath.collname)) < 0) {
            free(matches);
            bson_strfreev(strv);
            return -1;
          }
        }
        /* append " " if exactly one command matched */
        if (matches[1] == NULL) {
          if (el_insertstr(e, " ") < 0) {
            free(matches);
            bson_strfreev(strv);
            return -1;
          }
        }
        break;
      }
    }
    break;
  case CNONE:
  default:
    errx(1, "unexpected completion");
  }

  free(matches);
  bson_strfreev(strv);

  return 0;
}

int
exec_lsarg(const char *npath)
{
  int ret;
  path_t tmppath;
  mongoc_collection_t *ccoll;

  if (strlcpy(tmppath.dbname, path.dbname, MAXDBNAME) > MAXDBNAME)
    return -1;
  if (strlcpy(tmppath.collname, path.collname, MAXCOLLNAME) > MAXCOLLNAME)
    return -1;

  if (parse_path(npath, &tmppath) < 0)
    errx(1, "illegal path spec");

  if (strlen(tmppath.collname)) { /* print all document ids */
    ccoll = mongoc_client_get_collection(client, tmppath.dbname, tmppath.collname);
    ret = exec_query(ccoll, "{}", 2, 1);
    mongoc_collection_destroy(ccoll);
    return ret;
  } else if (strlen(tmppath.dbname))
    return exec_lscolls(client, tmppath.dbname);
  else
    return exec_lsdbs(client, NULL);
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
    if ((offset = relaxed_to_strict(doc, docsize, line, len, 1)) < 0) {
      warnx("jsonify error: %ld", offset);
      return -1;
    }
  }

  return offset;
}

/*
 * Parse path that consists of a database name and or a collection name. Support
 * both absolute and relative paths.
 * Absolute paths always start with a / followed by a database name.
 * Relative paths depend on the db and collection values in newpath.
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

  /* trim leading blanks */
  while (*path == ' ' || *path == '\t' || *path == '\n')
    path++;

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

  /* debian calls parse_cmd on startup and "optimizes out" argc */
  if (line == NULL)
    return UNKNOWN;

  /* check if the first token matches one or more commands */
  if (prefix_match((const char ***)&list_match, cmds, argv[0]) == -1)
    errx(1, "prefix_match error");

  // unknown prefix
  if (list_match[0] == NULL)
    return UNKNOWN;

  // matches more than one command
  if (list_match[1] != NULL)
    return AMBIGUOUS;

  // matches exactly one command from cmds
  cmd = list_match[0];

  if (strcmp("cd", cmd) == 0) {
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

  if (strcmp("ls", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    switch (argc) {
    case 1:
    case 2:
      return LSARG;
    default:
      return ILLEGAL;
    }
  }

  /* ls works without a database */

  if (strcmp("databases", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    switch (argc) {
    case 1:
      return LSDBS;
    default:
      return ILLEGAL;
    }
  }

  /* all the other commands need a database to be selected */

  if (!strlen(path.dbname))
    return DBMISSING;

  /* ls works without a selected collection as well */

  if (strcmp("collections", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    switch (argc) {
    case 1:
      return LSCOLLS;
    default:
      return ILLEGAL;
    }
  }

  /* all the other commands need a collection to be selected */

  if (!strlen(path.collname))
    return COLLMISSING;

  if (strcmp("count", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    return COUNT;
  } else if (strcmp("update", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    return UPDATE;
  } else if (strcmp("upsert", cmd) == 0) {
    *lp = strstr(line, argv[0]) + strlen(argv[0]);
    return UPSERT;
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
  case LSARG:
    return exec_lsarg(line);
  case LSDBS:
    return exec_lsdbs(client, NULL);
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
    return exec_update(ccoll, line, 0);
  case UPSERT:
    return exec_update(ccoll, line, 1);
  case INSERT:
    return exec_insert(ccoll, line, linelen);
  case REMOVE:
    return exec_remove(ccoll, line, linelen);
  case FIND:
    return exec_query(ccoll, line, linelen, 0);
  case AGQUERY:
    return exec_agquery(ccoll, line, linelen);
  }

  return -1;
}

// list database for the given client
// return 0 on success, -1 on failure
int exec_lsdbs(mongoc_client_t *client, const char *prefix)
{
  bson_error_t error;
  char **strv;
  int i, prefixlen;

  if (prefix != NULL)
    prefixlen = strlen(prefix);

  if ((strv = mongoc_client_get_database_names(client, &error)) == NULL) {
    warnx("cursor failed: %d.%d %s", error.domain, error.code, error.message);
    return -1;
  }

  for (i = 0; strv[i]; i++)
    if (prefix == NULL) {
      printf("%s\n", strv[i]);
    } else {
      if (strncmp(prefix, strv[i], prefixlen) == 0)
        printf("%s\n", strv[i]);
    }

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
    warnx("%d.%d %s", error.domain, error.code, error.message);
    return -1;
  }

  if ((count = mongoc_collection_count(collection, MONGOC_QUERY_NONE, &query, 0, 0, NULL, &error)) == -1) {
    warnx("cursor failed: %d.%d %s", error.domain, error.code, error.message);
    return -1;
  }

  printf("%lld\n", count);

  return 0;
}

// parse update command, expect two json objects, a selector, and an update doc and exec
int exec_update(mongoc_collection_t *collection, const char *line, int upsert)
{
  long offset;
  char query_doc[MAXDOC];
  char update_doc[MAXDOC];
  bson_error_t error;
  bson_t query, update;

  int opts = MONGOC_UPDATE_NONE;
  if (upsert)
    opts |= MONGOC_UPDATE_UPSERT;

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
    warnx("%d.%d %s", error.domain, error.code, error.message);
    return -1;
  }

  // try to parse the update as json and convert to bson
  if (!bson_init_from_json(&update, update_doc, -1, &error)) {
    warnx("%d.%d %s", error.domain, error.code, error.message);
    return -1;
  }

  /* execute update, always try with multi first, and if that fails, without */
  if (!mongoc_collection_update(collection, opts | MONGOC_UPDATE_MULTI_UPDATE, &query, &update, NULL, &error)) {
    /* if error is "multi update only works with $ operators", retry without MULTI */
    if (error.domain == MONGOC_ERROR_COMMAND && error.code == MONGOC_ERROR_CLIENT_TOO_SMALL) {
      if (!mongoc_collection_update(collection, opts, &query, &update, NULL, &error)) {
        warnx("%d.%d %s", error.domain, error.code, error.message);
        return -1;
      }
    } else {
      warnx("%d.%d %s", error.domain, error.code, error.message);
      return -1;
    }
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
    warnx("%d.%d %s", error.domain, error.code, error.message);
    return -1;
  }

  // execute insert
  if (!mongoc_collection_insert(collection, MONGOC_INSERT_NONE, &doc, NULL, &error)) {
    warnx("%d.%d %s", error.domain, error.code, error.message);
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
    warnx("%d.%d %s", error.domain, error.code, error.message);
    return -1;
  }

  // execute remove
  if (!mongoc_collection_remove(collection, MONGOC_REMOVE_NONE, &doc, NULL, &error)) {
    warnx("%d.%d %s", error.domain, error.code, error.message);
    return -1;
  }

  return 0;
}

// execute a query
// return 0 on success, -1 on failure
int exec_query(mongoc_collection_t *collection, const char *line, int len, int idsonly)
{
  long i;
  mongoc_cursor_t *cursor;
  bson_error_t error;
  size_t rlen;
  const bson_t *doc;
  char *str;
  bson_t query, fields;
  char query_doc[MAXDOC] = "{}"; /* default to all documents */
  struct winsize w;

  if (parse_selector(query_doc, MAXDOC, line, len) == -1)
    return -1;

  // try to parse it as json and convert to bson
  if (!bson_init_from_json(&query, query_doc, -1, &error)) {
    warnx("%d.%d %s", error.domain, error.code, error.message);
    return -1;
  }

  if (idsonly)
    if (!bson_init_from_json(&fields, "{ \"_id\": true }", -1, &error)) {
      warnx("%d.%d %s", error.domain, error.code, error.message);
      return -1;
    }

  cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &query, idsonly ? &fields : NULL, NULL);

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
    warnx("cursor failed: %d.%d %s", error.domain, error.code, error.message);
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
    warnx("%d.%d %s", error.domain, error.code, error.message);
    return -1;
  }

  cursor = mongoc_collection_aggregate(collection, MONGOC_QUERY_NONE, &aggr_query, NULL, NULL);

  while (mongoc_cursor_next(cursor, &doc)) {
    str = bson_as_json(doc, NULL);
    printf ("%s\n", str);
    bson_free(str);
  }

  if (mongoc_cursor_error(cursor, &error)) {
    warnx("cursor failed: %d.%d %s", error.domain, error.code, error.message);
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

  if (strlen(c1) && strlen(c2))
    snprintf(pmpt, MAXPROMPT + 1, "/%s/%s> ", c1, c2);
  else if (strlen(c1))
    snprintf(pmpt, MAXPROMPT + 1, "/%s> ", c1);
  else
    snprintf(pmpt, MAXPROMPT + 1, "/> ");

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
