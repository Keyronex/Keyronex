%{
#include <stdlib.h>
#include "parser.tab.h"

extern int yylex(void);
extern int yyerror(const char *);
%}

%option noyywrap nounput noinput

%%
"ALLOC" return ALLOC;
"FREE"  return FREE;
[0-9]+ { yylval.i = atoi(yytext); return INT; }
0[xX][0-9a-fA-F]+ { yylval.str = yytext; return HEX; }
"str:"[^ \n\t]* { yylval.str = yytext + 4; return STR; }
\n
.
