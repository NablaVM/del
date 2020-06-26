%skeleton "lalr1.cc"
%require  "3.0"
%debug 
%defines 
%define api.namespace {DEL}

%define api.parser.class {DEL_Parser}

%define parse.lac full
%define parse.error verbose

%code requires{
   
   namespace DEL 
   {
      class DEL_Driver;
      class DEL_Scanner;
      class Ast;
      class Element;
      class Function;
      class EncodedDataType;
   }

# ifndef YY_NULLPTR
#  if defined __cplusplus && 201103L <= __cplusplus
#   define YY_NULLPTR nullptr
#  else
#   define YY_NULLPTR 0
#  endif
# endif

}

%parse-param { DEL_Scanner  &scanner  }
%parse-param { DEL_Driver  &driver  }

%code{
   #include <iostream>
   #include <cstdlib>
   #include <fstream>
   #include <stdint.h>
   #include <vector>
   
   #include "Ast/Ast.hpp"
   #include "Ast/Elements.hpp"
   #include "Types/Variables.hpp"

   #include "del_driver.hpp"

#undef yylex
#define yylex scanner.yylex
}

%define api.value.type variant
%define parse.assert

%type<DEL::Element*> stmt;
%type<DEL::Element*> assignment;
%type<DEL::Element*> object_assignment;
%type<DEL::Element*> return_stmt;
%type<DEL::Element*> function_stmt;

%type<DEL::Element*> object_definition;

%type<DEL::Element*> object_stmt;
%type<DEL::Element*> member_definition;


%type<DEL::Ast*> expression;
%type<DEL::Ast*> assignment_allowed_expression;
%type<DEL::Ast*> string_expr;
%type<DEL::Ast*> term;
%type<DEL::Ast*> factor;
%type<DEL::Ast*> primary;

%type<EncodedDataType*> assignable_type;
%type<EncodedDataType*> returnable_type;


%type<std::vector<DEL::Element*>> multiple_object_stmts;
%type<std::vector<DEL::Element*>> multiple_statements;
%type<std::vector<DEL::Element*>> multiple_assignments;
%type<std::vector<DEL::Element*>> object_instantiation;
%type<std::vector<DEL::Element*>> block;
%type<std::vector<DEL::Element*>> object_block;

%type<std::string> identifiers;

%token LEFT_PAREN LEFT_BRACKET ASSIGN VAR LET

%token DOT COMMA COL 

%token INT DOUBLE STRING NIL ARROW RETURN PUB PRIV

%token LSH RSH BW_OR BW_AND BW_XOR AND OR NEGATE  MOD
%token LTE GTE GT LT EQ NE BW_NOT DIV ADD SUB MUL POW

%token <std::string> INT_LITERAL
%token <std::string> HEX_LITERAL
%token <std::string> REAL_LITERAL
%token <std::string> CHAR_LITERAL
%token <std::string> STRING_LITERAL

%token <std::string> IDENTIFIER
%token <int>         RIGHT_BRACKET  // These tokens encode line numbers
%token <int>         RIGHT_PAREN    // These tokens encode line numbers
%token <int>         SEMI           // These tokens encode line numbers
%token <int>         FUNC           // These tokens encode line numbers
%token <int>         OBJECT        // These tokens encode line numbers

%token               END    0     "end of file"
%locations
%start start

%%

start 
   : END                   { driver.indicate_complete(); }
   | input END             { driver.indicate_complete(); }
   ; 

input
   : function_stmt           { driver.build($1); }
   | input function_stmt     { driver.build($2); }
   | object_definition       { driver.build($1); }
   | input object_definition { driver.build($2); }
   ;

identifiers
   :  IDENTIFIER                 { $$ = $1;      } 
   |  IDENTIFIER DOT identifiers { $$ = $1 + "." + $3; }
   ;

string_expr
   : STRING_LITERAL              { $$ = new DEL::Ast(DEL::Ast::NodeType::VALUE, DEL::DataType::STRING, $1, nullptr, nullptr);  }
   ;

expression
   : term                        { $$ = $1;  }
   | expression ADD term         { $$ = new DEL::Ast(DEL::Ast::NodeType::ADD, $1, $3);  }
   | expression SUB term         { $$ = new DEL::Ast(DEL::Ast::NodeType::SUB, $1, $3);  }
   | expression LTE term         { $$ = new DEL::Ast(DEL::Ast::NodeType::LTE, $1, $3);  }
   | expression GTE term         { $$ = new DEL::Ast(DEL::Ast::NodeType::GTE, $1, $3);  }
   | expression GT  term         { $$ = new DEL::Ast(DEL::Ast::NodeType::GT , $1, $3);  }
   | expression LT  term         { $$ = new DEL::Ast(DEL::Ast::NodeType::LT , $1, $3);  }
   | expression EQ  term         { $$ = new DEL::Ast(DEL::Ast::NodeType::EQ , $1, $3);  }
   | expression NE  term         { $$ = new DEL::Ast(DEL::Ast::NodeType::NE , $1, $3);  }
   ;

term
   : factor                      { $$ = $1;  }
   | term MUL factor             { $$ = new DEL::Ast(DEL::Ast::NodeType::MUL,    $1, $3);  }
   | term DIV factor             { $$ = new DEL::Ast(DEL::Ast::NodeType::DIV,    $1, $3);  }
   | term POW factor             { $$ = new DEL::Ast(DEL::Ast::NodeType::POW,    $1, $3);  }
   | term MOD factor             { $$ = new DEL::Ast(DEL::Ast::NodeType::MOD,    $1, $3);  }
   | term LSH factor             { $$ = new DEL::Ast(DEL::Ast::NodeType::LSH,    $1, $3);  }
   | term RSH factor             { $$ = new DEL::Ast(DEL::Ast::NodeType::RSH,    $1, $3);  }
   | term BW_XOR factor          { $$ = new DEL::Ast(DEL::Ast::NodeType::BW_XOR, $1, $3);  }
   | term BW_OR factor           { $$ = new DEL::Ast(DEL::Ast::NodeType::BW_OR,  $1, $3);  }
   | term BW_AND factor          { $$ = new DEL::Ast(DEL::Ast::NodeType::BW_AND, $1, $3);  }
   | term OR factor              { $$ = new DEL::Ast(DEL::Ast::NodeType::OR,     $1, $3);  }
   | term AND factor             { $$ = new DEL::Ast(DEL::Ast::NodeType::AND,    $1, $3);  }
   ;

factor
   : primary                     { $$ = $1; }
   | LEFT_PAREN expression RIGHT_PAREN    { $$ = $2; }
   | BW_NOT factor               { $$ = new DEL::Ast(DEL::Ast::NodeType::BW_NOT, $2, nullptr);}
   | NEGATE factor               { $$ = new DEL::Ast(DEL::Ast::NodeType::NEGATE, $2, nullptr);}
   ;

primary
    : INT_LITERAL                { $$ = new DEL::Ast(DEL::Ast::NodeType::VALUE,      DEL::DataType::INT,       $1, nullptr, nullptr); }
    | REAL_LITERAL               { $$ = new DEL::Ast(DEL::Ast::NodeType::VALUE,      DEL::DataType::DOUBLE,    $1, nullptr, nullptr); }
    | identifiers                { $$ = new DEL::Ast(DEL::Ast::NodeType::IDENTIFIER, DEL::DataType::ID_STRING, $1, nullptr, nullptr); }
    ;

assignable_type
   : INT    { $$ = new EncodedDataType(DEL::DataType::INT,    "int"   ); }
   | DOUBLE { $$ = new EncodedDataType(DEL::DataType::DOUBLE, "double"); }
   | STRING { $$ = new EncodedDataType(DEL::DataType::STRING, "string"); }
   ;

returnable_type
   : assignable_type          { $$ = $1; }
   | OBJECT LT identifiers GT { $$ = new EncodedDataType(DEL::DataType::USER_DEFINED, $3); }
   | NIL                      { $$ = new EncodedDataType(DEL::DataType::NIL,    "nil"   ); }
   ;

assignment_allowed_expression
   : expression
   | string_expr
   ;

assignment
   : VAR identifiers COL assignable_type ASSIGN assignment_allowed_expression SEMI 
      { 
         // Create an assignment. Make a tree with root node as "=", lhs is an identifier node with the var name
         // and the rhs is the expression
         $$ = new DEL::Assignment(
               false, /* Not immutable */
               new DEL::Ast(DEL::Ast::NodeType::ROOT, DEL::DataType::NONE, "=", 
                  new DEL::Ast(DEL::Ast::NodeType::IDENTIFIER, DEL::DataType::ID_STRING, $2, nullptr, nullptr), /* Var name */
                  $6),  /* Expression AST    */
               $4,      /* Encoded Data type */
            $7);        /* Line Number       */
      }
   | LET identifiers COL assignable_type ASSIGN assignment_allowed_expression SEMI 
      { 
         // Create an assignment. Make a tree with root node as "=", lhs is an identifier node with the var name
         // and the rhs is the expression
         $$ = new DEL::Assignment(
               true, /* Is immutable */
               new DEL::Ast(DEL::Ast::NodeType::ROOT, DEL::DataType::NONE, "=", 
                  new DEL::Ast(DEL::Ast::NodeType::IDENTIFIER, DEL::DataType::ID_STRING, $2, nullptr, nullptr), /* Var name */
                  $6),  /* Expression AST    */
               $4,      /* Encoded Data type */
            $7);        /* Line Number       */
      }
   | identifiers ASSIGN assignment_allowed_expression SEMI
      {
         $$ = new DEL::Reassignment(
            new DEL::Ast(DEL::Ast::NodeType::ROOT, DEL::DataType::NONE, "=", 
               new DEL::Ast(DEL::Ast::NodeType::IDENTIFIER, DEL::DataType::ID_STRING, $1, nullptr, nullptr), /* Var name */
                  $3),  /* Expression AST    */
            $4); /* Line Number */
      }
   | object_assignment { $$ = $1; }
   ;

return_stmt
   : RETURN SEMI                               { $$ = new DEL::Return(nullptr, $2); }
   | RETURN assignment_allowed_expression SEMI { $$ = new DEL::Return($2, $3);      }
   ;

stmt
   : assignment  { $$ = $1; }
   | return_stmt { $$ = $1; }
   ;

multiple_statements
   : stmt                     { $$ = std::vector<DEL::Element*>(); $$.push_back($1); }
   | multiple_statements stmt { $1.push_back($2); $$ = $1; }
   ;

multiple_assignments
   : assignment                      { $$ = std::vector<DEL::Element*>(); $$.push_back($1); }
   | multiple_assignments assignment { $1.push_back($2); $$ = $1; }
   ;

block 
   : LEFT_BRACKET multiple_statements RIGHT_BRACKET { $$ = $2; }
   | LEFT_BRACKET RIGHT_BRACKET                     { $$ = std::vector<DEL::Element*>(); }
   ;

function_stmt 
   : FUNC identifiers LEFT_PAREN RIGHT_PAREN ARROW returnable_type block { $$ = new DEL::Function($2, std::vector<std::string>(), $7, $6, $1); }
   ;

// 
// ------------------ Object Definition Specific Stuff ------------------
// 

object_instantiation
   : LEFT_BRACKET multiple_assignments RIGHT_BRACKET { $$ = $2; }
   | LEFT_BRACKET RIGHT_BRACKET { $$ = std::vector<DEL::Element*>();  }
   ;

object_assignment
   : VAR identifiers COL OBJECT LT identifiers GT ASSIGN object_instantiation SEMI 
      {
         $$ = new DEL::ObjectAssignment(
                                    false, 
                                    $2, 
                                    new EncodedDataType(DEL::DataType::USER_DEFINED, $6),
                                    $9,
                                    $10);
      }
   | LET identifiers COL OBJECT LT identifiers GT ASSIGN object_instantiation SEMI 
      {
         $$ = new DEL::ObjectAssignment(
                                    true, 
                                    $2, 
                                    new EncodedDataType(DEL::DataType::USER_DEFINED, $6),
                                    $9,
                                    $10);
      }
   | identifiers ASSIGN object_instantiation SEMI 
      {
         $$ = new ObjectReassignment($1, $3, $4);
      }
   ;

member_definition
   : assignable_type COL identifiers SEMI { $$ = new ObjectMember($1, $3, $4); }
   ;

object_stmt
   : member_definition { $$ = $1; }
   | function_stmt     { $$ = $1; }
   ;

multiple_object_stmts
   : object_stmt                       { $$ = std::vector<DEL::Element*>(); $$.push_back($1); }
   | multiple_object_stmts object_stmt { $1.push_back($2); $$ = $1; }
   ;

object_block
   : LEFT_BRACKET multiple_object_stmts RIGHT_BRACKET { $$ = $2; }
   | LEFT_BRACKET RIGHT_BRACKET                       { $$ = std::vector<DEL::Element*>(); }
   ;

object_definition
   : OBJECT identifiers LEFT_BRACKET PUB  object_block PRIV object_block RIGHT_BRACKET 
         {
            $$ = new Object($2, $5, $7, $1);
         }
   | OBJECT identifiers LEFT_BRACKET PRIV object_block PUB  object_block RIGHT_BRACKET 
         {
            $$ = new Object($2, $7, $5, $1);
         }
   | OBJECT identifiers LEFT_BRACKET PUB object_block RIGHT_BRACKET 
         {
            $$ = new Object($2, $5, std::vector<DEL::Element*>(), $1);
         }
   ;
%%

void DEL::DEL_Parser::error( const location_type &l, const std::string &err_message )
{

   /*
   DEL::Errors       & error_man = driver.get_error_man_ref();
   DEL::Preprocessor & preproc   = driver.get_preproc_ref();

   // Report the error
   error_man.report_syntax_error(
         l.begin.line,                     // Line where issue appeared
         l.begin.column,                   // Column where issue appeared
         err_message,                      // Bison error information
         preproc.fetch_line(l.begin.line)  // The user line where issue appeared
   );
   */
   std::cerr << "Syntax error >> line: " << l.begin.line << " >> col: " << l.begin.column << std::endl;
}