#include "common.h"
#include "scanner.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define advance(scanner)   (*scanner->current++)
#define peek(scanner)      (*scanner->current)
#define isend(scanner)     (*scanner->current == '\0')
#define peek_next(scanner) ((isend(scanner)) ? '\0' : *(scanner->current + 1))

static Token     Token_new(Scanner* scanner, TokenType type);
static Token     Token_error(Scanner* scanner, const char* err);
static Token     Token_string(Scanner* scanner);
static Token     Token_number(Scanner* scanner);
static Token     Token_identifier(Scanner* scanner);
static TokenType TokenType_identifier(Scanner* scanner);
static void      Scanner_skipws(Scanner* scanner);
static bool      Scanner_match(Scanner* scanner, char c);
static TokenType check_keyword(
    Scanner*    scanner,
    UInt        start,
    UInt        end,
    const char* pattern,
    TokenType   type);

Scanner Scanner_new(const char* source)
{
    return (Scanner){
        .start   = source,
        .current = source,
        .line    = 1,
    };
}

Token Scanner_scan(Scanner* scanner)
{
    Scanner_skipws(scanner);
    scanner->start = scanner->current;

    if(isend(scanner)) {
        return Token_new(scanner, TOK_EOF);
    }

    char c = advance(scanner);

    if(c == '_' || isalpha(c)) {
        return Token_identifier(scanner);
    }

    if(isdigit(c)) {
        return Token_number(scanner);
    }

#ifdef THREADED_CODE
    #define lbrack &&err
    #define rbrack &&err
    // NOTE: This is a GCC extension
    // https://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html
    // IMPORTANT: update accordingly if TokenType enum changes!
    static const void* jump_table[UINT8_MAX + 1] = {
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&bang,   &&string, &&err,       &&err,  &&err,    &&err,     &&err,
        &&lparen, &&rparen, &&star,   &&plus,      &&err,  &&minus,  &&dot,     &&slash,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&colon,  &&semicolon, &&less, &&equal,  &&greater, &&qmark,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    lbrack,      &&err,  rbrack,   &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&lbrace,    &&err,  &&rbrace, &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
        &&err,    &&err,    &&err,    &&err,       &&err,  &&err,    &&err,     &&err,
    };
    #undef lbrack
    #undef rbrack

    goto* jump_table[c];

lparen:
    return Token_new(scanner, TOK_LPAREN);
rparen:
    return Token_new(scanner, TOK_RPAREN);
lbrace:
    return Token_new(scanner, TOK_LBRACE);
rbrace:
    return Token_new(scanner, TOK_RBRACE);
dot:
    return Token_new(scanner, TOK_DOT);
comma:
    return Token_new(scanner, TOK_COMMA);
minus:
    return Token_new(scanner, TOK_MINUS);
plus:
    return Token_new(scanner, TOK_PLUS);
colon:
    return Token_new(scanner, TOK_COLON);
semicolon:
    return Token_new(scanner, TOK_SEMICOLON);
slash:
    return Token_new(scanner, TOK_SLASH);
star:
    return Token_new(scanner, TOK_STAR);
qmark:
    return Token_new(scanner, TOK_QMARK);
bang:
    return Token_new(scanner, Scanner_match(scanner, '=') ? TOK_BANG_EQUAL : TOK_BANG);
equal:
    return Token_new(scanner, Scanner_match(scanner, '=') ? TOK_EQUAL_EQUAL : TOK_EQUAL);
greater:
    return Token_new(
        scanner,
        Scanner_match(scanner, '=') ? TOK_GREATER_EQUAL : TOK_GREATER);
less:
    return Token_new(scanner, Scanner_match(scanner, '=') ? TOK_LESS_EQUAL : TOK_LESS);
string:
    return Token_string(scanner);
err:
    return Token_error(scanner, "Unexpected character.");

    _unreachable;

#else
    switch(c) {
        case '(':
            return Token_new(scanner, TOK_LPAREN);
        case ')':
            return Token_new(scanner, TOK_RPAREN);
        case '{':
            return Token_new(scanner, TOK_LBRACE);
        case '}':
            return Token_new(scanner, TOK_RBRACE);
        case '.':
            return Token_new(scanner, TOK_DOT);
        case ',':
            return Token_new(scanner, TOK_COMMA);
        case '-':
            return Token_new(scanner, TOK_MINUS);
        case '+':
            return Token_new(scanner, TOK_PLUS);
        case ';':
            return Token_new(scanner, TOK_SEMICOLON);
        case ':':
            return Token_new(scanner, TOK_COLON);
        case '?':
            return Token_new(scanner, TOK_QMARK);
        case '/':
            return Token_new(scanner, TOK_SLASH);
        case '*':
            return Token_new(scanner, TOK_STAR);
        case '!':
            return Token_new(
                scanner,
                Scanner_match(scanner, '=') ? TOK_BANG_EQUAL : TOK_BANG);
        case '=':
            return Token_new(
                scanner,
                Scanner_match(scanner, '=') ? TOK_EQUAL_EQUAL : TOK_EQUAL);
        case '>':
            return Token_new(
                scanner,
                Scanner_match(scanner, '=') ? TOK_GREATER_EQUAL : TOK_GREATER);
        case '<':
            return Token_new(
                scanner,
                Scanner_match(scanner, '=') ? TOK_LESS_EQUAL : TOK_LESS);
        case '"':
            return Token_string(scanner);
        default:
            return Token_error(scanner, "Unexpected character.");
    }
#endif
}

static bool Scanner_match(Scanner* scanner, char c)
{
    if(isend(scanner) || c != peek(scanner)) {
        return false;
    }
    scanner->current++;
    return true;
}

static void Scanner_skipws(Scanner* scanner)
{
    register char c;

    while(true) {
#ifdef THREADED_CODE
    #define WS  &&ws
    #define NL  &&nl
    #define COM &&com
        // NOTE: This is a GCC extension
        // https://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html
        // IMPORTANT: update accordingly if language grammar changes!
        static const void* jump_table[UINT8_MAX + 1] = {
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, WS,    NL,
            &&ret, &&ret, WS,    &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, WS,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, COM,   &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&com, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
            &&ret, &&ret, &&ret,
        };
    #undef WS
    #undef NL
    #undef COM

        goto* jump_table[peek(scanner)];

    nl:
        scanner->line++;
        advance(scanner);
        continue;
    ws:
        advance(scanner);
        continue;
    com:
        if((c = peek_next(scanner)) == '/') {
            while(peek(scanner) != '\n' && !isend(scanner)) {
                advance(scanner);
            }
        } else {
            return;
        }
        continue;
    ret:
        return;

        _unreachable;

#else
        switch((c = peek(scanner))) {
            case '\n':
                scanner->line++;
                advance(scanner);
                break;
            case ' ':
            case '\r':
            case '\t':
                advance(scanner);
                break;
            case '/':
                if((c = peek_next(scanner)) == '/') {
                    while(peek(scanner) != '\n' && !isend(scanner)) {
                        advance(scanner);
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
#endif
    }
}

static Token Token_new(Scanner* scanner, TokenType type)
{
    Token token;
    token.type  = type;
    token.start = scanner->start;
    token.len   = scanner->current - scanner->start;
    token.line  = scanner->line;
    return token;
}

static Token Token_error(Scanner* scanner, const char* err)
{
    Token token;
    token.type  = TOK_ERROR;
    token.start = err;
    token.len   = strlen(err);
    token.line  = scanner->line;
    return token;
}

static Token Token_string(Scanner* scanner)
{
    while(peek(scanner) != '"' && !isend(scanner)) {
        if(peek(scanner) == '\n') {
            scanner->line++;
        }
        advance(scanner);
    }

    if(isend(scanner)) {
        return Token_error(scanner, "Unterminated string, missing closing quotes '\"'");
    }

    advance(scanner);
    return Token_new(scanner, TOK_STRING);
}

static Token Token_number(Scanner* scanner)
{
    while(isdigit(peek(scanner))) {
        advance(scanner);
    }

    if(peek(scanner) == '.' && isdigit(peek_next(scanner))) {
        advance(scanner);

        while(isdigit(peek(scanner))) {
            advance(scanner);
        }
    }

    return Token_new(scanner, TOK_NUMBER);
}

static Token Token_identifier(Scanner* scanner)
{
    register char c;

    advance(scanner);
    while(isalnum((c = peek(scanner))) || c == '_') {
        advance(scanner);
    }

    return Token_new(scanner, TokenType_identifier(scanner));
}

static TokenType TokenType_identifier(Scanner* scanner)
{
#ifdef THREADED_CODE
    // NOTE: This is a GCC extension
    // https://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html
    // IMPORTANT: update accordingly if language grammar changes!
    static const void* jump_table[UINT8_MAX + 1] = {
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&a,   &&ret,
        &&c,   &&ret, &&e,   &&f,   &&ret, &&ret, &&i,   &&ret, &&ret, &&ret, &&ret,
        &&n,   &&o,   &&p,   &&ret, &&r,   &&s,   &&t,   &&ret, &&v,   &&w,   &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret, &&ret,
        &&ret, &&ret, &&ret,
    };

    goto* jump_table[*scanner->start];

a:
    return check_keyword(scanner, 1, 2, "nd", TOK_AND);
c:
    return check_keyword(scanner, 1, 4, "lass", TOK_CLASS);
e:
    return check_keyword(scanner, 1, 3, "lse", TOK_ELSE);
f:
    if(scanner->current - scanner->start > 1) {
        switch(scanner->start[1]) {
            case 'a':
                return check_keyword(scanner, 2, 3, "lse", TOK_FALSE);
            case 'n':
                if(scanner->current - scanner->start == 2) {
                    return TOK_FN;
                } else {
                    return TOK_IDENTIFIER;
                }
            case 'o':
                return check_keyword(scanner, 2, 1, "r", TOK_FOR);
            default:
                break;
        }
    }
    goto ret;
i:
    if(scanner->current - scanner->start > 1) {
        switch(scanner->start[1]) {
            case 'm':
                check_keyword(scanner, 2, 2, "pl", TOK_IMPL);
            case 'f':
                if(scanner->current - scanner->start == 2) {
                    return TOK_IF;
                } else {
                    return TOK_IDENTIFIER;
                }
            default:
                break;
        }
    }
    goto ret;
n:
    return check_keyword(scanner, 1, 2, "il", TOK_NIL);
o:
    return check_keyword(scanner, 1, 1, "r", TOK_OR);
p:
    return check_keyword(scanner, 1, 4, "rint", TOK_PRINT);
r:
    return check_keyword(scanner, 1, 5, "eturn", TOK_RETURN);
s:
    if(scanner->current - scanner->start > 1) {
        switch(scanner->start[1]) {
            case 'u':
                return check_keyword(scanner, 2, 3, "per", TOK_SUPER);
            case 'e':
                return check_keyword(scanner, 2, 2, "lf", TOK_SELF);
            default:
                break;
        }
    }
    goto ret;
t:
    return check_keyword(scanner, 1, 3, "rue", TOK_TRUE);
v:
    return check_keyword(scanner, 1, 2, "ar", TOK_VAR);
w:
    return check_keyword(scanner, 1, 4, "hile", TOK_WHILE);

#else
    switch(*scanner->start) {
        case 'a':
            return check_keyword(scanner, 1, 2, "nd", TOK_AND);
        case 'c':
            return check_keyword(scanner, 1, 4, "lass", TOK_CLASS);
        case 'e':
            return check_keyword(scanner, 1, 3, "lse", TOK_ELSE);
        case 'f':
            if(scanner->current - scanner->start > 1) {
                switch(scanner->start[1]) {
                    case 'a':
                        return check_keyword(scanner, 2, 3, "lse", TOK_FALSE);
                    case 'n':
                        if(scanner->current - scanner->start == 2) {
                            return TOK_FN;
                        } else {
                            return TOK_IDENTIFIER;
                        }
                    case 'o':
                        return check_keyword(scanner, 2, 1, "r", TOK_FOR);
                    default:
                        break;
                }
            }
            break;
        case 'i':
            if(scanner->current - scanner->start > 1) {
                switch(scanner->start[1]) {
                    case 'm':
                        check_keyword(scanner, 2, 2, "pl", TOK_IMPL);
                    case 'f':
                        if(scanner->current - scanner->start == 2) {
                            return TOK_IF;
                        } else {
                            return TOK_IDENTIFIER;
                        }
                    default:
                        break;
                }
            }
            break;
        case 'n':
            return check_keyword(scanner, 1, 2, "il", TOK_NIL);
        case 'o':
            return check_keyword(scanner, 1, 1, "r", TOK_OR);
        case 'p':
            return check_keyword(scanner, 1, 4, "rint", TOK_PRINT);
        case 'r':
            return check_keyword(scanner, 1, 5, "eturn", TOK_RETURN);
        case 's':
            if(scanner->current - scanner->start > 1) {
                switch(scanner->start[1]) {
                    case 'u':
                        return check_keyword(scanner, 2, 3, "per", TOK_SUPER);
                    case 'e':
                        return check_keyword(scanner, 2, 2, "lf", TOK_SELF);
                    default:
                        break;
                }
            }
            break;
        case 't':
            return check_keyword(scanner, 1, 3, "rue", TOK_TRUE);
        case 'v':
            return check_keyword(scanner, 1, 2, "ar", TOK_VAR);
        case 'w':
            return check_keyword(scanner, 1, 4, "hile", TOK_WHILE);
        default:
            break;
    }
#endif
ret:
    return TOK_IDENTIFIER;
}

static TokenType check_keyword(
    Scanner*    scanner,
    UInt        start,
    UInt        length,
    const char* pattern,
    TokenType   type)
{
    if(scanner->current - scanner->start == start + length &&
       memcmp(scanner->start + start, pattern, length) == 0)
    {
        return type;
    }

    return TOK_IDENTIFIER;
}