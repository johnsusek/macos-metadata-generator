#pragma once

#include "TypeScript/DocSetManager.h"
#include "Meta/MetaEntities.h"
#include "Meta/Utils.h"
#include "Meta/NameRetrieverVisitor.h"
#include <Meta/TypeFactory.h>
#include <sstream>
#include <string>
#include <unordered_set>

namespace TypeScript {
class JSExportFormatter {
public:
  enum ParamCallType {
    Definition = 0, // initWithFrame(_: CGRect)
    Implementation, // initWithFrame(_ frame: CGRect)
    Call // initWithFrame(frame)
  };
  
  static JSExportFormatter current;
  static std::unordered_set<std::string> nonNullable;
  static void stripModifiersFromPointerType(std::string& name);
  std::vector<std::string> split2(std::string const &input);
  std::string nameForJSExport(const std::string& jsName);
  std::string swiftifyReference(const ::Meta::Meta& meta);
  std::string formatTypeId(const ::Meta::IdType& idType, const clang::QualType pointerType, const bool ignorePointerType = false);
  std::string formatTypePointer(const ::Meta::PointerType& pointerType, const clang::QualType pointerQualType, const bool ignorePointerType = false);
  std::string formatTypeInterface(const ::Meta::Type& type, const clang::QualType pointerType, const bool ignorePointerType = false);
  std::string formatTypeAnonymous(const ::Meta::Type& type, const clang::QualType pointerType);
  std::string formatType(const ::Meta::Type& type, const clang::QualType pointerType, const bool ignorePointerType = false);
  std::string getFunctionProto(const std::vector<::Meta::Type*>& signature, const clang::QualType qualType);
  std::string getFunctionProtoCall(std::string paramName, const std::vector<::Meta::Type*>& signature, const clang::QualType qualType);
  std::string sanitizeIdentifierForSwift(const std::string& identifierName);
  std::string getMethodParams(MethodMeta* meta, BaseClassMeta* owner, ParamCallType callType = ParamCallType::Definition);
  std::string getNullabilitySymbol(PropertyMeta* meta, BaseClassMeta* owner);
  std::string getTypeNullability(clang::ParmVarDecl* decl, MethodMeta* meta);
  std::string getTypeNullability(MethodMeta* method, BaseClassMeta* owner);
  std::string bitwiseView(std::string& name);

private:
};
}
