%{
//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//
// Replaces the hand-written gettok(). Keywords and literals become named
// tokens declared in parser.y; any other character is returned as its own
// ASCII value, just like before.

#include "ast.h"
#include "parser.tab.h"
#include <cstdlib>
#include <string>
%}

%option noyywrap nounput noinput

DIGIT   [0-9]
ALPHA   [a-zA-Z]

%%

[ \t\r\n]+                  { /* skip whitespace */ }
"#".*                       { /* comment until end of line */ }

"def"                       { return DEF; }
"extern"                    { return EXTERN; }

"int"                       {return INT; }
"double"                    {return DOUBLE; }
"bool"                      {return BOOL; }

"true"                      {return TRUE_LIT;}
"false"                     {return FALSE_LIT;}
[0-9]+                      {yylval.num = std::stod(yytext); return INT_LIT;}
[0-9]+\.[0-9]+              {yylval.num = std::stod(yytext); return DOUBLE_LIT;}

{ALPHA}({ALPHA}|{DIGIT})*   { yylval.str = new std::string(yytext);
                              return IDENTIFIER; }

.                           { return yytext[0]; }

%%
