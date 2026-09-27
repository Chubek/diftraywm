# AurocksGLR Grammar for JSON -- with disambiguation directives
#
# Disambiguation strategy
# -----------------------
# 1. %dprec  (dynamic precedence) — resolves GLR reduce/reduce conflicts
#            by assigning an integer rank to competing productions;
#            the higher rank wins.
# 2. %prefer / %avoid — scannerless lexical disambiguation; tells the
#            engine which alternative to keep when two token rules
#            match the same span.
# 3. %longest — maximal-munch hint; prefer the longest token match
#            over a shorter one for a given rule.

%{

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SList  SList;
typedef struct YY_TYPE YY_TYPE;

struct SList {
    YY_TYPE *value;
    char    *key;
    SList   *next;
};

struct YY_TYPE {
    enum YY_TAG {
        JSON_STRING, JSON_NUMBER,
        JSON_ARRAY,  JSON_OBJECT,
        JSON_TRUE,   JSON_FALSE, JSON_NULL,
    } tag;
    union {
        char   *string;
        double  number;
        SList  *array;
        SList  *object;
    } as;
};

static YY_TYPE *build_string(char *s);
static YY_TYPE *build_number(double d);
static YY_TYPE *build_array (SList *elems);
static YY_TYPE *build_object(SList *members);
static YY_TYPE *build_lit   (enum YY_TAG tag);
static SList   *slist_new   (char *key, YY_TYPE *value);
static SList   *slist_append(SList *list, char *key, YY_TYPE *value);

/* Duplicate a raw JSON string token, stripping surrounding quotes.
   Full Unicode unescaping should be added here for production use.  */
static char *json_strdup(const char *token);

%}

%start  json
%skip   /[ \t\r\n]+/

/* Keyword priority directive.
   In a scannerless setting any rule that could tokenise the same span
   as a keyword should lose to the explicit keyword literal.
   %prefer on the keyword alternatives in `value` achieves this.     */
%prefer "true" "false" "null"

%%

/* ------------------------------------------------------------------ */
/* Start rule                                                          */
/*                                                                     */
/* %dprec 2 on the `value` branch means: if both alternatives reduce  */
/* at the same input position, discard the %empty forest and keep the */
/* `value` forest.  Without this, a GLR engine builds both trees for  */
/* every non-empty input and the ambiguity propagates upward.         */
/* ------------------------------------------------------------------ */
json:
     %empty   %dprec 1   { $$ = NULL; }
   | value    %dprec 2   { $$ = $1;   }
   ;

/* ------------------------------------------------------------------ */
/* Value                                                               */
/*                                                                     */
/* %prefer on the three keyword alternatives prevents them from ever  */
/* being re-interpreted as STRING, even if the lexical rules change.  */
/* ------------------------------------------------------------------ */
value:
     object                { $$ = $1; }
   | array                 { $$ = $1; }
   | STRING                { $$ = build_string($1); }
   | NUMBER                { $$ = build_number($1); }
   | "true"  %prefer       { $$ = build_lit(JSON_TRUE);  }
   | "false" %prefer       { $$ = build_lit(JSON_FALSE); }
   | "null"  %prefer       { $$ = build_lit(JSON_NULL);  }
   ;

/* ------------------------------------------------------------------ */
/* Object                                                              */
/* ------------------------------------------------------------------ */
object:
     '{' '}'           { $$ = build_object(NULL); }
   | '{' members '}'   { $$ = build_object($2);   }
   ;

members:
     member                      { $$ = $1; }
   | members ',' member          { $$ = slist_append($1, $3->key, $3->value); }
   ;

member:
     STRING ':' value            { $$ = slist_new($1, $3); }
   ;

/* ------------------------------------------------------------------ */
/* Array                                                               */
/* ------------------------------------------------------------------ */
array:
     '[' ']'            { $$ = build_array(NULL); }
   | '[' elements ']'   { $$ = build_array($2);   }
   ;

elements:
     value                       { $$ = slist_new(NULL, $1); }
   | elements ',' value          { $$ = slist_append($1, NULL, $3); }
   ;

/* ------------------------------------------------------------------ */
/* Lexical rules                                                       */
/*                                                                     */
/* %longest instructs the scannerless engine to apply maximal munch:  */
/* always prefer the match that consumes the most input characters.   */
/* This matters for NUMBER, where `-0` should not be tokenised as     */
/* two separate tokens (`-` and `0`) when the full regex can match.  */
/* ------------------------------------------------------------------ */
STRING %longest:
     /"([^"\\]|\\["\\\/bfnrt]|\\u[0-9a-fA-F]{4})*"/
                                 { $$ = json_strdup($TOKEN); }
   ;

NUMBER %longest:
     /-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?/
                                 { $$ = strtod($TOKEN, NULL); }
   ;

%%

/* ------------------------------------------------------------------ */
/* C epilogue                                                          */
/* ------------------------------------------------------------------ */

static char *
json_strdup(const char *token)
{
    /* Strip the surrounding double-quote characters. */
    size_t len = strlen(token);
    if (len < 2) return strdup("");
    char *s = malloc(len - 1);          /* len-2 content chars + NUL  */
    memcpy(s, token + 1, len - 2);
    s[len - 2] = '\0';
    /* TODO: unescape \\, \", \/, \b, \f, \n, \r, \t, \uXXXX here.  */
    return s;
}

static YY_TYPE *build_string(char *s)
{
    YY_TYPE *n   = malloc(sizeof *n);
    n->tag       = JSON_STRING;
    n->as.string = s;
    return n;
}

static YY_TYPE *build_number(double d)
{
    YY_TYPE *n   = malloc(sizeof *n);
    n->tag       = JSON_NUMBER;
    n->as.number = d;
    return n;
}

static YY_TYPE *build_array(SList *elems)
{
    YY_TYPE *n  = malloc(sizeof *n);
    n->tag      = JSON_ARRAY;
    n->as.array = elems;
    return n;
}

static YY_TYPE *build_object(SList *members)
{
    YY_TYPE *n   = malloc(sizeof *n);
    n->tag       = JSON_OBJECT;
    n->as.object = members;
    return n;
}

static YY_TYPE *build_lit(enum YY_TAG tag)
{
    YY_TYPE *n = malloc(sizeof *n);
    n->tag     = tag;
    return n;
}

static SList *
slist_new(char *key, YY_TYPE *value)
{
    SList *node  = malloc(sizeof *node);
    node->key    = key;
    node->value  = value;
    node->next   = NULL;
    return node;
}

static SList *
slist_append(SList *list, char *key, YY_TYPE *value)
{
    SList *node = slist_new(key, value);
    if (!list) return node;
    SList *tail = list;
    while (tail->next) tail = tail->next;
    tail->next = node;
    return list;
}
