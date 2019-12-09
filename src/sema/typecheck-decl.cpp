#include "typecheck.h"
#pragma warning(push, 0)
#include <llvm/ADT/SmallPtrSet.h>
#pragma warning(pop)
#include "c-import.h"
#include "../ast/module.h"

using namespace delta;

void Typechecker::typecheckType(Type type, AccessLevel userAccessLevel) {
    switch (type.getKind()) {
        case TypeKind::BasicType: {
            if (type.isBuiltinType()) {
                validateGenericArgCount(0, type.getGenericArgs(), type.getName(), type.getLocation());
                break;
            }

            auto* basicType = llvm::cast<BasicType>(type.getBase());

            for (auto genericArg : basicType->getGenericArgs()) {
                typecheckType(genericArg, userAccessLevel);
            }

            auto decls = findDecls(basicType->getQualifiedName());
            Decl* decl;

            if (decls.empty()) {
                auto decls = findDecls(basicType->getName());

                if (decls.empty()) {
                    ERROR(type.getLocation(), "unknown type '" << type << "'");
                }

                ASSERT(decls.size() == 1);
                decl = decls[0];
                auto typeTemplate = llvm::cast<TypeTemplate>(decl)->instantiate(basicType->getGenericArgs());
                getCurrentModule()->addToSymbolTable(*typeTemplate);
                typecheckTypeDecl(*typeTemplate);
            } else {
                ASSERT(decls.size() == 1);
                decl = decls[0];

                switch (decl->getKind()) {
                    case DeclKind::TypeDecl:
                    case DeclKind::EnumDecl:
                        break;
                    case DeclKind::TypeTemplate:
                        validateGenericArgCount(llvm::cast<TypeTemplate>(decl)->getGenericParams().size(), basicType->getGenericArgs(),
                                                basicType->getName(), type.getLocation());
                        break;
                    default:
                        ERROR(type.getLocation(), "'" << type << "' is not a type");
                }
            }

            checkHasAccess(*decl, type.getLocation(), userAccessLevel);
            break;
        }
        case TypeKind::ArrayType:
            typecheckType(type.getElementType(), userAccessLevel);
            break;
        case TypeKind::TupleType:
            for (auto& element : type.getTupleElements()) {
                typecheckType(element.type, userAccessLevel);
            }
            break;
        case TypeKind::FunctionType:
            for (auto paramType : type.getParamTypes()) {
                typecheckType(paramType, userAccessLevel);
            }
            typecheckType(type.getReturnType(), userAccessLevel);
            break;
        case TypeKind::PointerType: {
            if (type.getPointee().isArrayWithRuntimeSize()) {
                auto qualifiedTypeName = getQualifiedTypeName("ArrayRef", type.getPointee().getElementType());
                if (findDecls(qualifiedTypeName).empty()) {
                    auto* arrayRef = llvm::cast<TypeTemplate>(findDecl("ArrayRef", SourceLocation()));
                    auto* instantiation = arrayRef->instantiate({ type.getPointee().getElementType() });
                    getCurrentModule()->addToSymbolTable(*instantiation);
                    declsToTypecheck.push_back(instantiation);
                }
            } else {
                typecheckType(type.getPointee(), userAccessLevel);
            }
            break;
        }
        case TypeKind::OptionalType:
            typecheckType(type.getWrappedType(), userAccessLevel);
            break;
    }
}

void Typechecker::typecheckParamDecl(ParamDecl& decl, AccessLevel userAccessLevel) {
    if (getCurrentModule()->getSymbolTable().containsInCurrentScope(decl.getName())) {
        ERROR(decl.getLocation(), "redefinition of '" << decl.getName() << "'");
    }

    typecheckType(decl.getType(), userAccessLevel);
    getCurrentModule()->getSymbolTable().add(decl.getName(), &decl);
}

static bool allPathsReturn(llvm::ArrayRef<Stmt*> block) {
    if (block.empty()) return false;

    switch (block.back()->getKind()) {
        case StmtKind::ReturnStmt:
            return true;
        case StmtKind::IfStmt: {
            auto& ifStmt = llvm::cast<IfStmt>(*block.back());
            return allPathsReturn(ifStmt.getThenBody()) && allPathsReturn(ifStmt.getElseBody());
        }
        case StmtKind::SwitchStmt: {
            auto& switchStmt = llvm::cast<SwitchStmt>(*block.back());
            return llvm::all_of(switchStmt.getCases(), [](auto& c) { return allPathsReturn(c.getStmts()); }) &&
                   allPathsReturn(switchStmt.getDefaultStmts());
        }
        default:
            return false;
    }
}

void Typechecker::typecheckGenericParamDecls(llvm::ArrayRef<GenericParamDecl> genericParams, AccessLevel userAccessLevel) {
    for (auto& genericParam : genericParams) {
        if (getCurrentModule()->getSymbolTable().contains(genericParam.getName())) {
            ERROR(genericParam.getLocation(), "redefinition of '" << genericParam.getName() << "'");
        }

        for (Type constraint : genericParam.getConstraints()) {
            typecheckType(constraint, userAccessLevel);

            if (!constraint.getDecl()->isInterface()) {
                ERROR(constraint.getLocation(), "only interface types can be used as generic constraints");
            }
        }
    }
}

void Typechecker::typecheckParams(llvm::MutableArrayRef<ParamDecl> params, AccessLevel userAccessLevel) {
    for (auto& param : params) {
        typecheckParamDecl(param, userAccessLevel);
    }
}

void Typechecker::typecheckFunctionDecl(FunctionDecl& decl) {
    if (decl.isTypechecked()) return;
    if (decl.isExtern()) return; // TODO: Typecheck parameters and return type of extern functions.

    TypeDecl* receiverTypeDecl = decl.getTypeDecl();

    getCurrentModule()->getSymbolTable().pushScope(&decl);
    SAVE_STATE(currentFunction);
    currentFunction = &decl;

    typecheckParams(decl.getParams(), decl.getAccessLevel());

    if (decl.isLambda()) {
        ASSERT(decl.getBody().size() == 1);
        decl.getProto().setReturnType(typecheckExpr(*llvm::cast<ReturnStmt>(decl.getBody().front())->getReturnValue()));
    }

    if (!decl.isInitDecl() && !decl.isDeinitDecl()) {
        typecheckType(decl.getReturnType(), decl.getAccessLevel());
    }

    if (!decl.isExtern()) {
        SAVE_STATE(functionReturnType);
        functionReturnType = decl.getReturnType();

        llvm::SmallPtrSet<FieldDecl*, 32> initializedFields;
        SAVE_STATE(onAssign);
        onAssign = [&](Expr& lhs) { initializedFields.insert(lhs.getFieldDecl()); };

        if (receiverTypeDecl) {
            Type thisType = receiverTypeDecl->getTypeForPassing();
            auto* varDecl = new VarDecl(thisType, "this", nullptr, &decl, AccessLevel::None, *getCurrentModule(), decl.getLocation());
            getCurrentModule()->addToSymbolTable(varDecl);
        }

        bool delegatedInit = false;

        if (decl.hasBody()) {
            for (auto& stmt : decl.getBody()) {
                typecheckStmt(stmt);

                if (!decl.isInitDecl()) continue;

                if (auto* exprStmt = llvm::dyn_cast<ExprStmt>(stmt)) {
                    if (auto* callExpr = llvm::dyn_cast<CallExpr>(&exprStmt->getExpr())) {
                        if (auto* initDecl = llvm::dyn_cast_or_null<InitDecl>(callExpr->getCalleeDecl())) {
                            if (initDecl->getTypeDecl() == receiverTypeDecl) {
                                delegatedInit = true;
                            }
                        }
                    }
                }
            }
        }

        if (decl.isInitDecl() && !delegatedInit) {
            for (auto& field : decl.getTypeDecl()->getFields()) {
                if (initializedFields.count(&field) == 0) {
                    WARN(decl.getLocation(), "initializer doesn't initialize member variable '" << field.getName() << "'");
                }
            }
        }
    }

    getCurrentModule()->getSymbolTable().popScope();

    if ((!receiverTypeDecl || !receiverTypeDecl->isInterface()) && !decl.getReturnType().isVoid() && !allPathsReturn(decl.getBody())) {
        ERROR(decl.getLocation(), "'" << decl.getName() << "' is missing a return statement");
    }

    decl.setTypechecked(true);
}

void Typechecker::typecheckFunctionTemplate(FunctionTemplate& decl) {
    typecheckGenericParamDecls(decl.getGenericParams(), decl.getAccessLevel());
}

void Typechecker::typecheckTypeDecl(TypeDecl& decl) {
    for (Type interface : decl.getInterfaces()) {
        typecheckType(interface, decl.getAccessLevel());
        auto* interfaceDecl = interface.getDecl();

        if (!interfaceDecl->isInterface()) {
            ERROR(interface.getLocation(), "'" << interface << "' is not an interface");
        }

        std::string errorReason;
        if (!providesInterfaceRequirements(decl, *interfaceDecl, &errorReason)) {
            ERROR(decl.getLocation(), "'" << decl.getName() << "' " << errorReason << " required by interface '" << interfaceDecl->getName() << "'");
        }

        for (auto& method : interfaceDecl->getMethods()) {
            auto& methodDecl = llvm::cast<MethodDecl>(*method);

            if (methodDecl.hasBody()) {
                auto copy = methodDecl.instantiate({ { "This", decl.getType() } }, decl);
                getCurrentModule()->addToSymbolTable(*copy);
                decl.addMethod(copy);
            }
        }
    }

    TypeDecl* realDecl;
    TypeDecl* thisTypeResolved;

    if (decl.isInterface()) {
        thisTypeResolved = llvm::cast<TypeDecl>(decl.instantiate({ { "This", decl.getType() } }, {}));
        realDecl = thisTypeResolved;
    } else {
        realDecl = &decl;
    }

    for (auto& fieldDecl : realDecl->getFields()) {
        typecheckFieldDecl(fieldDecl);
    }

    for (auto& memberDecl : realDecl->getMemberDecls()) {
        typecheckMemberDecl(*memberDecl);
    }
}

void Typechecker::typecheckTypeTemplate(TypeTemplate& decl) {
    typecheckGenericParamDecls(decl.getGenericParams(), decl.getAccessLevel());
}

void Typechecker::typecheckEnumDecl(EnumDecl& decl) {
    std::vector<const EnumCase*> cases = map(decl.getCases(), [](const EnumCase& c) { return &c; });
    std::sort(cases.begin(), cases.end(), [](auto* a, auto* b) { return a->getName() < b->getName(); });
    auto it = std::adjacent_find(cases.begin(), cases.end(), [](auto* a, auto* b) { return a->getName() == b->getName(); });

    if (it != cases.end()) {
        ERROR((*it)->getLocation(), "duplicate enum case '" << (*it)->getName() << "'");
    }

    for (auto& enumCase : decl.getCases()) {
        typecheckExpr(*enumCase.getValue());
    }
}

void Typechecker::typecheckVarDecl(VarDecl& decl, bool isGlobal) {
    if (!isGlobal && getCurrentModule()->getSymbolTable().contains(decl.getName())) {
        ERROR(decl.getLocation(), "redefinition of '" << decl.getName() << "'");
    }
    if (isGlobal && decl.getInitializer()->isUndefinedLiteralExpr()) {
        ERROR(decl.getLocation(), "global variables cannot be uninitialized");
    }

    Type declaredType = decl.getType();
    Type initializerType = typecheckExpr(*decl.getInitializer());

    if (declaredType) {
        bool isLocalVariable = decl.getParent() && decl.getParent()->isFunctionDecl();
        typecheckType(declaredType, isLocalVariable ? AccessLevel::None : decl.getAccessLevel());

        if (!convert(decl.getInitializer(), declaredType)) {
            const char* hint = "";

            if (initializerType.isNull()) {
                ASSERT(!declaredType.isOptionalType());
                hint = " (add '?' to the type to make it nullable)";
            }

            ERROR(decl.getInitializer()->getLocation(),
                  "cannot initialize variable of type '" << declaredType << "' with '" << initializerType << "'" << hint);
        }
    } else {
        if (initializerType.isNull()) {
            ERROR(decl.getLocation(), "couldn't infer type of '" << decl.getName() << "', add a type annotation");
        }

        decl.setType(initializerType.withMutability(decl.getType().getMutability()));
    }

    if (!isGlobal) {
        getCurrentModule()->addToSymbolTable(decl, false);
    }

    if (!decl.getType().isImplicitlyCopyable()) {
        decl.getInitializer()->setMoved(true);
    }
}

void Typechecker::typecheckFieldDecl(FieldDecl& decl) {
    typecheckType(decl.getType(), std::min(decl.getAccessLevel(), decl.getParent()->getAccessLevel()));
}

void Typechecker::typecheckImportDecl(ImportDecl& decl, const PackageManifest* manifest) {
    if (importDeltaModule(currentSourceFile, manifest, decl.getTarget())) {
        return;
    }

    if (!importCHeader(*currentSourceFile, decl.getTarget(), options)) {
        ERROR(decl.getLocation(), "couldn't find module or C header '" << decl.getTarget() << "'");
    }
}

void Typechecker::typecheckTopLevelDecl(Decl& decl, const PackageManifest* manifest) {
    switch (decl.getKind()) {
        case DeclKind::ParamDecl:
            llvm_unreachable("no top-level parameter declarations");
        case DeclKind::FunctionDecl:
            typecheckFunctionDecl(llvm::cast<FunctionDecl>(decl));
            break;
        case DeclKind::MethodDecl:
            llvm_unreachable("no top-level method declarations");
        case DeclKind::GenericParamDecl:
            llvm_unreachable("no top-level parameter declarations");
        case DeclKind::InitDecl:
            llvm_unreachable("no top-level initializer declarations");
        case DeclKind::DeinitDecl:
            llvm_unreachable("no top-level deinitializer declarations");
        case DeclKind::FunctionTemplate:
            typecheckFunctionTemplate(llvm::cast<FunctionTemplate>(decl));
            break;
        case DeclKind::TypeDecl:
            typecheckTypeDecl(llvm::cast<TypeDecl>(decl));
            break;
        case DeclKind::TypeTemplate:
            typecheckTypeTemplate(llvm::cast<TypeTemplate>(decl));
            break;
        case DeclKind::EnumDecl:
            typecheckEnumDecl(llvm::cast<EnumDecl>(decl));
            break;
        case DeclKind::EnumCase:
            llvm_unreachable("no top-level enum case declarations");
        case DeclKind::VarDecl:
            typecheckVarDecl(llvm::cast<VarDecl>(decl), true);
            break;
        case DeclKind::FieldDecl:
            llvm_unreachable("no top-level field declarations");
        case DeclKind::ImportDecl:
            typecheckImportDecl(llvm::cast<ImportDecl>(decl), manifest);
            break;
    }
}

void Typechecker::typecheckMemberDecl(Decl& decl) {
    switch (decl.getKind()) {
        case DeclKind::MethodDecl:
        case DeclKind::InitDecl:
        case DeclKind::DeinitDecl:
            typecheckFunctionDecl(llvm::cast<MethodDecl>(decl));
            break;
        case DeclKind::FieldDecl:
            typecheckFieldDecl(llvm::cast<FieldDecl>(decl));
            break;
        case DeclKind::FunctionTemplate:
            typecheckFunctionTemplate(llvm::cast<FunctionTemplate>(decl));
            break;
        default:
            llvm_unreachable("invalid member declaration kind");
    }
}
