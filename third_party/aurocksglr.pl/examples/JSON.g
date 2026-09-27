# AurocksGLR Grammar for JSON -- Minimal
# A scannerless GLR grammar. Whitespace handling is assumed to be
# declared via %skip (see the %skip directive below).

%{

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A simple singly linked list used for arrays and object members. */
typedef struct SList
{
   struct YY_TYPE *value;   /* element (array) or member value (object) */
   char           *key;     /* member key, NULL for array elements      */
   struct SList   *next;
} SList;

typedef struct YY_TYPE
{
   enum YY_TAG
   {
      JSON_STRING,
      JSON_NUMBER,
      JSON_ARRAY,
      JSON_OBJECT,
      JSON_TRUE,
      JSON_FALSE,
      JSON_NULL,
   } tag;

   union
   {
      char         *string;
      double        number;
      SList        *array;    /* JSON_ARRAY  */
      SList        *object;   /* JSON_OBJECT: list of keyed members */
   } as;
} YY_TYPE;

/* Constructors -- one per shape of node. */
static YY_TYPE *build_string (char *s);
static YY_TYPE *build_number (double d);
static YY_TYPE *build_array  (SList *elems);
static YY_TYPE *build_object (SList *members);
static YY_TYPE *build_lit    (enum YY_TAG tag);   /* true / false / null */

/* List helpers. append() adds to the tail so source order is preserved. */
static SList *slist_new    (char *key, YY_TYPE *value);
static SList *slist_append (SList *list, char *key, YY_TYPE *value);

static inline void die (char *msg)
{
   fprintf (stderr, "%s", msg);
   fputc ('\n', stderr);
   exit (EXIT_FAILURE);
}


static void pretty_print_json (YY_TYPE *);

%}

%target C
%start json

%skip /[ \t\r\n]+/          /* scannerless: skip insignificant whitespace */

%%

json:
     %empty                 { $$ = NULL; }
   | value                  { $$ = $1;   }
   ;

value:
     object                 { $$ = $1; }
   | array                  { $$ = $1; }
   | STRING                 { $$ = build_string ($1); }
   | NUMBER                 { $$ = build_number ($1); }
   | "true"                 { $$ = build_lit (JSON_TRUE);  }
   | "false"                { $$ = build_lit (JSON_FALSE); }
   | "null"                 { $$ = build_lit (JSON_NULL);  }
   ;

// utf8 characters in single quotes, utf8 strings in double quotes
// regular expressions between forward slashes
object:
     '{' '}'                            { $$ = build_object (NULL); }
   | '{' members '}'                    { $$ = build_object ($2);   }
   ;

members:
     member                             { $$ = $1; }
   | members ',' member                 { $$ = slist_append ($1, $3->key, $3->value); }
   ;

member:
     STRING ':' value                   { $$ = slist_new ($1, $3); }
   ;

array:
     '[' ']'                            { $$ = build_array (NULL); }
   | '[' elements ']'                   { $$ = build_array ($2);   }
   ;

elements:
     value                              { $$ = slist_new (NULL, $1); }
   | elements ',' value                 { $$ = slist_append ($1, NULL, $3); }
   ;

STRING:
     /"([^"\\]|\\["\\\/bfnrt]|\\u[0-9a-fA-F]{4})*"/
                                        { $$ = my_strdup ($TOKEN); }
   ;

NUMBER:
     /-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?/
                                        { $$ = strtod ($TOKEN, NULL); }
   ;

%%

/* --- Constructors --- */

static YY_TYPE *
build_string (char *s)
{
   YY_TYPE *n = malloc (sizeof *n);
   n->tag = JSON_STRING;
   n->as.string = s;
   return n;
}

static YY_TYPE *
build_number (double d)
{
   YY_TYPE *n = malloc (sizeof *n);
   n->tag = JSON_NUMBER;
   n->as.number = d;
   return n;
}

static YY_TYPE *
build_array (SList *elems)
{
   YY_TYPE *n = malloc (sizeof *n);
   n->tag = JSON_ARRAY;
   n->as.array = elems;
   return n;
}

static YY_TYPE *
build_object (SList *members)
{
   YY_TYPE *n = malloc (sizeof *n);
   n->tag = JSON_OBJECT;
   n->as.object = members;
   return n;
}

static YY_TYPE *
build_lit (enum YY_TAG tag)
{
   YY_TYPE *n = malloc (sizeof *n);
   n->tag = tag;
   return n;
}

/* --- List helpers --- */

static SList *
slist_new (char *key, YY_TYPE *value)
{
   SList *node = malloc (sizeof *node);
   node->key   = key;
   node->value = value;
   node->next  = NULL;
   return node;
}

static SList *
slist_append (SList *list, char *key, YY_TYPE *value)
{
   SList *node = slist_new (key, value);
   if (list == NULL)
      return node;
   SList *tail = list;
   while (tail->next != NULL)
      tail = tail->next;
   tail->next = node;
   return list;
}

int
main (int argc, char **argv)
{
   FILE *infile = NULL;

   if (argc < 2 && !isatty (STDIN_FILENO))   /* piped / redirected input */
      infile = stdin;
   else if (argc < 2)
      die ("No input file given");
   else
   {
      if (access (argv[1], F_OK) != 0)
         die ("File does not exist");

      infile = fopen (argv[1], "r");
      if (infile == NULL)
         die ("Failed to open file");
   }

   fseek (infile, 0, SEEK_END);          /* go to end to measure size    */
   ssize_t len = ftell (infile);
   fseek (infile, 0, SEEK_SET);          /* rewind before reading        */

   if (len < 0)
      die ("Failed reading JSON file");

   char *contents = malloc (len + 1);    /* +1 for null terminator       */
   if (contents == NULL)
      die ("Out of memory");

   size_t rlen = fread (contents, 1, len, infile);
   contents[rlen] = '\0';

   if ((ssize_t) rlen != len)
      die ("Error reading entire file");

   fclose (infile);

   YY_TYPE *rjson = (YY_TYPE*)yyparse (contents);
   free (contents);

   pretty_print_json (rjson);
   free (rjson);

   return 0;
}

static void pretty_print_r (YY_TYPE *node, int depth);

static void
indent (int depth)
{
   for (int i = 0; i < depth; i++)
      fputs ("  ", stdout);   /* two-space indent per level */
}

static void
pretty_print_r (YY_TYPE *node, int depth)
{
   if (node == NULL)
   {
      fputs ("null", stdout);
      return;
   }

   switch (node->tag)
   {
   case JSON_NULL:    fputs ("null",  stdout); break;
   case JSON_TRUE:    fputs ("true",  stdout); break;
   case JSON_FALSE:   fputs ("false", stdout); break;

   case JSON_STRING:
      /* $TOKEN was stored with surrounding quotes intact via my_strdup. */
      fputs (node->as.string, stdout);
      break;

   case JSON_NUMBER:
      /* Print as integer when the value is whole, else floating-point. */
      if (node->as.number == (long long) node->as.number)
         printf ("%lld", (long long) node->as.number);
      else
         printf ("%.17g", node->as.number);
      break;

   case JSON_ARRAY:
   {
      SList *elem = node->as.array;
      if (elem == NULL) { fputs ("[]", stdout); break; }

      fputs ("[\n", stdout);
      for (; elem != NULL; elem = elem->next)
      {
         indent (depth + 1);
         pretty_print_r (elem->value, depth + 1);
         if (elem->next != NULL)
            fputc (',', stdout);
         fputc ('\n', stdout);
      }
      indent (depth);
      fputc (']', stdout);
      break;
   }

   case JSON_OBJECT:
   {
      SList *member = node->as.object;
      if (member == NULL) { fputs ("{}", stdout); break; }

      fputs ("{\n", stdout);
      for (; member != NULL; member = member->next)
      {
         indent (depth + 1);
         /* key was also stored with surrounding quotes by the grammar action */
         printf ("%s: ", member->key);
         pretty_print_r (member->value, depth + 1);
         if (member->next != NULL)
            fputc (',', stdout);
         fputc ('\n', stdout);
      }
      indent (depth);
      fputc ('}', stdout);
      break;
   }
   }
}

static void
pretty_print_json (YY_TYPE *json)
{
   pretty_print_r (json, 0);
   fputc ('\n', stdout);
}

