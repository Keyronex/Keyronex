
%code requires {
    #include <list>
    #include <string>
    #include <vector>
}

%{
#include <iostream>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>

#include "result.hh"

extern int yylex(void);
extern int yyerror(const char*);
%}


%union {
    const char *str;
    std::vector<std::string> *strlist;
    int i;
}

%debug

%token ALLOC FREE
%token <str> HEX STR
%token <i> INT

%type <str> string hex

%type <strlist> hex_list opt_hex_list

%%
input:
  note_list ;

note_list:
      note
    | note_list note
    ;

string: STR
    {
        $$ = strdup($1);
    }

hex: HEX
    {
        $$ = strdup($1);
    }


hex_list:
      hex {
        auto list = new std::vector<std::string>;
        list->push_back($1);
        $$ = list;
    }
    | hex_list hex  {
        $$ = $1;
        $$->push_back($2);
    }
    ;

opt_hex_list:
      hex_list { $$ = $1; }
    | %empty { $$ = NULL; }
    ;

note:
      ALLOC INT string hex opt_hex_list {
        if(allocs.find($4) != allocs.end()) {
            std::cout << "Allocation of an already-allocated address(" << $4 << ") !\n";
            //abort();
        }
        allocs[$4] = Allocation { $2, $3, $5 };
    }
    | FREE string hex {
        auto it = allocs.find($3);
        if (it != allocs.end())
            allocs.erase(it);
        else 
            std::cout << "Didn't find " << $3 << "\n";
     }
    ;
