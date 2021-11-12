#pragma once

#include "TypeScript/DocSetManager.h"
#include "Meta/MetaEntities.h"
#include <Meta/TypeFactory.h>
#include <sstream>
#include <string>
#include <unordered_set>

namespace TypeScript {
class JSExportDefinitionWriter : ::Meta::MetaVisitor {
public:
  JSExportDefinitionWriter(std::pair<clang::Module*, std::vector<::Meta::Meta*> >& module, ::Meta::TypeFactory& typeFactory, std::string docSetPath)
  : _module(module)
  , _typeFactory(typeFactory)
  , _docSet(docSetPath)
  {
  }

  std::string write();
  
  void writeJSExport(std::string filename, ::Meta::Meta* meta, std::string frameworkName);

  virtual void visit(::Meta::InterfaceMeta* meta) override;
  
  virtual void visit(::Meta::ProtocolMeta* meta) override;
  
  virtual void visit(::Meta::CategoryMeta* meta) override;
  
  virtual void visit(::Meta::FunctionMeta* meta) override;
  
  virtual void visit(::Meta::StructMeta* meta) override;
  
  virtual void visit(::Meta::UnionMeta* meta) override;
  
  virtual void visit(::Meta::EnumMeta* meta) override;
  
  virtual void visit(::Meta::VarMeta* meta) override;
  
  virtual void visit(::Meta::MethodMeta* meta) override;
  
  virtual void visit(::Meta::PropertyMeta* meta) override;
  
  virtual void visit(::Meta::EnumConstantMeta* meta) override;

  static std::unordered_set<std::string> hiddenClasses;
  static std::string outputJSEFolder;
  static std::string outputVueFolder;

  std::pair<clang::Module*, std::vector<::Meta::Meta*> >& _module;
  ::Meta::TypeFactory& _typeFactory;
  DocSetManager _docSet;

private:
  template <class Member>
  using CompoundMemberMap = std::map<std::string, std::pair<::Meta::BaseClassMeta*, Member*> >;
  
  static std::string writeProperty(::Meta::PropertyMeta* meta, ::Meta::BaseClassMeta* owner);
  void writeProperty(::Meta::PropertyMeta* meta, ::Meta::BaseClassMeta* owner, ::Meta::InterfaceMeta* target, CompoundMemberMap<::Meta::PropertyMeta> compoundProperties);
  void writeClass(::Meta::InterfaceMeta* meta, CompoundMemberMap<::Meta::MethodMeta>* staticMethods, CompoundMemberMap<::Meta::MethodMeta>* instanceMethods);
  void writeProto(std::string protocolName, ::Meta::InterfaceMeta* meta);
  void writeProto(::Meta::ProtocolMeta* meta);
  void writeCreate(::Meta::MethodMeta* method, ::Meta::BaseClassMeta* owner, bool isStatic = false);
  void writeMethodImpl(::Meta::MethodMeta* method, ::Meta::BaseClassMeta* owner, bool isStatic = false);

  void writeExtension(std::string protocolName, ::Meta::InterfaceMeta* meta, CompoundMemberMap<::Meta::MethodMeta>* staticMethods, CompoundMemberMap<::Meta::MethodMeta>* instanceMethods);

  static void getInheritedMembersRecursive(::Meta::InterfaceMeta* interface,
                                           CompoundMemberMap<::Meta::MethodMeta>* staticMethods,
                                           CompoundMemberMap<::Meta::MethodMeta>* instanceMethods,
                                           CompoundMemberMap<::Meta::PropertyMeta>* staticProperties,
                                           CompoundMemberMap<::Meta::PropertyMeta>* instanceProperties);
  
  static void getProtocolMembersRecursive(::Meta::ProtocolMeta* protocol,
                                          CompoundMemberMap<::Meta::MethodMeta>* staticMethods,
                                          CompoundMemberMap<::Meta::MethodMeta>* instanceMethods,
                                          CompoundMemberMap<::Meta::PropertyMeta>* staticProperties,
                                          CompoundMemberMap<::Meta::PropertyMeta>* instanceProperties,
                                          std::unordered_set<::Meta::ProtocolMeta*>& visitedProtocols);
  
  static std::string getMethodReturnType(::Meta::MethodMeta* meta, ::Meta::BaseClassMeta* owner, size_t numArgs, const bool skipGenerics = false);
  static std::string writeSubclass(::Meta::InterfaceMeta* meta);
  static std::string writeMethod(::Meta::MethodMeta* meta, ::Meta::BaseClassMeta* owner, std::string keyword = "", std::string metaJsName = "");
  static std::string writeMethod(CompoundMemberMap<::Meta::MethodMeta>::value_type& method, ::Meta::BaseClassMeta* owner,
                                 const std::unordered_set<::Meta::ProtocolMeta*>& protocols, std::string keyword = "", std::string metaJsName = "");
  
  std::unordered_set<std::string> _importedModules;
  std::ostringstream _buffer;
};
}
