#include "parse.h"
#include <forward_list>
#include <sstream>
#include <vector>
#pragma warning(push, 0)
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#pragma warning(pop)
#include "lex.h"
#include "../ast/decl.h"
#include "../ast/module.h"
#include "../ast/token.h"
#include "../driver/driver.h"
#include "../support/utility.h"

using namespace delta;

static llvm::MemoryBuffer* getFileMemoryBuffer(llvm::StringRef filePath) {
    auto buffer = llvm::MemoryBuffer::getFile(filePath);
    if (!buffer) ABORT("couldn't open file '" << filePath << "'");
    return buffer->release();
}

Parser::Parser(llvm::StringRef filePath, Module& module, const CompileOptions& options)
: lexer(getFileMemoryBuffer(filePath)), currentModule(&module), currentTokenIndex(0), options(options) {
    tokenBuffer.emplace_back(lexer.nextToken());
}

Token Parser::currentToken() {
    ASSERT(currentTokenIndex < tokenBuffer.size());
    return tokenBuffer[currentTokenIndex];
}

SourceLocation Parser::getCurrentLocation() {
    return currentToken().getLocation();
}

Token Parser::lookAhead(int offset) {
    if (int(currentTokenIndex) + offset < 0) return Token(Token::None, SourceLocation());
    int count = int(currentTokenIndex) + offset - int(tokenBuffer.size()) + 1;
    while (count-- > 0) {
        tokenBuffer.emplace_back(lexer.nextToken());
    }
    return tokenBuffer[currentTokenIndex + offset];
}

Token Parser::consumeToken() {
    Token token = currentToken();
    if (++currentTokenIndex == tokenBuffer.size()) {
        tokenBuffer.emplace_back(lexer.nextToken());
    }
    return token;
}

/// Adds quotes around the string representation of the given token unless
/// it's an identifier, numeric literal, string literal, or end-of-file.
static std::string quote(Token::Kind tokenKind) {
    std::ostringstream stream;
    if (tokenKind < Token::Break) {
        stream << tokenKind;
    } else {
        stream << '\'' << tokenKind << '\'';
    }
    return stream.str();
}

static std::string formatList(llvm::ArrayRef<Token::Kind> tokens) {
    std::string result;

    for (auto& token : tokens) {
        result += quote(token);

        if (tokens.size() > 2 && &token != &tokens.back()) {
            result += ", ";
        }

        if (&token == &tokens.back() - 1) {
            if (tokens.size() == 2) {
                result += " ";
            }
            result += "or ";
        }
    }

    return result;
}

static void unexpectedToken(Token token, llvm::ArrayRef<Token::Kind> expected = {}, const char* contextInfo = nullptr) {
    if (expected.size() == 0) {
        ERROR(token.getLocation(), "unexpected " << quote(token) << (contextInfo ? " " : "") << (contextInfo ? contextInfo : ""));
    } else {
        ERROR(token.getLocation(), "expected " << formatList(expected) << (contextInfo ? " " : "") << (contextInfo ? contextInfo : "")
                                               << ", got " << quote(token));
    }
}

void Parser::expect(llvm::ArrayRef<Token::Kind> expected, const char* contextInfo) {
    if (!llvm::is_contained(expected, currentToken())) {
        unexpectedToken(currentToken(), expected, contextInfo);
    }
}

Token Parser::parse(llvm::ArrayRef<Token::Kind> expected, const char* contextInfo) {
    expect(expected, contextInfo);
    return consumeToken();
}

void Parser::checkStmtTerminatorConsistency(Token::Kind currentTerminator, llvm::function_ref<SourceLocation()> getLocation) {
    static Token::Kind previousTerminator = Token::None;
    static const char* filePath = nullptr;

    if (filePath != lexer.getFilePath()) {
        filePath = lexer.getFilePath();
        previousTerminator = Token::None;
    }

    if (previousTerminator == Token::None) {
        previousTerminator = currentTerminator;
    } else if (previousTerminator != currentTerminator) {
        WARN(getLocation(), "inconsistent statement terminator, expected " << quote(previousTerminator));
    }
}

bool isNewline(char ch) {
    return ch == '\n';
}

void Parser::parseStmtTerminator(const char* contextInfo) {
    if (getCurrentLocation().line != lookAhead(-1).getLocation().line) {
        checkStmtTerminatorConsistency(Token::Newline, [this] {
            auto source = getCurrentSource();
            auto line = lookAhead(-1).getLocation().line;
            while (--line) {
                source = source.drop_until(isNewline).drop_front();
            }
            auto column = source.find_first_of("\r\n") + 1;
            return SourceLocation(getCurrentLocation().file, lookAhead(-1).getLocation().line, static_cast<SourceLocation::IntegerType>(column));
        });
        return;
    }

    switch (currentToken()) {
        case Token::RightBrace:
            checkStmtTerminatorConsistency(Token::Newline, std::bind(&Parser::getCurrentLocation, this));
            return;
        case Token::Semicolon:
            checkStmtTerminatorConsistency(Token::Semicolon, std::bind(&Parser::getCurrentLocation, this));
            consumeToken();
            return;
        default:
            unexpectedToken(currentToken(), { Token::Newline, Token::Semicolon }, contextInfo);
    }
}

llvm::StringRef Parser::getCurrentSource() const {
    return Lexer::fileBuffers.back()->getBuffer();
}

/// argument-list ::= '(' ')' | '(' nonempty-argument-list ')'
/// nonempty-argument-list ::= argument | nonempty-argument-list ',' argument
/// argument ::= (id ':')? expr
std::vector<NamedValue> Parser::parseArgumentList() {
    parse(Token::LeftParen);
    std::vector<NamedValue> args;
    while (currentToken() != Token::RightParen) {
        std::string name;
        SourceLocation location = SourceLocation();
        if (lookAhead(1) == Token::Colon) {
            auto result = parse(Token::Identifier);
            name = std::move(result.getString());
            location = result.getLocation();
            consumeToken();
        }
        auto value = parseExpr();
        if (!location.isValid()) location = value->getLocation();
        args.push_back({ std::move(name), value, location });
        if (currentToken() != Token::RightParen) parse(Token::Comma);
    }
    consumeToken();
    return args;
}

/// var-expr ::= id
VarExpr* Parser::parseVarExpr() {
    ASSERT(currentToken().is({ Token::Identifier, Token::Init }));
    auto id = consumeToken();
    return new VarExpr(id.getString(), id.getLocation());
}

VarExpr* Parser::parseThis() {
    ASSERT(currentToken() == Token::This);
    auto expr = new VarExpr("this", getCurrentLocation());
    consumeToken();
    return expr;
}

static std::string replaceEscapeChars(llvm::StringRef literalContent, SourceLocation literalStartLocation) {
    std::string result;
    result.reserve(literalContent.size());

    for (auto it = literalContent.begin(), end = literalContent.end(); it != end; ++it) {
        if (*it == '\\') {
            ++it;
            ASSERT(it != end);
            switch (*it) {
                case '0':
                    result += '\0';
                    break;
                case 'a':
                    result += '\a';
                    break;
                case 'b':
                    result += '\b';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                case 'v':
                    result += '\v';
                    break;
                case '"':
                    result += '"';
                    break;
                case '\'':
                    result += '\'';
                    break;
                case '\\':
                    result += '\\';
                    break;
                default:
                    auto itColumn = literalStartLocation.column + 1 + (it - literalContent.begin());
                    SourceLocation itLocation(literalStartLocation.file, literalStartLocation.line, itColumn);
                    ERROR(itLocation, "unknown escape character '\\" << *it << "'");
            }
            continue;
        }
        result += *it;
    }
    return result;
}

StringLiteralExpr* Parser::parseStringLiteral() {
    ASSERT(currentToken() == Token::StringLiteral);
    auto content = replaceEscapeChars(currentToken().getString().drop_back().drop_front(), getCurrentLocation());
    auto expr = new StringLiteralExpr(std::move(content), getCurrentLocation());
    consumeToken();
    return expr;
}

CharacterLiteralExpr* Parser::parseCharacterLiteral() {
    ASSERT(currentToken() == Token::CharacterLiteral);
    auto content = replaceEscapeChars(currentToken().getString().drop_back().drop_front(), getCurrentLocation());
    if (content.size() != 1) ERROR(getCurrentLocation(), "character literal must consist of a single UTF-8 byte");
    auto expr = new CharacterLiteralExpr(content[0], getCurrentLocation());
    consumeToken();
    return expr;
}

IntLiteralExpr* Parser::parseIntLiteral() {
    ASSERT(currentToken() == Token::IntegerLiteral);
    auto expr = new IntLiteralExpr(currentToken().getIntegerValue(), getCurrentLocation());
    consumeToken();
    return expr;
}

FloatLiteralExpr* Parser::parseFloatLiteral() {
    ASSERT(currentToken() == Token::FloatLiteral);
    auto expr = new FloatLiteralExpr(currentToken().getFloatingPointValue(), getCurrentLocation());
    consumeToken();
    return expr;
}

BoolLiteralExpr* Parser::parseBoolLiteral() {
    BoolLiteralExpr* expr;
    switch (currentToken()) {
        case Token::True:
            expr = new BoolLiteralExpr(true, getCurrentLocation());
            break;
        case Token::False:
            expr = new BoolLiteralExpr(false, getCurrentLocation());
            break;
        default:
            llvm_unreachable("all cases handled");
    }
    consumeToken();
    return expr;
}

NullLiteralExpr* Parser::parseNullLiteral() {
    ASSERT(currentToken() == Token::Null);
    auto expr = new NullLiteralExpr(getCurrentLocation());
    consumeToken();
    return expr;
}

UndefinedLiteralExpr* Parser::parseUndefinedLiteral() {
    ASSERT(currentToken() == Token::Undefined);
    auto expr = new UndefinedLiteralExpr(getCurrentLocation());
    consumeToken();
    return expr;
}

/// array-literal ::= '[' expr-list ']'
ArrayLiteralExpr* Parser::parseArrayLiteral() {
    ASSERT(currentToken() == Token::LeftBracket);
    auto location = getCurrentLocation();
    consumeToken();
    auto elements = parseExprList();
    parse(Token::RightBracket);
    return new ArrayLiteralExpr(std::move(elements), location);
}

/// tuple-literal ::= '(' tuple-literal-elements ')'
/// tuple-literal-elements ::= tuple-literal-element | tuple-literal-elements ',' tuple-literal-element
/// tuple-literal-element ::= id ':' expr
TupleExpr* Parser::parseTupleLiteral() {
    ASSERT(currentToken() == Token::LeftParen);
    auto location = getCurrentLocation();
    auto elements = parseArgumentList();
    for (auto& element : elements) {
        if (element.getName().empty()) {
            if (auto* varExpr = llvm::dyn_cast<VarExpr>(element.getValue())) {
                element.setName(varExpr->getIdentifier());
            } else {
                ERROR(element.getLocation(), "tuple elements must have names");
            }
        }
    }
    return new TupleExpr(std::move(elements), location);
}

/// non-empty-type-list ::= type | type ',' non-empty-type-list
std::vector<Type> Parser::parseNonEmptyTypeList() {
    std::vector<Type> types;

    while (true) {
        types.push_back(parseType());

        if (currentToken() == Token::Comma) {
            consumeToken();
        } else {
            if (currentToken() == Token::RightShift) {
                tokenBuffer[currentTokenIndex] = Token(Token::Greater, currentToken().getLocation());
                tokenBuffer.insert(tokenBuffer.begin() + currentTokenIndex + 1, Token(Token::Greater, currentToken().getLocation().nextColumn()));
            }
            return types;
        }
    }
}

/// generic-argument-list ::= '<' non-empty-type-list '>'
std::vector<Type> Parser::parseGenericArgumentList() {
    ASSERT(currentToken() == Token::Less);
    consumeToken();
    std::vector<Type> genericArgs = parseNonEmptyTypeList();
    parse(Token::Greater);
    return genericArgs;
}

llvm::Optional<int64_t> Parser::parseArraySizeInBrackets() {
    ASSERT(currentToken() == Token::LeftBracket);
    consumeToken();
    int64_t arraySize;

    switch (currentToken()) {
        case Token::IntegerLiteral:
            arraySize = consumeToken().getIntegerValue().getExtValue();
            break;
        case Token::RightBracket:
            arraySize = ArrayType::runtimeSize;
            break;
        case Token::QuestionMark:
            consumeToken();
            arraySize = ArrayType::unknownSize;
            break;
        default:
            return llvm::None;
    }

    parse(Token::RightBracket);
    return arraySize;
}

/// simple-type ::= id | id generic-argument-list | id '[' int-literal? ']'
Type Parser::parseSimpleType(Mutability mutability) {
    auto identifier = parse(Token::Identifier);
    std::vector<Type> genericArgs;

    switch (currentToken()) {
        case Token::Less:
            genericArgs = parseGenericArgumentList();
            LLVM_FALLTHROUGH;
        default:
            return BasicType::get(identifier.getString(), std::move(genericArgs), mutability, identifier.getLocation());
        case Token::LeftBracket:
            auto bracketLocation = getCurrentLocation();
            Type elementType = BasicType::get(identifier.getString(), {}, mutability, identifier.getLocation());
            if (auto size = parseArraySizeInBrackets()) {
                return ArrayType::get(elementType, *size, mutability, bracketLocation);
            } else {
                return Type();
            }
    }
}

/// tuple-type ::= '(' tuple-type-elements ')'
/// tuple-type-elements ::= tuple-type-element | tuple-type-elements ',' tuple-type-element
/// tuple-type-element ::= type id
Type Parser::parseTupleType() {
    ASSERT(currentToken() == Token::LeftParen);
    auto location = getCurrentLocation();
    consumeToken();
    std::vector<TupleElement> elements;

    while (currentToken() != Token::RightParen) {
        auto type = parseType();
        std::string name = parse(Token::Identifier).getString();
        elements.push_back({ std::move(name), type });
        if (currentToken() != Token::RightParen) parse(Token::Comma);
    }

    consumeToken();
    return TupleType::get(std::move(elements), Mutability::Mutable, location);
}

/// function-type ::= type '(' param-types ')'
/// param-types ::= '' | non-empty-param-types
/// non-empty-param-types ::= type | type ',' non-empty-param-types
Type Parser::parseFunctionType(Type returnType) {
    parse(Token::LeftParen);
    std::vector<Type> paramTypes;

    while (currentToken() != Token::RightParen) {
        paramTypes.emplace_back(parseType());
        if (currentToken() != Token::RightParen) parse(Token::Comma);
    }

    consumeToken();
    return FunctionType::get(returnType, std::move(paramTypes), Mutability::Mutable, returnType.getLocation());
}

/// type ::= simple-type | 'const' simple-type | type '*' | type '?' | function-type | tuple-type
Type Parser::parseType() {
    Type type;
    auto location = getCurrentLocation();

    switch (currentToken()) {
        case Token::Identifier:
            type = parseSimpleType(Mutability::Mutable);
            break;
        case Token::Const:
            consumeToken();
            type = parseSimpleType(Mutability::Const);
            break;
        case Token::LeftParen:
            type = parseTupleType();
            break;
        default:
            unexpectedToken(currentToken());
    }

    while (true) {
        switch (currentToken()) {
            case Token::Star:
                type = PointerType::get(type, Mutability::Mutable, getCurrentLocation());
                consumeToken();
                break;
            case Token::QuestionMark:
                type = OptionalType::get(type, Mutability::Mutable, getCurrentLocation());
                consumeToken();
                break;
            case Token::LeftParen:
                type = parseFunctionType(type);
                break;
            case Token::LeftBracket:
                if (auto size = parseArraySizeInBrackets()) {
                    type = ArrayType::get(type, *size, type.getMutability(), getCurrentLocation());
                } else {
                    return Type();
                }
                break;
            case Token::And:
                ERROR(getCurrentLocation(),
                      "Delta doesn't have C++-style references; use pointers ('*') instead, they are non-null by default");
            default:
                return type.withLocation(location);
        }
    }
}

/// sizeof-expr ::= 'sizeof' '(' type ')'
SizeofExpr* Parser::parseSizeofExpr() {
    ASSERT(currentToken() == Token::Sizeof);
    auto location = getCurrentLocation();
    consumeToken();
    parse(Token::LeftParen);
    auto type = parseType();
    parse(Token::RightParen);
    return new SizeofExpr(type, location);
}

/// addressof-expr ::= 'addressof' '(' expr ')'
AddressofExpr* Parser::parseAddressofExpr() {
    ASSERT(currentToken() == Token::Addressof);
    auto location = getCurrentLocation();
    consumeToken();
    parse(Token::LeftParen);
    auto operand = parseExpr();
    parse(Token::RightParen);
    return new AddressofExpr(operand, location);
}

/// member-expr ::= expr '.' id
MemberExpr* Parser::parseMemberExpr(Expr* lhs) {
    auto location = getCurrentLocation();
    llvm::StringRef member;

    if (currentToken().is({ Token::Identifier, Token::Init, Token::Deinit })) {
        member = consumeToken().getString();
    } else {
        unexpectedToken(currentToken(), Token::Identifier);
    }

    return new MemberExpr(lhs, member, location);
}

/// subscript-expr ::= expr '[' expr ']'
SubscriptExpr* Parser::parseSubscript(Expr* operand) {
    ASSERT(currentToken() == Token::LeftBracket);
    auto location = getCurrentLocation();
    consumeToken();
    auto index = parseExpr();
    parse(Token::RightBracket);
    return new SubscriptExpr(operand, index, location);
}

/// unwrap-expr ::= expr '!'
UnwrapExpr* Parser::parseUnwrapExpr(Expr* operand) {
    ASSERT(currentToken() == Token::Not);
    auto location = getCurrentLocation();
    consumeToken();
    return new UnwrapExpr(operand, location);
}

/// call-expr ::= expr generic-argument-list? argument-list
CallExpr* Parser::parseCallExpr(Expr* callee) {
    std::vector<Type> genericArgs;
    if (currentToken() == Token::Less) {
        genericArgs = parseGenericArgumentList();
    }
    auto location = getCurrentLocation();
    auto args = parseArgumentList();
    return new CallExpr(callee, std::move(args), std::move(genericArgs), location);
}

LambdaExpr* Parser::parseLambdaExpr() {
    ASSERT(currentToken().is({ Token::LeftParen, Token::Identifier }));
    auto location = getCurrentLocation();
    std::vector<ParamDecl> params;

    if (currentToken() == Token::Identifier) {
        params.push_back(parseParam(true));
    } else {
        params = parseParamList(nullptr, true);
    }

    parse(Token::RightArrow);
    auto body = parseExpr();
    return new LambdaExpr(std::move(params), body, currentModule, location);
}

/// paren-expr ::= '(' expr ')'
Expr* Parser::parseParenExpr() {
    ASSERT(currentToken() == Token::LeftParen);
    consumeToken();
    auto expr = parseExpr();
    parse(Token::RightParen);
    return expr;
}

/// if-expr ::= expr '?' expr ':' expr
IfExpr* Parser::parseIfExpr(Expr* condition) {
    ASSERT(currentToken() == Token::QuestionMark);
    auto location = getCurrentLocation();
    consumeToken();
    auto thenExpr = parseExpr();
    parse(Token::Colon);
    auto elseExpr = parseExpr();
    return new IfExpr(condition, thenExpr, elseExpr, location);
}

bool Parser::shouldParseVarStmt() {
    if (currentToken() == Token::Var) return true;
    auto backtrackLocation = currentTokenIndex;
    bool result = parseType() && currentToken() == Token::Identifier;
    currentTokenIndex = backtrackLocation;
    return result;
}

bool Parser::shouldParseGenericArgumentList() {
    // Temporary hack: use spacing to determine whether to parse a generic argument list
    // of a less-than binary expression. Zero spaces on either side of '<' will cause it
    // to be interpreted as a generic argument list, for now.
    return lookAhead(0).getLocation().column + int(lookAhead(0).getString().size()) == lookAhead(1).getLocation().column ||
           lookAhead(1).getLocation().column + 1 == lookAhead(2).getLocation().column;
}

/// Returns true if a right-arrow token immediately follows the current set of parentheses.
bool Parser::arrowAfterParentheses() {
    ASSERT(currentToken() == Token::LeftParen);
    int offset = 1;

    for (int parenDepth = 1; parenDepth > 0; ++offset) {
        switch (lookAhead(offset)) {
            case Token::LeftParen:
                ++parenDepth;
                break;
            case Token::RightParen:
                --parenDepth;
                break;
            default:
                break;
        }
    }

    return lookAhead(offset) == Token::RightArrow;
}

/// postfix-expr ::= postfix-expr postfix-op | call-expr | variable-expr | string-literal |
///                  int-literal | float-literal | bool-literal | null-literal |
///                  paren-expr | array-literal | tuple-literal | subscript-expr |
///                  member-expr | unwrap-expr | lambda-expr | sizeof-expr | addressof-expr
Expr* Parser::parsePostfixExpr() {
    Expr* expr;

    switch (currentToken()) {
        case Token::Identifier:
        case Token::Init:
            switch (lookAhead(1)) {
                case Token::LeftParen:
                    expr = parseCallExpr(parseVarExpr());
                    break;
                case Token::Less:
                    if (shouldParseGenericArgumentList()) {
                        expr = parseCallExpr(parseVarExpr());
                        break;
                    }
                    LLVM_FALLTHROUGH;
                default:
                    expr = parseVarExpr();
                    break;
            }
            break;
        case Token::StringLiteral:
            expr = parseStringLiteral();
            break;
        case Token::CharacterLiteral:
            expr = parseCharacterLiteral();
            break;
        case Token::IntegerLiteral:
            expr = parseIntLiteral();
            break;
        case Token::FloatLiteral:
            expr = parseFloatLiteral();
            break;
        case Token::True:
        case Token::False:
            expr = parseBoolLiteral();
            break;
        case Token::Null:
            expr = parseNullLiteral();
            break;
        case Token::This:
            expr = parseThis();
            break;
        case Token::LeftParen:
            if (arrowAfterParentheses()) {
                expr = parseLambdaExpr();
            } else if (lookAhead(1) == Token::Identifier && lookAhead(2).is({ Token::Colon, Token::Comma })) {
                expr = parseTupleLiteral();
            } else {
                expr = parseParenExpr();
            }
            break;
        case Token::LeftBracket:
            expr = parseArrayLiteral();
            break;
        case Token::Sizeof:
            expr = parseSizeofExpr();
            break;
        case Token::Addressof:
            expr = parseAddressofExpr();
            break;
        case Token::Undefined:
            expr = parseUndefinedLiteral();
            break;
        default:
            unexpectedToken(currentToken());
            break;
    }

    while (true) {
        switch (currentToken()) {
            case Token::LeftBracket:
                expr = parseSubscript(expr);
                break;
            case Token::LeftParen:
                expr = parseCallExpr(expr);
                break;
            case Token::Dot:
                consumeToken();
                expr = parseMemberExpr(expr);
                break;
            case Token::Increment:
            case Token::Decrement:
                expr = parseIncrementOrDecrementExpr(expr);
                break;
            case Token::Not:
                expr = parseUnwrapExpr(expr);
                break;
            default:
                return expr;
        }
    }
}

/// prefix-expr ::= prefix-operator (prefix-expr | postfix-expr)
UnaryExpr* Parser::parsePrefixExpr() {
    ASSERT(isUnaryOperator(currentToken()));
    auto op = consumeToken();
    return new UnaryExpr(op.getKind(), parsePreOrPostfixExpr(), op.getLocation());
}

Expr* Parser::parsePreOrPostfixExpr() {
    return isUnaryOperator(currentToken()) ? parsePrefixExpr() : parsePostfixExpr();
}

/// inc-expr ::= expr '++'
/// dec-expr ::= expr '--'
UnaryExpr* Parser::parseIncrementOrDecrementExpr(Expr* operand) {
    auto op = parse({ Token::Increment, Token::Decrement });
    return new UnaryExpr(op.getKind(), operand, op.getLocation());
}

/// binary-expr ::= expr op expr
Expr* Parser::parseBinaryExpr(int minPrecedence) {
    auto lhs = parsePreOrPostfixExpr();

    while (((isBinaryOperator(currentToken()) || currentToken() == Token::QuestionMark) && getPrecedence(currentToken()) >= minPrecedence)) {
        if (currentToken() == Token::QuestionMark) {
            lhs = parseIfExpr(lhs);
            continue;
        }

        auto backtrackLocation = currentTokenIndex;
        auto op = consumeToken();
        auto rhs = parseBinaryExpr(getPrecedence(op) + 1);

        if (isAssignmentOperator(currentToken())) {
            currentTokenIndex = backtrackLocation;
            break;
        }

        lhs = new BinaryExpr(op.getKind(), lhs, rhs, op.getLocation());
    }

    return lhs;
}

/// expr ::= prefix-expr | postfix-expr | binary-expr | if-expr
Expr* Parser::parseExpr() {
    return parseBinaryExpr(0);
}

/// expr-list ::= '' | nonempty-expr-list
/// nonempty-expr-list ::= expr | expr ',' nonempty-expr-list
std::vector<Expr*> Parser::parseExprList() {
    std::vector<Expr*> exprs;

    if (currentToken() == Token::Semicolon || currentToken() == Token::RightBrace) {
        return exprs;
    }

    while (true) {
        exprs.emplace_back(parseExpr());
        if (currentToken() != Token::Comma) return exprs;
        consumeToken();
    }
}

/// return-stmt ::= 'return' expr ('\n' | ';')
ReturnStmt* Parser::parseReturnStmt() {
    ASSERT(currentToken() == Token::Return);
    auto location = getCurrentLocation();
    consumeToken();
    auto returnValue = currentToken().is({ Token::Semicolon, Token::RightBrace }) ? nullptr : parseExpr();
    parseStmtTerminator();
    return new ReturnStmt(returnValue, location);
}

/// var-decl ::= type-specifier id '=' initializer ('\n' | ';')
/// type-specifier ::= 'const' | 'const' type | type | 'var'
/// initializer ::= expr | 'undefined'
VarDecl* Parser::parseVarDecl(bool requireInitialValue, Decl* parent, AccessLevel accessLevel) {
    Type type;
    auto mutability = Mutability::Mutable;

    if (currentToken() == Token::Const) {
        consumeToken();
        mutability = Mutability::Const;
    }

    if (currentToken() == Token::Var) {
        consumeToken();
    } else if (lookAhead(1) != Token::Assignment) {
        type = parseType();
    }

    auto name = parse(Token::Identifier);
    return parseVarDeclAfterName(requireInitialValue, parent, accessLevel, type.withMutability(mutability), name.getString(), name.getLocation());
}

VarDecl* Parser::parseVarDeclAfterName(bool requireInitialValue, Decl* parent, AccessLevel accessLevel, Type type, llvm::StringRef name,
                                       SourceLocation location) {
    Expr* initializer = nullptr;

    if (requireInitialValue) {
        parse(Token::Assignment);
        initializer = parseExpr();
        parseStmtTerminator();
    }

    return new VarDecl(type, name, initializer, parent, accessLevel, *currentModule, location);
}

/// var-stmt ::= var-decl
VarStmt* Parser::parseVarStmt(Decl* parent) {
    return new VarStmt(parseVarDecl(true, parent, AccessLevel::None));
}

/// expr-stmt ::= binary-expr | call-expr ('\n' | ';')
ExprStmt* Parser::parseExprStmt(Expr* expr) {
    auto stmt = new ExprStmt(expr);
    parseStmtTerminator();
    return stmt;
}

/// block-or-stmt ::= '{' stmt* '}' | stmt
std::vector<Stmt*> Parser::parseBlockOrStmt(Decl* parent) {
    std::vector<Stmt*> stmts;
    if (currentToken() == Token::LeftBrace) {
        consumeToken();
        stmts = parseStmtsUntil(Token::RightBrace, parent);
        consumeToken();
    } else {
        stmts.emplace_back(parseStmt(parent));
    }
    return stmts;
}

/// defer-stmt ::= 'defer' expr ('\n' | ';')
DeferStmt* Parser::parseDeferStmt() {
    ASSERT(currentToken() == Token::Defer);
    consumeToken();
    auto stmt = new DeferStmt(parseExpr());
    parseStmtTerminator();
    return stmt;
}

/// if-stmt ::= 'if' '(' expr ')' block-or-stmt ('else' block-or-stmt)?
IfStmt* Parser::parseIfStmt(Decl* parent) {
    ASSERT(currentToken() == Token::If);
    consumeToken();
    parse(Token::LeftParen);
    auto condition = parseExpr();
    parse(Token::RightParen);
    auto thenStmts = parseBlockOrStmt(parent);
    std::vector<Stmt*> elseStmts;
    if (currentToken() == Token::Else) {
        consumeToken();
        elseStmts = parseBlockOrStmt(parent);
    }
    return new IfStmt(condition, std::move(thenStmts), std::move(elseStmts));
}

/// while-stmt ::= 'while' '(' expr ')' block-or-stmt
WhileStmt* Parser::parseWhileStmt(Decl* parent) {
    ASSERT(currentToken() == Token::While);
    consumeToken();
    parse(Token::LeftParen);
    auto condition = parseExpr();
    parse(Token::RightParen);
    auto body = parseBlockOrStmt(parent);
    return new WhileStmt(condition, std::move(body), nullptr);
}

/// for-stmt ::= 'for' '(' 'var' id 'in' expr ')' block-or-stmt
ForStmt* Parser::parseForStmt(Decl* parent) {
    ASSERT(currentToken() == Token::For);
    auto location = getCurrentLocation();
    consumeToken();
    parse(Token::LeftParen);
    auto variable = parseVarDecl(false, parent, AccessLevel::None);
    parse(Token::In);
    auto range = parseExpr();
    parse(Token::RightParen);
    auto body = parseBlockOrStmt(parent);
    return new ForStmt(variable, range, std::move(body), location);
}

/// switch-stmt ::= 'switch' '(' expr ')' '{' cases default-case? '}'
/// cases ::= case | case cases
/// case ::= 'case' expr ':' stmt+
/// default-case ::= 'default' ':' stmt+
SwitchStmt* Parser::parseSwitchStmt(Decl* parent) {
    ASSERT(currentToken() == Token::Switch);
    consumeToken();
    parse(Token::LeftParen);
    auto condition = parseExpr();
    parse(Token::RightParen);
    parse(Token::LeftBrace);
    std::vector<SwitchCase> cases;
    std::vector<Stmt*> defaultStmts;
    bool defaultSeen = false;

    while (true) {
        if (currentToken() == Token::Case) {
            consumeToken();
            auto value = parseExpr();

            VarDecl* associatedValue = nullptr;
            if (currentToken() == Token::As) {
                consumeToken();
                auto name = parse(Token::Identifier);
                associatedValue = new VarDecl(Type(), name.getString(), new UndefinedLiteralExpr(name.getLocation()), nullptr,
                                              AccessLevel::None, *currentModule, name.getLocation());
            }

            parse(Token::Colon);
            auto stmts = parseStmtsUntilOneOf(Token::Case, Token::Default, Token::RightBrace, parent);
            cases.push_back(SwitchCase(value, associatedValue, std::move(stmts)));
        } else if (currentToken() == Token::Default) {
            if (defaultSeen) {
                ERROR(getCurrentLocation(), "switch-statement may only contain one 'default' case");
            }
            consumeToken();
            parse(Token::Colon);
            defaultStmts = parseStmtsUntilOneOf(Token::Case, Token::Default, Token::RightBrace, parent);
            defaultSeen = true;
        } else {
            ERROR(getCurrentLocation(), "expected 'case' or 'default'");
        }

        if (currentToken() == Token::RightBrace) break;
    }

    consumeToken();
    return new SwitchStmt(condition, std::move(cases), std::move(defaultStmts));
}

/// break-stmt ::= 'break' ('\n' | ';')
BreakStmt* Parser::parseBreakStmt() {
    auto location = getCurrentLocation();
    consumeToken();
    parseStmtTerminator();
    return new BreakStmt(location);
}

/// continue-stmt ::= 'continue' ('\n' | ';')
ContinueStmt* Parser::parseContinueStmt() {
    auto location = getCurrentLocation();
    consumeToken();
    parseStmtTerminator();
    return new ContinueStmt(location);
}

/// stmt ::= var-stmt | return-stmt | expr-stmt | defer-stmt | if-stmt |
///          switch-stmt | while-stmt | for-stmt | break-stmt | continue-stmt
Stmt* Parser::parseStmt(Decl* parent) {
    switch (currentToken()) {
        case Token::Return:
            return parseReturnStmt();
        case Token::Defer:
            return parseDeferStmt();
        case Token::If:
            return parseIfStmt(parent);
        case Token::While:
            return parseWhileStmt(parent);
        case Token::For:
            return parseForStmt(parent);
        case Token::Switch:
            return parseSwitchStmt(parent);
        case Token::Break:
            return parseBreakStmt();
        case Token::Continue:
            return parseContinueStmt();
        case Token::Underscore: {
            consumeToken();
            parse(Token::Assignment);
            auto stmt = new ExprStmt(parseExpr());
            parseStmtTerminator();
            return stmt;
        }
        default:
            if (shouldParseVarStmt()) {
                return parseVarStmt(parent);
            }
            break;
    }

    // If we're here, the statement starts with an expression.
    auto* expr = parseExpr();

    if (!expr->isCallExpr() && !expr->isIncrementOrDecrementExpr() && !expr->isAssignment()) {
        unexpectedToken(currentToken());
    }

    return parseExprStmt(expr);
}

std::vector<Stmt*> Parser::parseStmtsUntil(Token::Kind end, Decl* parent) {
    std::vector<Stmt*> stmts;
    while (currentToken() != end) {
        stmts.emplace_back(parseStmt(parent));
    }
    return stmts;
}

std::vector<Stmt*> Parser::parseStmtsUntilOneOf(Token::Kind end1, Token::Kind end2, Token::Kind end3, Decl* parent) {
    std::vector<Stmt*> stmts;
    while (currentToken() != end1 && currentToken() != end2 && currentToken() != end3) {
        stmts.emplace_back(parseStmt(parent));
    }
    return stmts;
}

/// param-decl ::= type id
ParamDecl Parser::parseParam(bool withType) {
    auto type = withType ? parseType() : Type();
    auto name = parse(Token::Identifier);
    return ParamDecl(type, name.getString(), name.getLocation());
}

/// param-list ::= '(' params ')'
/// params ::= '' | non-empty-params
/// non-empty-params ::= param-decl | param-decl ',' non-empty-params
std::vector<ParamDecl> Parser::parseParamList(bool* isVariadic, bool withTypes) {
    parse(Token::LeftParen);
    std::vector<ParamDecl> params;
    while (currentToken() != Token::RightParen) {
        if (isVariadic && currentToken() == Token::DotDotDot) {
            consumeToken();
            *isVariadic = true;
            break;
        }
        params.emplace_back(parseParam(withTypes));
        if (currentToken() != Token::RightParen) parse(Token::Comma);
    }
    parse(Token::RightParen);
    return params;
}

void Parser::parseGenericParamList(std::vector<GenericParamDecl>& genericParams) {
    parse(Token::Less);
    while (true) {
        auto genericParamName = parse(Token::Identifier);
        genericParams.emplace_back(genericParamName.getString(), genericParamName.getLocation());

        if (currentToken() == Token::Colon) {
            consumeToken();
            genericParams.back().setConstraints(parseType());
        }

        if (currentToken() == Token::Greater) break;
        parse(Token::Comma);
    }
    parse(Token::Greater);
}

llvm::StringRef Parser::parseFunctionName(TypeDecl* receiverTypeDecl) {
    auto name = parse(Token::Identifier);

    if (name.getString() == "operator") {
        auto op = consumeToken();
        if (op == Token::LeftBracket) {
            parse(Token::RightBracket);
            return "[]";
        } else {
            if (!isOverloadable(op)) {
                unexpectedToken(op, {}, "as function name");
            }
            if (receiverTypeDecl) {
                ERROR(name.getLocation(), "operator functions other than subscript must be non-member functions");
            }
            return toString(op);
        }
    } else {
        return name.getString();
    }
}

/// function-proto ::= type id param-list
FunctionDecl* Parser::parseFunctionProto(bool isExtern, TypeDecl* receiverTypeDecl, AccessLevel accessLevel, std::vector<GenericParamDecl>* genericParams,
                                         Type returnType, llvm::StringRef name, SourceLocation location) {
    if (currentToken() == Token::Less) {
        parseGenericParamList(*genericParams);
    }

    bool isVariadic = false;
    auto params = parseParamList(isExtern ? &isVariadic : nullptr, true);
    FunctionProto proto(name, std::move(params), returnType, isVariadic, isExtern);

    if (receiverTypeDecl) {
        return new MethodDecl(std::move(proto), *receiverTypeDecl, std::vector<Type>(), accessLevel, location);
    } else {
        return new FunctionDecl(std::move(proto), std::vector<Type>(), accessLevel, *currentModule, location);
    }
}

/// function-template-proto ::= type id template-param-list param-list
/// template-param-list ::= '<' template-param-decls '>'
/// template-param-decls ::= id | id ',' template-param-decls
FunctionTemplate* Parser::parseFunctionTemplateProto(TypeDecl* receiverTypeDecl, AccessLevel accessLevel, Type type, llvm::StringRef name,
                                                     SourceLocation location) {
    std::vector<GenericParamDecl> genericParams;
    auto decl = parseFunctionProto(false, receiverTypeDecl, accessLevel, &genericParams, type, name, location);
    return new FunctionTemplate(std::move(genericParams), decl, accessLevel);
}

/// function-decl ::= function-proto '{' stmt* '}'
FunctionDecl* Parser::parseFunctionDecl(TypeDecl* receiverTypeDecl, AccessLevel accessLevel, bool requireBody, Type type,
                                        llvm::StringRef name, SourceLocation location) {
    auto decl = parseFunctionProto(false, receiverTypeDecl, accessLevel, nullptr, type, name, location);

    if (requireBody || currentToken() == Token::LeftBrace) {
        parse(Token::LeftBrace);
        decl->setBody(parseStmtsUntil(Token::RightBrace, decl));
        parse(Token::RightBrace);
    }

    if (lookAhead(-1) != Token::RightBrace) {
        parseStmtTerminator();
    }

    return decl;
}

/// function-template-decl ::= function-template-proto '{' stmt* '}'
FunctionTemplate* Parser::parseFunctionTemplate(TypeDecl* receiverTypeDecl, AccessLevel accessLevel, Type type, llvm::StringRef name,
                                                SourceLocation location) {
    auto decl = parseFunctionTemplateProto(receiverTypeDecl, accessLevel, type, name, location);
    parse(Token::LeftBrace);
    decl->getFunctionDecl()->setBody(parseStmtsUntil(Token::RightBrace, decl));
    parse(Token::RightBrace);
    return decl;
}

/// extern-function-decl ::= 'extern' function-proto ('\n' | ';')
FunctionDecl* Parser::parseExternFunctionDecl(Type type, llvm::StringRef name, SourceLocation location) {
    auto decl = parseFunctionProto(true, nullptr, AccessLevel::Default, nullptr, type, name, location);
    parseStmtTerminator();
    return decl;
}

/// init-decl ::= 'init' param-list '{' stmt* '}'
InitDecl* Parser::parseInitDecl(TypeDecl& receiverTypeDecl, AccessLevel accessLevel) {
    auto initLocation = parse(Token::Init).getLocation();
    auto params = parseParamList(nullptr, true);
    auto decl = new InitDecl(receiverTypeDecl, std::move(params), accessLevel, initLocation);
    parse(Token::LeftBrace);
    decl->setBody(parseStmtsUntil(Token::RightBrace, decl));
    parse(Token::RightBrace);
    return decl;
}

/// deinit-decl ::= 'deinit' '(' ')' '{' stmt* '}'
DeinitDecl* Parser::parseDeinitDecl(TypeDecl& receiverTypeDecl) {
    auto deinitLocation = parse(Token::Deinit).getLocation();
    parse(Token::LeftParen);
    auto expectedRParenLocation = getCurrentLocation();
    if (consumeToken() != Token::RightParen) ERROR(expectedRParenLocation, "deinitializers cannot have parameters");
    auto decl = new DeinitDecl(receiverTypeDecl, deinitLocation);
    parse(Token::LeftBrace);
    decl->setBody(parseStmtsUntil(Token::RightBrace, decl));
    parse(Token::RightBrace);
    return decl;
}

/// field-decl ::= type id ('\n' | ';')
FieldDecl Parser::parseFieldDecl(TypeDecl& typeDecl, AccessLevel accessLevel, Type type, llvm::StringRef name, SourceLocation nameLocation) {
    parseStmtTerminator();
    return FieldDecl(type, name, typeDecl, accessLevel, nameLocation);
}

/// type-template-decl ::= ('struct' | 'interface') id generic-param-list? '{' member-decl* '}'
TypeTemplate* Parser::parseTypeTemplate(AccessLevel accessLevel) {
    std::vector<GenericParamDecl> genericParams;
    auto typeDecl = parseTypeDecl(&genericParams, accessLevel);
    return new TypeTemplate(std::move(genericParams), typeDecl, accessLevel);
}

Token Parser::parseTypeHeader(std::vector<Type>& interfaces, std::vector<GenericParamDecl>* genericParams) {
    auto name = parse(Token::Identifier);

    if (currentToken() == Token::Less) {
        parseGenericParamList(*genericParams);
    }

    if (currentToken() == Token::Colon) {
        consumeToken();
        interfaces = parseNonEmptyTypeList();
    }

    return name;
}

/// type-decl ::= ('struct' | 'interface') id generic-param-list? interface-list? '{' member-decl* '}'
/// interface-list ::= ':' non-empty-type-list
/// member-decl ::= field-decl | function-decl
TypeDecl* Parser::parseTypeDecl(std::vector<GenericParamDecl>* genericParams, AccessLevel typeAccessLevel) {
    TypeTag tag;
    switch (consumeToken()) {
        case Token::Struct:
            tag = TypeTag::Struct;
            break;
        case Token::Interface:
            tag = TypeTag::Interface;
            break;
        default:
            llvm_unreachable("invalid token");
    }

    std::vector<Type> interfaces;
    auto typeName = parseTypeHeader(interfaces, genericParams);
    auto typeDecl = new TypeDecl(tag, typeName.getString(), std::vector<Type>(), std::move(interfaces), typeAccessLevel, *currentModule,
                                 typeName.getLocation());
    bool hasInitializer = false;
    parse(Token::LeftBrace);

    while (currentToken() != Token::RightBrace) {
        AccessLevel accessLevel = AccessLevel::Default;

    start:
        switch (currentToken()) {
            case Token::Private:
                if (tag == TypeTag::Interface) {
                    WARN(getCurrentLocation(), "interface members cannot be private");
                }
                if (accessLevel != AccessLevel::Default) {
                    WARN(getCurrentLocation(), "duplicate access specifier");
                }
                accessLevel = AccessLevel::Private;
                consumeToken();
                goto start;
            case Token::Init:
                typeDecl->addMethod(parseInitDecl(*typeDecl, accessLevel));
                hasInitializer = true;
                break;
            case Token::Deinit:
                if (accessLevel != AccessLevel::Default) {
                    WARN(lookAhead(-1).getLocation(), "deinitializers cannot be " << accessLevel);
                }
                typeDecl->addMethod(parseDeinitDecl(*typeDecl));
                break;
            default: {
                auto type = parseType();
                auto location = getCurrentLocation();
                auto name = parseFunctionName(&*typeDecl);
                auto requireBody = tag != TypeTag::Interface;

                switch (currentToken()) {
                    case Token::LeftParen:
                        typeDecl->addMethod(parseFunctionDecl(typeDecl, accessLevel, requireBody, type, name, location));
                        break;
                    case Token::Less:
                        typeDecl->addMethod(parseFunctionTemplate(typeDecl, accessLevel, type, name, location));
                        break;
                    default:
                        typeDecl->addField(parseFieldDecl(*typeDecl, accessLevel, type, name, location));
                        break;
                }
                break;
            }
        }
    }

    if (tag == TypeTag::Struct && !hasInitializer) {
        typeDecl->addMethod(createAutogeneratedInitializer(typeDecl));
    }

    consumeToken();
    return typeDecl;
}

InitDecl* Parser::createAutogeneratedInitializer(TypeDecl* typeDecl) const {
    auto params = map(typeDecl->getFields(),
                      [](const FieldDecl& field) { return ParamDecl(field.getType(), field.getName(), field.getLocation()); });
    auto* autogeneratedInit = new InitDecl(*typeDecl, std::move(params), typeDecl->getAccessLevel(), typeDecl->getLocation());
    auto body = map(typeDecl->getFields(), [&](const FieldDecl& field) -> Stmt* {
        auto* left = new MemberExpr(new VarExpr("this", field.getLocation()), field.getName(), field.getLocation());
        auto* right = new VarExpr(field.getName(), field.getLocation());
        return new ExprStmt(new BinaryExpr(Token::Assignment, left, right, field.getLocation()));
    });
    autogeneratedInit->setBody(std::move(body));
    return autogeneratedInit;
}

/// enum-decl ::= 'enum' id generic-param-list? interface-list? '{' enum-case-decl* '}'
/// enum-case-decl ::= 'case' id ('\n' | ';')
EnumDecl* Parser::parseEnumDecl(AccessLevel typeAccessLevel) {
    ASSERT(currentToken() == Token::Enum);
    consumeToken();

    if (lookAhead(1) == Token::Less) {
        ERROR(getCurrentLocation(), "generic enums not implemented yet");
    }

    std::vector<GenericParamDecl> genericParams;
    std::vector<Type> interfaces;
    auto name = parseTypeHeader(interfaces, &genericParams);

    parse(Token::LeftBrace);
    std::vector<EnumCase> cases;
    auto valueCounter = llvm::APSInt::get(0);

    while (currentToken() != Token::RightBrace) {
        auto caseName = parse(Token::Identifier);
        Type associatedType;

        if (currentToken() == Token::LeftParen) {
            associatedType = parseTupleType();
        }

        auto value = new IntLiteralExpr(valueCounter, caseName.getLocation());
        cases.push_back(EnumCase(caseName.getString(), value, associatedType, typeAccessLevel, caseName.getLocation()));
        if (currentToken() != Token::RightBrace) parse(Token::Comma);
        ++valueCounter;
    }

    consumeToken();
    return new EnumDecl(name.getString(), std::move(cases), typeAccessLevel, *currentModule, name.getLocation());
}

/// import-decl ::= 'import' (id | string-literal) ('\n' | ';')
ImportDecl* Parser::parseImportDecl() {
    ASSERT(currentToken() == Token::Import);
    consumeToken();

    std::string importTarget;
    auto location = getCurrentLocation();

    if (currentToken() == Token::StringLiteral) {
        importTarget = parseStringLiteral()->getValue();
    } else {
        importTarget = parse({ Token::Identifier, Token::StringLiteral }, "after 'import'").getString();
    }

    parseStmtTerminator("after 'import' declaration");
    return new ImportDecl(std::move(importTarget), *currentModule, location);
}

void Parser::parseIfdefBody(std::vector<Decl*>* activeDecls) {
    if (currentToken() == Token::HashIf) {
        parseIfdef(activeDecls);
    } else {
        if (activeDecls) {
            activeDecls->emplace_back(parseTopLevelDecl(true));
        } else {
            parseTopLevelDecl(false);
        }
    }
}

void Parser::parseIfdef(std::vector<Decl*>* activeDecls) {
    ASSERT(currentToken() == Token::HashIf);
    consumeToken();
    bool negate = currentToken() == Token::Not;
    if (negate) consumeToken();
    auto identifier = parse(Token::Identifier);

    bool condition = false;
    if (identifier.getString() == "hasInclude") {
        parse(Token::LeftParen);
        auto header = parse(Token::StringLiteral);
        parse(Token::RightParen);

        for (llvm::StringRef path : llvm::concat<const std::string>(options.importSearchPaths, options.frameworkSearchPaths)) {
            auto headerPath = path + "/" + header.getString().drop_back().drop_front();
            if (llvm::sys::fs::exists(headerPath) && !llvm::sys::fs::is_directory(headerPath)) {
                condition = true;
                break;
            }
        }
    } else {
        condition = llvm::is_contained(options.defines, identifier.getString());
    }

    if (negate) condition = !condition;

    while (!currentToken().is({ Token::HashElse, Token::HashEndif })) {
        parseIfdefBody(condition ? activeDecls : nullptr);
    }

    if (currentToken() == Token::HashElse) {
        consumeToken();
        while (currentToken() != Token::HashEndif) {
            parseIfdefBody(condition ? nullptr : activeDecls);
        }
    }

    consumeToken();
}

/// top-level-decl ::= function-decl | extern-function-decl | type-decl | enum-decl | import-decl | var-decl
Decl* Parser::parseTopLevelDecl(bool addToSymbolTable) {
    AccessLevel accessLevel = AccessLevel::Default;
    Decl* decl;

start:
    switch (currentToken()) {
        case Token::Private:
            if (accessLevel != AccessLevel::Default) WARN(getCurrentLocation(), "duplicate access specifier");
            accessLevel = AccessLevel::Private;
            consumeToken();
            goto start;
        case Token::Extern:
            if (accessLevel != AccessLevel::Default) {
                WARN(lookAhead(-1).getLocation(), "extern functions cannot have access specifiers");
            }
            consumeToken();
            return parseTopLevelFunctionOrVariable(true, addToSymbolTable, accessLevel);
        case Token::Struct:
        case Token::Interface:
            if (lookAhead(2) == Token::Less) {
                decl = parseTypeTemplate(accessLevel);
                if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<TypeTemplate>(*decl));
            } else {
                decl = parseTypeDecl(nullptr, accessLevel);
                if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<TypeDecl>(*decl));
            }
            break;
        case Token::Enum:
            decl = parseEnumDecl(accessLevel);
            if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<EnumDecl>(*decl));
            break;
        case Token::Var:
        case Token::Const:
            // Determine if this is a constant declaration or if the const is part of a type.
            if (currentToken() == Token::Const && lookAhead(2) != Token::Assignment) {
                return parseTopLevelFunctionOrVariable(false, addToSymbolTable, accessLevel);
            }
            decl = parseVarDecl(true, nullptr, accessLevel);
            if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<VarDecl>(*decl), true);
            break;
        case Token::Import:
            if (accessLevel != AccessLevel::Default) {
                WARN(lookAhead(-1).getLocation(), "imports cannot have access specifiers");
            }
            return parseImportDecl();
        default:
            return parseTopLevelFunctionOrVariable(false, addToSymbolTable, accessLevel);
    }

    return decl;
}

Decl* Parser::parseTopLevelFunctionOrVariable(bool isExtern, bool addToSymbolTable, AccessLevel accessLevel) {
    Decl* decl;
    auto type = parseType();
    auto location = getCurrentLocation();
    auto name = parseFunctionName(nullptr);

    switch (currentToken()) {
        case Token::LeftParen:
            if (isExtern) {
                decl = parseExternFunctionDecl(type, name, location);
            } else {
                decl = parseFunctionDecl(nullptr, accessLevel, false, type, name, location);
            }
            if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<FunctionDecl>(*decl));
            break;
        case Token::Less:
            decl = parseFunctionTemplate(nullptr, accessLevel, type, name, location);
            if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<FunctionTemplate>(*decl));
            break;
        default:
            decl = parseVarDeclAfterName(true, nullptr, accessLevel, type, name, location);
            if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<VarDecl>(*decl), true);
            break;
    }

    return decl;
}

void Parser::parse() {
    std::vector<Decl*> topLevelDecls;
    SourceFile sourceFile(lexer.getFilePath());

    while (currentToken() != Token::None) {
        if (currentToken() == Token::HashIf) {
            parseIfdef(&topLevelDecls);
        } else {
            topLevelDecls.emplace_back(parseTopLevelDecl(true));
        }
    }

    sourceFile.setDecls(std::move(topLevelDecls));
    currentModule->addSourceFile(std::move(sourceFile));
}
