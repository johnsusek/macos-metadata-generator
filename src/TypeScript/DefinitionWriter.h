#pragma once

#include "DocSetManager.h"
#include "Meta/MetaEntities.h"
#include <Meta/TypeFactory.h>
#include <sstream>
#include <string>
#include <unordered_set>

namespace TypeScript {
class DefinitionWriter : Meta::MetaVisitor {
public:
    DefinitionWriter(std::pair<clang::Module*, std::vector<Meta::Meta*> >& module, Meta::TypeFactory& typeFactory, std::string docSetPath)
        : _module(module)
        , _typeFactory(typeFactory)
        , _docSet(docSetPath)
    {
    }

    std::string write();
    
    static std::string writeNamespaces(bool writeToClasses = false);

    static std::string writeExports();

    static bool applyManualChanges;

    static bool hasClosedGenerics(const Meta::Type& type);
  
    virtual void visit(Meta::InterfaceMeta* meta) override;

    virtual void visit(Meta::ProtocolMeta* meta) override;

    virtual void visit(Meta::CategoryMeta* meta) override;

    virtual void visit(Meta::FunctionMeta* meta) override;

    virtual void visit(Meta::StructMeta* meta) override;

    virtual void visit(Meta::UnionMeta* meta) override;

    virtual void visit(Meta::EnumMeta* meta) override;

    virtual void visit(Meta::VarMeta* meta) override;

    virtual void visit(Meta::MethodMeta* meta) override;

    virtual void visit(Meta::PropertyMeta* meta) override;

    virtual void visit(Meta::EnumConstantMeta* meta) override;

private:
    template <class Member>
    using CompoundMemberMap = std::map<std::string, std::pair<Meta::BaseClassMeta*, Member*> >;

    std::string writeMembers(const std::vector<Meta::RecordField>& fields, std::vector<TSComment> fieldsComments);
    void writeProperty(Meta::PropertyMeta* meta, Meta::BaseClassMeta* owner, Meta::InterfaceMeta* target, CompoundMemberMap<Meta::PropertyMeta> compoundProperties);

    static void getInheritedMembersRecursive(Meta::InterfaceMeta* interface,
        CompoundMemberMap<Meta::MethodMeta>* staticMethods,
        CompoundMemberMap<Meta::MethodMeta>* instanceMethods,
        CompoundMemberMap<Meta::PropertyMeta>* staticProperties,
        CompoundMemberMap<Meta::PropertyMeta>* instanceProperties);

    static void getProtocolMembersRecursive(Meta::ProtocolMeta* protocol,
        CompoundMemberMap<Meta::MethodMeta>* staticMethods,
        CompoundMemberMap<Meta::MethodMeta>* instanceMethods,
        CompoundMemberMap<Meta::PropertyMeta>* staticProperties,
        CompoundMemberMap<Meta::PropertyMeta>* instanceProperties,
        std::unordered_set<Meta::ProtocolMeta*>& visitedProtocols);

    static std::string writeMethod(Meta::MethodMeta* meta, Meta::BaseClassMeta* owner, bool canUseThisType = false);
    static std::string writeMethod(CompoundMemberMap<Meta::MethodMeta>::value_type& method, Meta::BaseClassMeta* owner,
        const std::unordered_set<Meta::ProtocolMeta*>& protocols, bool canUseThisType = false);
    static std::string writeProperty(Meta::PropertyMeta* meta, Meta::BaseClassMeta* owner, bool optOutTypeChecking);
    static std::string writeFunctionProto(const std::vector<Meta::Type*>& signature);
    static std::string tsifyType(const Meta::Type& type, const bool isParam = false);
    static std::string computeMethodReturnType(const Meta::Type* retType, const Meta::BaseClassMeta* owner, bool canUseThisType = false);
    std::string getTypeArgumentsStringOrEmpty(const clang::ObjCObjectType* objectType);
    static std::string formatType(const Meta::Type& type, const clang::QualType pointerType, const bool ignorePointerType = false);
    static std::string getFunctionProto(const std::vector<Meta::Type*>& signature, const clang::QualType qualType);
    static std::string formatTypeAnonymous(const Meta::Type& type, const clang::QualType pointerType);
    static std::string formatTypeInterface(const Meta::Type& type, const clang::QualType pointerType);
    static std::string getTypeString(clang::ASTContext &Ctx, clang::Decl::ObjCDeclQualifier Quals, clang::QualType qualType, const Meta::Type& type, const bool isFuncParam);
    static bool getTypeOptional(clang::ASTContext &Ctx, clang::Decl::ObjCDeclQualifier Quals, clang::QualType qualType, const Meta::Type& type, const bool isFuncParam);
    static std::string formatTypeId(const Meta::IdType& idType, const clang::QualType pointerType, const bool ignorePointerType);
    static std::string formatTypePointer(const Meta::PointerType& pointerType, const clang::QualType pointerQualType, const bool ignorePointerType);
    static std::string getInstanceParamsStr(Meta::MethodMeta* method, Meta::BaseClassMeta* owner);
    static void populateStructsMeta();
    static std::string jsifyTypeName(const std::string& jsName);

    std::pair<clang::Module*, std::vector<Meta::Meta*> >& _module;
    Meta::TypeFactory& _typeFactory;
    DocSetManager _docSet;
    std::unordered_set<std::string> _importedModules;
    std::ostringstream _buffer;
};
}
